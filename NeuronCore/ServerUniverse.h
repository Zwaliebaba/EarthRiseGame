#pragma once
// ServerUniverse — authoritative simulation state for ERServer (§9).
//
// M1a scope: one mobile Base per player, integrated by the shared MovementSystem
// at the fixed 30 Hz step, with a full snapshot built per tick (interest =
// everything until M4 sector subscriptions land). M3 area D adds navigation:
// the cooked jump-beacon graph is loaded as Structure entities and bases warp /
// jump across it server-authoritatively (NavigationSystem). Platform-independent
// so the whole server→client path runs in the loopback integration test.
//
// Component type IDs (Components.h) must be bound via NEURON_DEFINE_COMPONENT in
// exactly one TU per executable: SimComponents.cpp for ERServer/ERHeadless, and
// the test TU for the test runner.

#include "Combat.h"
#include "CombatData.h"
#include "Command.h"
#include "Components.h"
#include "Economy.h"
#include "Fleet.h"
#include "Interest.h"
#include "Movement.h"
#include "Navigation.h"
#include "ShapeCatalog.h"
#include "Snapshot.h"
#include "SnapshotScheduler.h"
#include "WarmRestart.h" // M5 area F — PersistState capture/restore (portable, testrunner-verified)
#include "Ecs.h"
#include "UniverseData.h"
#include "UniversePos.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Neuron::Sim
{

class ServerUniverse
{
public:
    // seedDemoContent: spawn the M2 scenery + a small dev seed (working beacons,
    // a resource node, a starter fleet) near spawn so the M3 entities are visible
    // in the live client. Tests pass false for a pristine world.
    explicit ServerUniverse(bool seedDemoContent = true)
    {
        m_world.RegisterComponent<Transform>();
        m_world.RegisterComponent<Velocity>();
        m_world.RegisterComponent<NetId>();
        m_world.RegisterComponent<BaseTag>();
        m_world.RegisterComponent<ShipTag>();
        m_world.RegisterComponent<Health>();
        m_world.RegisterComponent<ShapeId>();
        m_world.RegisterComponent<Fuel>();
        m_world.RegisterComponent<NavState>();
        m_world.RegisterComponent<BeaconTag>();
        m_world.RegisterComponent<OwnerId>();
        m_world.RegisterComponent<ResourceNodeTag>();
        m_world.RegisterComponent<Cargo>();
        m_world.RegisterComponent<Storage>();
        m_world.RegisterComponent<BuildQueue>();
        m_world.RegisterComponent<FleetMember>();
        m_world.RegisterComponent<Sensor>();
        m_world.RegisterComponent<HarvestOrder>();
        m_world.RegisterComponent<FleetOrder>();
        m_world.RegisterComponent<Weapon>();
        m_world.RegisterComponent<NpcAi>();
        // M6 combat model (areas B–G).
        m_world.RegisterComponent<DefenseLayers>();
        m_world.RegisterComponent<ResistProfile>();
        m_world.RegisterComponent<Fitting>();
        m_world.RegisterComponent<EwarStatus>();
        m_world.RegisterComponent<Projectile>();
        m_world.RegisterComponent<LootContainer>();
        m_world.RegisterComponent<BaseCombat>();
        m_world.RegisterComponent<HullInfo>();
        if (seedDemoContent) {
            SpawnScenery();
            SpawnDemoSeed();
        }
    }

    // Shape used for a player's mobile home base (a station hull reads as a base).
    static uint16_t BaseShapeId()
    {
        const uint16_t id = ShapeIdByName("Outpost01");
        return id != kInvalidShapeId ? id : 0; // 0 is always a valid catalog entry
    }

    // Shape used for a jump beacon (a jumpgate mesh).
    static uint16_t BeaconShapeId()
    {
        const uint16_t id = ShapeIdByName("Jumpgate01");
        return id != kInvalidShapeId ? id : 0;
    }

    // Placeholder hull for a freshly built ship (M3; fitting/roles are M6).
    static uint16_t ShipShapeId()
    {
        const uint16_t id = ShapeIdByName("HullShuttle");
        return id != kInvalidShapeId ? id : 0;
    }

    // Spawn a mobile base for a player. Returns the assigned network id. Bases
    // carry Fuel + NavState so they can warp/jump (the "mobile home" relocates).
    uint32_t SpawnBase(Neuron::Universe::UniversePos start, DirectX::XMFLOAT3 vel)
    {
        const uint32_t netId = m_nextNetId++;
        auto e = m_world.CreateEntity();
        auto& t = m_world.AddComponent<Transform>(e);
        t.pos = start;
        auto& v = m_world.AddComponent<Velocity>(e);
        v.metresPerSecond = ClampSpeed(vel, kMaxBaseSpeed);
        m_world.AddComponent<NetId>(e).value = netId;
        m_world.AddComponent<BaseTag>(e);
        m_world.AddComponent<ShapeId>(e) = { BaseShapeId(), EntityKind::Base };
        m_world.AddComponent<Fuel>(e)    = { m_nav.baseFuelMax, m_nav.baseFuelMax };
        m_world.AddComponent<NavState>(e);
        m_world.AddComponent<OwnerId>(e).player   = netId;    // a player ≈ their base
        m_world.AddComponent<Storage>(e).capacity = m_economy.storageCapacity;
        m_world.AddComponent<BuildQueue>(e);
        m_world.AddComponent<Sensor>(e).range     = m_economy.sensorRangeBase;
        m_world.AddComponent<BaseCombat>(e);                  // disable-not-destroy state (§13.1)
        // The capital fit = fire-support + light defensive weapons (§13.1 first pass);
        // installs layered HP + resists + the fitting grid + the synced Health mirror.
        InstallFit(e, "base-firesupport");
        m_netIdToEntity[netId] = e;
        return netId;
    }

    // Spawn a static catalog prop (scenery: stations, asteroids, jumpgates, ...).
    // Replicated like any entity (gets a NetId); no Velocity, so it stays put.
    uint32_t SpawnProp(uint16_t shapeId, Neuron::Universe::UniversePos pos)
    {
        const ShapeDef* def = ShapeById(shapeId);
        const EntityKind kind = def ? KindForCategory(def->category) : EntityKind::Unknown;
        const uint32_t netId = m_nextNetId++;
        auto e = m_world.CreateEntity();
        m_world.AddComponent<Transform>(e).pos = pos;
        m_world.AddComponent<NetId>(e).value = netId;
        m_world.AddComponent<ShapeId>(e) = { shapeId, kind };
        m_netIdToEntity[netId] = e;
        return netId;
    }

    // Spawn a harvestable resource node (area C spawns these from the dataset's
    // resource fields; the dev seed places one near spawn). Rendered as an asteroid.
    uint32_t SpawnResourceNode(ResourceType type, float yield, Neuron::Universe::UniversePos pos)
    {
        const uint16_t shape = (type == ResourceType::Ice) ? ShapeIdByName("Asteroid04Ice")
                             : (type == ResourceType::Gas) ? ShapeIdByName("Asteroid06Lava")
                                                           : ShapeIdByName("Asteroid01Rock");
        const uint32_t netId = m_nextNetId++;
        auto e = m_world.CreateEntity();
        m_world.AddComponent<Transform>(e).pos = pos;
        m_world.AddComponent<NetId>(e).value = netId;
        const uint16_t sid = (shape != kInvalidShapeId) ? shape : uint16_t{ 0 };
        m_world.AddComponent<ShapeId>(e) = { sid, EntityKind::ResourceNode };
        auto& rn = m_world.AddComponent<ResourceNodeTag>(e);
        rn.type      = static_cast<uint8_t>(type);
        rn.remaining = yield;
        m_netIdToEntity[netId] = e;
        return netId;
    }

    // Apply a validated move intent to a player's base (server-authoritative).
    void SetBaseVelocity(uint32_t netId, DirectX::XMFLOAT3 vel)
    {
        if (auto* v = m_world.GetComponent<Velocity>(EntityOf(netId)))
            v->metresPerSecond = ClampSpeed(vel, kMaxBaseSpeed);
    }

    // --- navigation: warp + jump-beacon network (§13.12) ---------------------

    // Load the cooked universe layout (NeuronTools/datacook output): store the nav
    // tuning and spawn the jump beacons as replicated Structure entities, indexed
    // by name for jump validation. Resource fields stay in the dataset for area C.
    void LoadUniverse(const UniverseDataset& data)
    {
        m_universe = data;
        m_nav      = data.nav;
        m_economy  = data.economy;
        for (uint16_t bi = 0; bi < static_cast<uint16_t>(m_universe.beacons.size()); ++bi) {
            const BeaconDef& b = m_universe.beacons[bi];
            const uint32_t netId = m_nextNetId++;
            auto e = m_world.CreateEntity();
            m_world.AddComponent<Transform>(e).pos = b.pos;
            m_world.AddComponent<NetId>(e).value   = netId;
            m_world.AddComponent<ShapeId>(e)       = { BeaconShapeId(), EntityKind::Structure };
            m_world.AddComponent<BeaconTag>(e).beaconIndex = bi;
            m_netIdToEntity[netId] = e;
            m_beaconEntity[bi]     = netId;
            m_beaconName[b.name]   = bi;
        }
        SpawnFieldNodes();
    }

    // Decode a cooked blob (NeuronTools/datacook output) and load it. The runtime
    // entry point: ERServer supplies the bytes (shipped/embedded) at startup.
    bool LoadUniverseFromCooked(std::span<const uint8_t> blob)
    {
        auto ds = DecodeUniverseDataset(blob);
        if (!ds) return false;
        LoadUniverse(*ds);
        return true;
    }

    // --- combat catalog (M6 area A) -----------------------------------------

    // Replace the active combat catalog (hulls/modules/fits). This is the §26
    // live-ops HOT-RELOAD hook: balance/pacing can change without a code change or a
    // redeploy. Only AFFECTS units spawned afterward (existing fits are immutable).
    void LoadCombat(CombatCatalog c) { m_combat = std::move(c); }

    // Decode + load a cooked combat catalog (NeuronTools combat datacook output).
    bool LoadCombatFromCooked(std::span<const uint8_t> blob)
    {
        auto c = DecodeCombatCatalog(blob);
        if (!c) return false;
        m_combat = std::move(*c);
        return true;
    }
    [[nodiscard]] const CombatCatalog& Combat() const noexcept { return m_combat; }

    // Combat tunables (the §19 open questions; area-M balance gates sweep them).
    void SetProjectileSubSteps(int n) noexcept { m_projectileSubSteps = n < 1 ? 1 : n; }
    [[nodiscard]] int  ProjectileSubSteps() const noexcept { return m_projectileSubSteps; }
    void SetBaseRetreatHullFrac(float f) noexcept { m_baseRetreatHullFrac = f; }
    void SetBaseRetreatSeconds(float s) noexcept { m_baseRetreatSeconds = s; }
    // Disable the combat systems (weapons/EWAR/projectiles/regen/retreat) — used by the
    // M4 replication load harness, which packs many mutually-hostile bases into one
    // sector purely to stress the snapshot pipeline, not to fight.
    void SetCombatEnabled(bool on) noexcept { m_combatEnabled = on; }

    // Begin a server-validated warp to a universe position. False if the unit
    // can't warp now (unknown id / already travelling).
    bool BeginWarpTo(uint32_t netId, Neuron::Universe::UniversePos dest)
    {
        NavState* nav = NavOf(netId);
        if (!nav || nav->phase != NavPhase::Idle) return false;
        StopMotion(netId);
        Sim::BeginWarp(*nav, dest, IsBase(netId) ? m_nav.warpSpeedBase : m_nav.warpSpeedShip,
                       m_nav.warpAlignSeconds);
        OnTravelStart(dest);
        PrefetchTravelInterest(netId, dest);
        return true;
    }

    // Begin a server-validated jump to a *named* destination beacon. The source is
    // the nearest beacon within range; validates link, fuel, and busy state. On
    // success, fuel is consumed and the spool-up (vulnerability window) starts.
    JumpReject BeginJumpTo(uint32_t netId, std::string_view destBeacon)
    {
        NavState*  nav  = NavOf(netId);
        Fuel*      fuel = FuelOf(netId);
        Transform* tr   = m_world.GetComponent<Transform>(EntityOf(netId));
        if (!nav || !fuel || !tr) return JumpReject::NotAtBeacon;

        auto dit = m_beaconName.find(std::string(destBeacon));
        if (dit == m_beaconName.end()) return JumpReject::NotLinked;
        const uint16_t destIdx = dit->second;

        const int srcIdx = NearestBeaconWithin(tr->pos, m_nav.beaconUseRange);
        if (srcIdx < 0) return JumpReject::NotAtBeacon;

        const auto& links = m_universe.beacons[static_cast<size_t>(srcIdx)].links;
        if (std::find(links.begin(), links.end(), m_universe.beacons[destIdx].name) == links.end())
            return JumpReject::NotLinked;

        const bool  base = IsBase(netId);
        const float cost = base ? m_nav.jumpFuelBase : m_nav.jumpFuelShip;
        const JumpReject ready = CheckJumpReady(*nav, *fuel, cost);
        if (ready != JumpReject::Accepted) return ready;

        fuel->current -= cost;
        StopMotion(netId);
        Sim::BeginJump(*nav, m_universe.beacons[destIdx].pos, destIdx,
                       base ? m_nav.jumpSpoolBase : m_nav.jumpSpoolShip);
        OnTravelStart(m_universe.beacons[destIdx].pos);
        PrefetchTravelInterest(netId, m_universe.beacons[destIdx].pos);
        return JumpReject::Accepted;
    }

    // Interdict a unit (tackle/warp-disruptor): drops it out of an in-progress
    // warp or cancels a spooling jump on the next tick (full EWAR is M6).
    void Interdict(uint32_t netId) { if (NavState* nav = NavOf(netId)) nav->interdicted = true; }

    // Accessors (diagnostics / tests).
    [[nodiscard]] NavState* NavOf(uint32_t netId)  { return m_world.GetComponent<NavState>(EntityOf(netId)); }
    [[nodiscard]] Fuel*     FuelOf(uint32_t netId) { return m_world.GetComponent<Fuel>(EntityOf(netId)); }
    [[nodiscard]] const UniverseDataset& Universe() const noexcept { return m_universe; }
    [[nodiscard]] uint32_t BeaconNetId(std::string_view name) const
    {
        auto it = m_beaconName.find(std::string(name));
        return it == m_beaconName.end() ? 0u : m_beaconEntity.at(it->second);
    }
    [[nodiscard]] Neuron::Universe::SectorId LastTravelSector() const noexcept { return m_lastTravelSector; }

    // --- fleet & economy (§13.1, §13.4) --------------------------------------

    // Spawn a ship owned by 'player', up to the data-driven fleet cap. Returns the
    // new net id, or 0 if the player is already at cap.
    uint32_t SpawnFleetShip(uint32_t player, uint16_t shapeId, Neuron::Universe::UniversePos pos)
    {
        return SpawnFleetShipFit(player, shapeId, pos, kDefaultShipFit);
    }

    // Spawn a ship owned by 'player' with a NAMED catalog fit (area A/B/F). The fit
    // installs the layered HP + resists + the fitting grid + the derived weapon
    // summary + the synced Health mirror (combat-balance.md §6). Returns 0 at cap or
    // if the fit/hull is unknown (server-authoritative — no over-budget fit, §8.4).
    uint32_t SpawnFleetShipFit(uint32_t player, uint16_t shapeId, Neuron::Universe::UniversePos pos,
                               std::string_view fitName)
    {
        if (OwnedShipCount(player) >= m_economy.fleetCap) return 0;
        const ShapeDef* def = ShapeById(shapeId);
        const uint32_t netId = m_nextNetId++;
        auto e = m_world.CreateEntity();
        m_world.AddComponent<Transform>(e).pos = pos;
        m_world.AddComponent<Velocity>(e);
        m_world.AddComponent<NetId>(e).value = netId;
        m_world.AddComponent<ShipTag>(e).shipType = def ? static_cast<uint8_t>(def->category) : 0;
        m_world.AddComponent<ShapeId>(e) = { shapeId, EntityKind::Ship };
        m_world.AddComponent<OwnerId>(e).player   = player;
        m_world.AddComponent<Cargo>(e).capacity   = m_economy.cargoCapacity;
        m_world.AddComponent<Fuel>(e)  = { m_nav.shipFuelMax, m_nav.shipFuelMax };
        m_world.AddComponent<NavState>(e);
        m_world.AddComponent<Sensor>(e).range     = m_economy.sensorRangeShip;
        m_world.AddComponent<FleetMember>(e);
        m_world.AddComponent<FleetOrder>(e);
        if (!InstallFit(e, fitName)) InstallFit(e, kDefaultShipFit); // resilient to a bad name
        m_netIdToEntity[netId] = e;
        return netId;
    }

    // Number of ships a player currently owns (the base is not a ship).
    [[nodiscard]] uint16_t OwnedShipCount(uint32_t player)
    {
        uint16_t n = 0;
        m_world.ForEach<OwnerId, ShipTag>([&](OwnerId& o, ShipTag&) { if (o.player == player) ++n; });
        return n;
    }

    // Enqueue the basic-ship build at a player's base (server-validated entry point).
    bool EnqueueBuild(uint32_t baseNetId)
    {
        auto* q = m_world.GetComponent<BuildQueue>(EntityOf(baseNetId));
        if (!q || q->active) return false;
        q->active = true; q->paid = false; q->progress = 0.0f; q->recipe = m_economy.buildShipType;
        return true;
    }

    // Drain (and clear) the net ids of ships finished since the last call — the
    // "build complete" feedback hook (client SFX is M2; not persisted until M5).
    [[nodiscard]] std::vector<uint32_t> DrainBuildCompleted() { return std::exchange(m_buildCompleted, {}); }

    // Accessors (tests / diagnostics).
    [[nodiscard]] Cargo*      CargoOf(uint32_t netId)      { return m_world.GetComponent<Cargo>(EntityOf(netId)); }
    [[nodiscard]] Storage*    StorageOf(uint32_t netId)    { return m_world.GetComponent<Storage>(EntityOf(netId)); }
    [[nodiscard]] BuildQueue* BuildQueueOf(uint32_t netId) { return m_world.GetComponent<BuildQueue>(EntityOf(netId)); }
    [[nodiscard]] const EconomyTuning& Economy() const noexcept { return m_economy; }

    // Order a harvester to auto-mine a node and return to its base (server entry
    // point; area B routes a Harvest command here). The home base is the ship
    // owner's base (player ≈ base net id). False if ship / node / base invalid.
    bool OrderHarvest(uint32_t shipNetId, uint32_t nodeNetId)
    {
        const auto shipE = EntityOf(shipNetId);
        OwnerId* owner = m_world.GetComponent<OwnerId>(shipE);
        if (!owner || !m_world.HasComponent<Cargo>(shipE)) return false;
        if (!m_world.HasComponent<ResourceNodeTag>(EntityOf(nodeNetId))) return false;
        const uint32_t baseNetId = owner->player; // player ≈ their base net id
        if (!m_world.HasComponent<Storage>(EntityOf(baseNetId))) return false;
        HarvestOrder* ord = m_world.GetComponent<HarvestOrder>(shipE);
        if (!ord) ord = &m_world.AddComponent<HarvestOrder>(shipE);
        ord->phase     = HarvestPhase::ToNode;
        ord->nodeNetId = nodeNetId;
        ord->baseNetId = baseNetId;
        return true;
    }

    [[nodiscard]] HarvestOrder*     HarvestOrderOf(uint32_t netId) { return m_world.GetComponent<HarvestOrder>(EntityOf(netId)); }
    [[nodiscard]] ResourceNodeTag*  ResourceNodeOf(uint32_t netId) { return m_world.GetComponent<ResourceNodeTag>(EntityOf(netId)); }

    // --- fleet command — RTS intents (§8.4 / §23.4; area B) ------------------

    // Apply a validated fleet command for 'player'. Every targeted unit is
    // ownership-checked (you only command your own); intents with a bad target are
    // rejected. Returns the number of units the command actually affected (0 = the
    // whole command was rejected). Server-authoritative — never trusts the client.
    uint32_t ApplyFleetCommand(uint32_t player, const FleetCommand& cmd)
    {
        uint32_t affected = 0;
        for (const uint32_t unitNet : cmd.units) {
            const auto unit = EntityOf(unitNet);
            const OwnerId* owner = m_world.GetComponent<OwnerId>(unit);
            if (!owner || owner->player != player) continue;  // ownership check (§8.4)
            if (ApplyIntentToUnit(unitNet, cmd)) ++affected;
        }
        return affected;
    }

    [[nodiscard]] FleetOrder* FleetOrderOf(uint32_t netId) { return m_world.GetComponent<FleetOrder>(EntityOf(netId)); }
    [[nodiscard]] Health*     HealthOf(uint32_t netId)     { return m_world.GetComponent<Health>(EntityOf(netId)); }
    [[nodiscard]] NpcAi*      NpcAiOf(uint32_t netId)      { return m_world.GetComponent<NpcAi>(EntityOf(netId)); }
    [[nodiscard]] Weapon*     WeaponOf(uint32_t netId)     { return m_world.GetComponent<Weapon>(EntityOf(netId)); }

    // --- combat accessors (M6 areas B–G; tests / diagnostics) ----------------
    [[nodiscard]] DefenseLayers* DefenseOf(uint32_t netId) { return m_world.GetComponent<DefenseLayers>(EntityOf(netId)); }
    [[nodiscard]] ResistProfile* ResistOf(uint32_t netId)  { return m_world.GetComponent<ResistProfile>(EntityOf(netId)); }
    [[nodiscard]] Fitting*       FittingOf(uint32_t netId) { return m_world.GetComponent<Fitting>(EntityOf(netId)); }
    [[nodiscard]] EwarStatus*    EwarOf(uint32_t netId)    { return m_world.GetComponent<EwarStatus>(EntityOf(netId)); }
    [[nodiscard]] HullInfo*      HullInfoOf(uint32_t netId){ return m_world.GetComponent<HullInfo>(EntityOf(netId)); }
    [[nodiscard]] BaseCombat*    BaseCombatOf(uint32_t netId){ return m_world.GetComponent<BaseCombat>(EntityOf(netId)); }
    [[nodiscard]] LootContainer* LootOf(uint32_t netId)    { return m_world.GetComponent<LootContainer>(EntityOf(netId)); }

    // Net ids of all live loot containers (overview / claim UI / tests).
    [[nodiscard]] std::vector<uint32_t> LootContainerIds()
    {
        std::vector<uint32_t> out;
        m_world.ForEach<LootContainer, NetId>([&](LootContainer&, NetId& id) { out.push_back(id.value); });
        return out;
    }
    // Net ids of a unit's projectiles in flight (R16 cap evidence / tests).
    [[nodiscard]] size_t ProjectileCount()
    {
        size_t n = 0;
        m_world.ForEach<Projectile>([&](Projectile&) { ++n; });
        return n;
    }

    // Recover a loot container into a claimer's cargo (an economy event → outbox, §15).
    // Transfers up to the claimer's free cargo space; removes the container; emits one
    // LootClaim event (zero-loss / idempotent reconciliation is the M5 outbox's job).
    bool ClaimLoot(uint32_t claimerNetId, uint32_t containerNetId)
    {
        LootContainer* lc = m_world.GetComponent<LootContainer>(EntityOf(containerNetId));
        Cargo*         cg = m_world.GetComponent<Cargo>(EntityOf(claimerNetId));
        if (!lc || !cg) return false;
        int32_t value = 0;
        for (int i = 0; i < kResourceSlots; ++i) {
            float used = 0.0f; for (int j = 0; j < kResourceSlots; ++j) used += cg->amount[j];
            const float space = cg->capacity - used;
            const float take  = std::min(lc->items[i], std::max(0.0f, space));
            cg->amount[i] += take;
            lc->items[i]  -= take;
            value += static_cast<int32_t>(take);
        }
        DestroyUnit(containerNetId);
        m_econEvents.push_back({ EconEventType::LootClaim, containerNetId, claimerNetId, value, m_tick });
        return true;
    }

    // --- economy events + killmails (area G; ERServer feeds the M5 outbox) ----
    enum class EconEventType : uint8_t { LootDrop = 0, LootClaim = 1, Killmail = 2, CargoLost = 3 };
    struct EconEvent { EconEventType type; uint32_t aNetId; uint32_t bNetId; int32_t value; uint32_t tick; };
    struct Killmail  { uint32_t victimNetId; uint32_t killerNetId; uint8_t victimKind; int32_t value; uint32_t tick; };

    // Drain the loot/kill/cargo-loss economy events since the last call (→ M5 write-
    // through outbox, zero-loss, §15) and the killmail log (→ KillmailLog + §24 notify).
    [[nodiscard]] std::vector<EconEvent> DrainEconEvents() { return std::exchange(m_econEvents, {}); }
    [[nodiscard]] std::vector<Killmail>  DrainKillmails()  { return std::exchange(m_killmails, {}); }
    // Drain base low-hull → retreat alerts (area H client SFX/UI hook, §11.3).
    [[nodiscard]] std::vector<uint32_t>  DrainLowHullAlerts() { return std::exchange(m_lowHullAlerts, {}); }

    // --- basic PvE NPC site (§13.7; area F) ----------------------------------

    // Spawn a hand-placed guardian site: 'count' NPC units in a deterministic ring
    // of 'radius' around 'center', each defending the site. Returns a site id; the
    // site is "cleared" once every guardian is destroyed (DrainClearedSites). NPCs
    // are server ECS entities (OwnerId.player == 0), distinct from ERHeadless bots.
    uint16_t SpawnNpcSite(Neuron::Universe::UniversePos center, int count, float radius = 1200.0f,
                          std::string_view fitName = kNpcFit)
    {
        const uint16_t siteId = m_nextSiteId++;
        int& alive = m_siteAlive[siteId];
        alive = 0;
        const int n = std::clamp(count, 1, 32);
        for (int i = 0; i < n; ++i) {
            const double ang = (2.0 * 3.14159265358979323846 * static_cast<double>(i)) / static_cast<double>(n);
            const int64_t ox = static_cast<int64_t>(std::llround(std::cos(ang) * static_cast<double>(radius)));
            const int64_t oz = static_cast<int64_t>(std::llround(std::sin(ang) * static_cast<double>(radius)));
            const Neuron::Universe::UniversePos pos{ center.x + ox, center.y, center.z + oz };
            SpawnNpcGuardian(pos, siteId, fitName);
            ++alive;
        }
        return siteId;
    }

    [[nodiscard]] int  NpcSiteAlive(uint16_t siteId) const
    {
        auto it = m_siteAlive.find(siteId);
        return it == m_siteAlive.end() ? 0 : it->second;
    }
    [[nodiscard]] bool IsNpcSiteCleared(uint16_t siteId) const { return NpcSiteAlive(siteId) == 0; }
    // Net ids of a site's surviving guardians (overview / targeting / tests).
    [[nodiscard]] std::vector<uint32_t> NpcSiteMembers(uint16_t siteId)
    {
        std::vector<uint32_t> out;
        m_world.ForEach<NpcAi, NetId>([&](NpcAi& ai, NetId& id) { if (ai.siteId == siteId) out.push_back(id.value); });
        return out;
    }
    // Drain (and clear) the ids of sites cleared since the last call — fires once
    // per site (the "site cleared" feedback hook; client SFX/notification is M7).
    [[nodiscard]] std::vector<uint16_t> DrainClearedSites() { return std::exchange(m_clearedSites, {}); }

    // --- eXplore — sensor range & fog of war (§13.0; area E) ------------------

    // The set of net ids a player can currently see: their own entities, anything
    // within sensor range of one of their units/base, the beacon graph (map
    // infrastructure is always known, §13.12), and anything they have scanned.
    // M3 keeps this interest-light — a visibility filter on the full snapshot path
    // (full sector-subscription interest is M4).
    [[nodiscard]] std::unordered_set<uint32_t> DetectedSet(uint32_t player)
    {
        // Gather the player's sensor sources (pos + range).
        struct Eye { Neuron::Universe::UniversePos pos; float range; };
        std::vector<Eye> eyes;
        m_world.ForEach<OwnerId, Sensor, Transform>([&](OwnerId& o, Sensor& s, Transform& t) {
            if (o.player == player) eyes.push_back({ t.pos, s.range });
        });

        std::unordered_set<uint32_t> seen;
        const auto& revealed = m_revealed[player];
        m_world.ForEach<NetId, Transform, ShapeId>([&](NetId& id, Transform& t, ShapeId& sh) {
            // Own entities + always-known beacons are unconditionally visible.
            const OwnerId* o = m_world.GetComponent<OwnerId>(EntityOf(id.value));
            if ((o && o->player == player) || sh.kind == EntityKind::Structure) {
                seen.insert(id.value);
                return;
            }
            if (revealed.count(id.value)) { seen.insert(id.value); return; }
            for (const Eye& e : eyes)
                if (SensorDetect(e.pos, t.pos, e.range)) { seen.insert(id.value); return; }
        });
        return seen;
    }

    // Progress a timed scan of 'targetNetId' by 'player'; once dwell ≥ scanSeconds
    // the target is permanently revealed to that player (feeds warp/jump target +
    // the site, §13.7). Returns true when the scan completes this call.
    bool OrderScan(uint32_t player, uint32_t targetNetId, float dt, float scanSeconds = kScanSeconds)
    {
        if (!m_world.IsAlive(EntityOf(targetNetId))) return false;
        if (m_revealed[player].count(targetNetId)) return true; // already known
        float& dwell = m_scanDwell[(static_cast<uint64_t>(player) << 32) | targetNetId];
        dwell += dt;
        if (dwell >= scanSeconds) { m_revealed[player].insert(targetNetId); return true; }
        return false;
    }

    [[nodiscard]] bool IsRevealedTo(uint32_t player, uint32_t targetNetId)
    {
        return m_revealed[player].count(targetNetId) != 0;
    }

    // Full snapshot filtered to what 'player' can detect (area E fog). Own/beacon/
    // scanned/in-range entities only; the M5 per-client snapshot will build on this.
    [[nodiscard]] Snapshot BuildSnapshotFor(uint32_t player)
    {
        const auto seen = DetectedSet(player);
        Snapshot snap;
        snap.tick = m_tick;
        m_world.ForEach<NetId, Transform, ShapeId>([&](NetId& id, Transform& t, ShapeId& s) {
            if (!seen.count(id.value)) return;
            snap.entities.push_back(MakeSnapshotEntity(id, t, s));
        });
        return snap;
    }

    // Remove a player's base from the universe (on disconnect/timeout). Returns
    // true if a base for that net id existed.
    bool DespawnBase(uint32_t netId)
    {
        auto it = m_netIdToEntity.find(netId);
        if (it == m_netIdToEntity.end()) return false;
        m_world.DestroyEntity(it->second);
        m_netIdToEntity.erase(it);
        m_interest.Remove(netId); // drop residency (area A); TombstonesFor evicts it per client (area D)
        m_repl.Remove(netId);     // drop replication version (area B)
        return true;
    }

    // Advance the simulation one fixed step. Order is fixed for determinism (§7.2):
    // AI sets NPC orders + target priority; EWAR/logistics apply debuffs + remote rep;
    // fleet orders steer (web-slowed); weapons fire (hitscan + projectiles); projectiles
    // sub-step and resolve; shields regen + EWAR decays; the base disable-not-destroy
    // check runs; then movement/navigation/economy integrate; finally the Health mirror
    // is synced and replication/interest are stamped.
    void Step(float dtSeconds)
    {
        AiSystem(dtSeconds);
        if (m_combatEnabled) {        // the M4 replication load harness runs combat-free
            EwarLogiSystem(dtSeconds);    // EWAR debuffs + remote rep (area E)
            FleetOrderSystem(dtSeconds);  // steer (web-slowed)
            CombatSystem(dtSeconds);      // weapons fire → hitscan damage / projectile spawn (area D)
            ProjectileSystem(dtSeconds);  // advance projectiles, sub-step intercept → damage (area D)
            RegenSystem(dtSeconds);       // shield regen + EWAR-timer decay (area C/E)
            BaseRetreatSystem();          // disable-not-destroy (area G)
        } else {
            FleetOrderSystem(dtSeconds);  // steering still runs (movement is not combat)
        }
        MovementSystem(m_world, dtSeconds);
        NavigationSystem(m_world, m_nav, dtSeconds);
        HarvestSystem(dtSeconds);
        BuildSystem(dtSeconds);
        SyncHealthMirror();           // Health mirror ← DefenseLayers (areas B/C)
        m_simTime += static_cast<double>(dtSeconds);
        ++m_tick;
        StampReplication();
        UpdateInterest();
    }

    // Stamp every replicated entity's version against this tick's final state (M4
    // area B, §8.4): a version advances iff the entity's replicated fields (the
    // App. A snapshot projection) changed, so an idle entity holds its version and
    // costs ≈0 downstream. Pure bookkeeping — touches only the version side table,
    // never sim state, so SimHash is unchanged. The per-client diff reads it.
    void StampReplication()
    {
        m_world.ForEach<NetId, Transform, ShapeId>([&](NetId& id, Transform& t, ShapeId& s) {
            const auto e = EntityOf(id.value);
            Neuron::Sim::ReplFields f;
            f.x = t.pos.x; f.y = t.pos.y; f.z = t.pos.z;
            f.lox = t.localOffset.x; f.loy = t.localOffset.y; f.loz = t.localOffset.z;
            const Health* h = m_world.GetComponent<Health>(e);
            f.hp = h ? h->hp : 1000; // mirror MakeSnapshotEntity's default
            const OwnerId* o = m_world.GetComponent<OwnerId>(e);
            f.ownerPlayer = o ? o->player : 0;
            f.shapeId = s.value;
            f.kind    = static_cast<uint8_t>(s.kind);
            m_repl.Stamp(id.value, f);
        });
    }

    // --- per-client replication baseline (area B, §8.4 / §8.3) ---------------

    // Current server-side replication version of an entity (0 = never stamped).
    [[nodiscard]] uint32_t ReplVersion(uint32_t netId) const { return m_repl.Version(netId); }

    // The netIds a client still lacks: in its interest set (area A) and with a
    // server version beyond its acked baseline. This is the raw area-B diff — the
    // area-E scheduler orders it by priority and fits it to the MTU budget. Drawn
    // from VisibleTo so it is sorted/deduped (deterministic). Ack-advanced, never
    // last-sent, so a dropped snapshot re-deltas from the still-current baseline.
    [[nodiscard]] std::vector<uint32_t> ChangedFor(uint32_t clientId)
    {
        std::vector<uint32_t> visible;
        m_interest.VisibleTo(clientId, visible);
        Neuron::Sim::ClientBaseline& base = m_baselines[clientId];
        std::vector<uint32_t> changed;
        for (uint32_t netId : visible)
            if (base.Needs(netId, m_repl.Version(netId))) changed.push_back(netId);
        return changed;
    }

    // Record that 'clientId' was sent these netIds (at their current versions) in
    // the snapshot for 'tick' (§8.3), so acking 'tick' advances its baseline. The
    // area-E/F encoder calls this as it seals each client's snapshot.
    void RecordSent(uint32_t clientId, uint32_t tick, const std::vector<uint32_t>& netIds)
    {
        Neuron::Sim::ClientBaseline::SentList sent;
        sent.reserve(netIds.size());
        for (uint32_t n : netIds) sent.emplace_back(n, m_repl.Version(n));
        m_baselines[clientId].RecordSent(tick, sent);
    }

    // Advance a client's acked baseline to 'tick' (its §8.3 snapshot ack).
    void AckBaseline(uint32_t clientId, uint32_t tick) { m_baselines[clientId].Ack(tick); }

    // The tombstone (pending-removal) netIds for a client (M4 area D, §8.4): every
    // entity the client had acked that is no longer in its interest set — it left a
    // cell (area A) or was destroyed (removed from the grid). Reconciled against the
    // live interest set each call, so an entity that re-entered before its removal
    // acked is un-tombstoned (its normal version diff resends it). The scheduler
    // (area E) emits one DeltaTomb record per returned netId and reports them via
    // RecordTombstonesSent so the client's ack clears them. netId order.
    [[nodiscard]] std::vector<uint32_t> TombstonesFor(uint32_t clientId)
    {
        std::vector<uint32_t> visible;
        m_interest.VisibleTo(clientId, visible); // sorted/deduped
        Neuron::Sim::ClientBaseline& base = m_baselines[clientId];

        std::vector<uint32_t> acked;
        base.CollectAcked(acked);
        // acked and visible are both sorted: an acked entry absent from visible has
        // left interest → tombstone it.
        size_t vi = 0;
        for (uint32_t n : acked) {
            while (vi < visible.size() && visible[vi] < n) ++vi;
            if (vi >= visible.size() || visible[vi] != n) {
                base.Tombstone(n);
                m_known[clientId].Forget(n); // drop its delta base — a re-entry is first-sight
            }
        }
        for (uint32_t n : visible) base.Untombstone(n); // came back → cancel

        std::vector<uint32_t> out;
        base.CollectTombstones(out);
        return out;
    }

    // Record that 'clientId's snapshot for 'tick' carried tombstone records for
    // 'netIds' (area D), so acking 'tick' clears them.
    void RecordTombstonesSent(uint32_t clientId, uint32_t tick, const std::vector<uint32_t>& netIds)
    {
        m_baselines[clientId].RecordTombstonesSent(tick, netIds);
    }

    // --- priority / quota snapshot scheduler (area E, §8.4) ------------------

    // Tunables (the §19 open question; area J sweeps them). Authored, not literal.
    [[nodiscard]] RelevanceWeights& SchedulerWeights() noexcept { return m_weights; }
    void SetVisibleCap(size_t cap) noexcept { m_visibleCap = cap; }
    [[nodiscard]] size_t VisibleCap() const noexcept { return m_visibleCap; }

    // Build the MTU-budgeted delta snapshot for one client (the area-A→E pipeline):
    //   1. select the entities it still lacks (area B: in interest, version > acked),
    //   2. rank them by the named priority function and apply the visible cap (R16),
    //   3. encode each as a minimal delta against the client's last-acked base
    //      (area C), append tombstone records for entities that left (area D),
    //   4. keep the priority-ordered prefix that fits the byte budget; the rest spill
    //      to later ticks (their version stays > acked, so none are dropped).
    // The returned snapshot is ready to seal/send. 'capped' reports how many interest
    // entities the cap shed (R16 evidence). Pure read of post-tick state — the area-F
    // job pool runs this per client over a frozen snapshot.
    [[nodiscard]] DeltaSnapshot BuildClientSnapshot(uint32_t clientId, size_t byteBudget,
                                                    size_t* capped = nullptr)
    {
        // (1) what the client lacks, and its focus + current targets for ranking.
        const std::vector<uint32_t> lacked = ChangedFor(clientId);
        Neuron::Universe::UniversePos focus{};
        const bool haveFocus = GetBasePos(clientId, focus);
        const std::unordered_set<uint32_t> targets = ClientTargets(clientId);

        std::vector<Neuron::Sim::SchedCandidate> cands;
        cands.reserve(lacked.size());
        auto& sent = m_lastSent[clientId];
        for (uint32_t netId : lacked) {
            Neuron::Sim::SchedCandidate c;
            c.netId = netId;
            SnapshotEntity cur;
            if (!SnapshotEntityOf(netId, cur)) continue;
            c.distance = haveFocus ? Neuron::Sim::UniverseDistance(focus, cur.pos) : 0.0;
            c.iff      = ClassifyIff(clientId, cur);
            c.isBase   = (cur.kind == EntityKind::Base || cur.kind == EntityKind::Structure);
            c.isTarget = targets.count(netId) != 0;
            const auto sit = sent.find(netId);
            c.staleness = (sit == sent.end()) ? 0u : (m_tick - sit->second);
            cands.push_back(c);
        }

        const Neuron::Sim::ScheduleResult sched =
            Neuron::Sim::ScheduleClient(std::move(cands), m_weights, m_visibleCap);
        if (capped) *capped = sched.capped;

        // (3) encode minimal deltas against the per-client acked base.
        Neuron::Sim::ClientKnownState& known = m_known[clientId];
        std::vector<DeltaRecord> ordered;
        ordered.reserve(sched.ordered.size());
        for (uint32_t netId : sched.ordered) {
            SnapshotEntity cur;
            if (!SnapshotEntityOf(netId, cur)) continue;
            DeltaRecord r = MakeDeltaRecord(cur, known.Base(netId));
            if (r.mask != 0) ordered.push_back(r);
        }
        // Tombstones ride every snapshot until acked (area D), ahead of the budget.
        const std::vector<uint32_t> tombs = TombstonesFor(clientId);
        for (uint32_t netId : tombs) {
            DeltaRecord r; r.netId = netId; r.mask = DeltaTomb;
            ordered.push_back(r);
        }

        // (4) keep the prefix that fits the MTU budget; the rest spill next tick.
        std::vector<uint32_t> overflow;
        DeltaSnapshot snap = BuildBudgetedSnapshot(m_tick, ordered, byteBudget, overflow);
        return snap;
    }

    // Stage what a client snapshot carried so its ack advances every baseline (the
    // version baseline, the delta-base cache, the last-sent staleness clock, and the
    // tombstone clock). Call with the snapshot BuildClientSnapshot returned.
    void RecordClientSnapshotSent(uint32_t clientId, const DeltaSnapshot& snap)
    {
        std::vector<uint32_t> liveIds;     // non-tombstone records
        std::vector<uint32_t> tombIds;     // tombstone records
        std::vector<SnapshotEntity> states;
        auto& sent = m_lastSent[clientId];
        for (const DeltaRecord& r : snap.records) {
            if (r.mask & DeltaTomb) { tombIds.push_back(r.netId); continue; }
            liveIds.push_back(r.netId);
            sent[r.netId] = snap.tick;
            SnapshotEntity cur;
            if (SnapshotEntityOf(r.netId, cur)) states.push_back(cur);
        }
        RecordSent(clientId, snap.tick, liveIds);              // version baseline (area B)
        m_known[clientId].RecordSent(snap.tick, std::move(states)); // delta base (area E)
        RecordTombstonesSent(clientId, snap.tick, tombIds);    // tombstone clock (area D)
    }

    // Advance every per-client baseline to the client's §8.3 ack of 'tick'.
    void AckClient(uint32_t clientId, uint32_t tick)
    {
        m_baselines[clientId].Ack(tick);
        m_known[clientId].Ack(tick);
    }

    // Total per-client baseline RAM: acked-version maps (area B) + delta-base caches
    // (area E). The App. B gate area I reports.
    [[nodiscard]] size_t TotalClientBaselineBytes() const
    {
        size_t bytes = 0;
        for (const auto& [clientId, base] : m_baselines) bytes += base.ApproxBytes();
        for (const auto& [clientId, known] : m_known)    bytes += known.ApproxBytes();
        return bytes;
    }

    // Per-client / total baseline RAM (App. B gate; area I telemetry reads these).
    [[nodiscard]] size_t BaselineBytes(uint32_t clientId) { return m_baselines[clientId].ApproxBytes(); }
    [[nodiscard]] size_t TotalBaselineBytes() const
    {
        size_t bytes = 0;
        for (const auto& [clientId, base] : m_baselines) bytes += base.ApproxBytes();
        return bytes;
    }

    [[nodiscard]] Neuron::Sim::ClientBaseline& Baseline(uint32_t clientId) { return m_baselines[clientId]; }

    // Refresh the cell publish/subscribe interest grid for this tick (M4 area A,
    // §8.4): every replicated entity re-homes into its current sector cell (one
    // leave + one enter on a crossing), and every player's subscription is set to
    // the union of the sector neighbourhoods its sensor sources can reach. Pure
    // bookkeeping — touches only the interest grid, never sim state, so SimHash is
    // unchanged. The per-client snapshot diff (areas B/C/E) reads this grid.
    void UpdateInterest()
    {
        m_world.ForEach<NetId, Transform>([&](NetId& id, Transform& t) {
            m_interest.UpdateResidency(id.value, Neuron::Universe::UniverseToSector(t.pos));
        });
        std::unordered_map<uint32_t, std::vector<Neuron::Universe::SectorId>> perPlayer;
        m_world.ForEach<OwnerId, Sensor, Transform>([&](OwnerId& o, Sensor& s, Transform& t) {
            if (o.player == 0) return; // NPC/unowned eyes don't subscribe
            Neuron::Sim::CollectNeighbourhood(Neuron::Universe::UniverseToSector(t.pos),
                                              Neuron::Sim::SectorRadiusForRange(s.range),
                                              perPlayer[o.player]);
        });
        for (auto& [player, cells] : perPlayer)
            m_interest.SetSubscription(player, cells);
    }

    // Interest grid accessor (the area-B/C/E snapshot diff + tests read it).
    [[nodiscard]] Neuron::Sim::InterestGrid& Interest() noexcept { return m_interest; }

    // Build the full snapshot for this tick (interest = everything until M4; the
    // per-player fog-filtered variant is BuildSnapshotFor, area E).
    [[nodiscard]] Snapshot BuildSnapshot()
    {
        Snapshot snap;
        snap.tick = m_tick;
        m_world.ForEach<NetId, Transform, ShapeId>([&](NetId& id, Transform& t, ShapeId& s) {
            snap.entities.push_back(MakeSnapshotEntity(id, t, s));
        });
        return snap;
    }

    // Current replicated projection of an entity (the area-C delta codec / area-E
    // encoder reads this). False if the netId has no replicated components.
    [[nodiscard]] bool SnapshotEntityOf(uint32_t netId, SnapshotEntity& out)
    {
        const auto e = EntityOf(netId);
        NetId*     id = m_world.GetComponent<NetId>(e);
        Transform* t  = m_world.GetComponent<Transform>(e);
        ShapeId*   s  = m_world.GetComponent<ShapeId>(e);
        if (!id || !t || !s) return false;
        out = MakeSnapshotEntity(*id, *t, *s);
        return true;
    }

    [[nodiscard]] uint32_t Tick() const noexcept { return m_tick; }
    [[nodiscard]] Neuron::ECS::World& World() noexcept { return m_world; }

    // Deterministic 64-bit fingerprint of the authoritative sim state (§16.1/§16.2
    // record/replay): hash every entity's key fields in netId order so two runs of
    // the same input log produce identical hashes. Order-independent of ECS storage
    // layout (sorted by netId first), so it's a stable cross-run determinism gate.
    [[nodiscard]] uint64_t SimHash()
    {
        struct Row { uint32_t netId; int64_t x, y, z; int32_t hp; uint8_t kind, phase; uint32_t fuelBits; };
        std::vector<Row> rows;
        m_world.ForEach<NetId, Transform, ShapeId>([&](NetId& id, Transform& t, ShapeId& s) {
            Row r{ id.value, t.pos.x, t.pos.y, t.pos.z, 0,
                   static_cast<uint8_t>(s.kind), 0u, 0u };
            if (const Health* h   = m_world.GetComponent<Health>(EntityOf(id.value)))   r.hp = h->hp;
            if (const NavState* n = m_world.GetComponent<NavState>(EntityOf(id.value)))  r.phase = static_cast<uint8_t>(n->phase);
            if (const Fuel* f     = m_world.GetComponent<Fuel>(EntityOf(id.value))) {
                float v = f->current; std::memcpy(&r.fuelBits, &v, sizeof(v));
            }
            rows.push_back(r);
        });
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.netId < b.netId; });

        uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
        auto mix = [&h](const void* p, size_t n) {
            const auto* b = static_cast<const uint8_t*>(p);
            for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        };
        mix(&m_tick, sizeof(m_tick));
        for (const Row& r : rows) { // field-by-field (avoid struct padding in the hash)
            mix(&r.netId, sizeof(r.netId)); mix(&r.x, sizeof(r.x));
            mix(&r.y, sizeof(r.y)); mix(&r.z, sizeof(r.z)); mix(&r.hp, sizeof(r.hp));
            mix(&r.kind, sizeof(r.kind)); mix(&r.phase, sizeof(r.phase));
            mix(&r.fuelBits, sizeof(r.fuelBits));
        }
        return h;
    }

    // Read a base's authoritative position (for tests / diagnostics).
    [[nodiscard]] bool GetBasePos(uint32_t netId, Neuron::Universe::UniversePos& out)
    {
        if (auto* t = m_world.GetComponent<Transform>(EntityOf(netId))) { out = t->pos; return true; }
        return false;
    }

    // --- warm-restart capture / restore (M5 area F, §15) ----------------------
    // Portable glue between the authoritative ECS and the persistable POD mirror
    // Neuron::Persist::PersistState (WarmRestart.h): the half the snapshot serializer
    // (SimSnapshotStore) writes and a restart loads. Verified on the Linux testrunner
    // (WarmRestartCaptureTests) — Capture→Encode→Decode→Restore→Capture is stable, and
    // SimHash matches for the economy/ownership subset.
    //
    // Authoritative-entity → PersistState mapping (covers what M3/M4 produce, §15):
    //   * bases (BaseTag):     pos, layered HP (M3's single Health → hull; shield/armor
    //                          seed to maxHp until the M6 combat model fills them, per
    //                          WarmRestart.h), Storage[3], Fuel, NavState.phase,
    //                          ownership (OwnerId.player), baseState (0 = active at M3).
    //   * ships  (ShipTag, no NpcAi): pos, HP, Cargo[3], shipType, ownership.
    //   * builds (active BuildQueue):  owner, recipe, progress.
    //   * npcs   (NpcAi):      pos, HP, siteId, aiState — transient, blob-only (§15).
    // ownerAccount carries OwnerId.player here (a player ≈ their base net id at this
    // layer); ERServer's auth layer rebinds the real AccountId on restore (§14).
    [[nodiscard]] Neuron::Persist::PersistState CaptureState() const
    {
        Neuron::Persist::PersistState s;
        s.tick = m_tick;

        // Bases: BaseTag. Layered-HP convention: hull = current Health.hp, shield/armor
        // seed to maxHp (round-trips: restore writes hull back to Health.hp).
        m_world.ForEach<BaseTag, NetId, Transform>(
            [&](const BaseTag&, const NetId& id, const Transform& t) {
                Neuron::Persist::PersistBase b;
                b.netId = id.value;
                b.x = t.pos.x; b.y = t.pos.y; b.z = t.pos.z;
                if (const OwnerId* o = GetC<OwnerId>(id.value)) b.ownerAccount = o->player;
                if (const Health* h = GetC<Health>(id.value)) {
                    b.hullHp = h->hp; b.shieldHp = h->maxHp; b.armorHp = h->maxHp;
                }
                if (const Storage* st = GetC<Storage>(id.value))
                    for (int i = 0; i < kResourceSlots; ++i) b.storage[i] = st->amount[i];
                if (const Fuel* f = GetC<Fuel>(id.value)) b.fuel = f->current;
                if (const NavState* n = GetC<NavState>(id.value)) b.navPhase = static_cast<uint8_t>(n->phase);
                // Disable-not-destroy state (§13.1, area G) — persisted so a restart keeps
                // a retreating/disabled base in that state (the Health mapping above stays
                // the M5 mirror convention: hullHp = total cur, shield/armor = total max).
                const BaseCombat* bc = GetC<BaseCombat>(id.value);
                b.baseState = bc ? static_cast<uint8_t>(bc->state) : 0;
                s.bases.push_back(b);
            });

        // Ships: ShipTag without NpcAi (NPC guardians also carry ShipTag).
        m_world.ForEach<ShipTag, NetId, Transform>(
            [&](const ShipTag& tag, const NetId& id, const Transform& t) {
                if (GetC<NpcAi>(id.value)) return; // NPC, captured below
                Neuron::Persist::PersistShip sh;
                sh.netId = id.value;
                sh.x = t.pos.x; sh.y = t.pos.y; sh.z = t.pos.z;
                sh.shipType = tag.shipType;
                if (const OwnerId* o = GetC<OwnerId>(id.value)) sh.ownerAccount = o->player;
                if (const Health* h = GetC<Health>(id.value)) sh.hp = h->hp;
                if (const Cargo* c = GetC<Cargo>(id.value))
                    for (int i = 0; i < kResourceSlots; ++i) sh.cargo[i] = c->amount[i];
                s.ships.push_back(sh);
            });

        // Builds: one row per active build queue (owner-scoped, §13.4).
        m_world.ForEach<BuildQueue, OwnerId>([&](const BuildQueue& q, const OwnerId& o) {
            if (!q.active) return;
            Neuron::Persist::PersistBuild bd;
            bd.ownerAccount = o.player;
            bd.itemDefId    = q.recipe;
            bd.progress     = q.progress;
            s.builds.push_back(bd);
        });

        // NPCs: NpcAi (transient; blob-only, §15).
        m_world.ForEach<NpcAi, NetId, Transform>(
            [&](const NpcAi& ai, const NetId& id, const Transform& t) {
                Neuron::Persist::PersistNpc n;
                n.netId = id.value;
                n.x = t.pos.x; n.y = t.pos.y; n.z = t.pos.z;
                n.siteId  = ai.siteId;
                n.aiState = static_cast<uint8_t>(ai.state);
                if (const Health* h = GetC<Health>(id.value)) n.hp = h->hp;
                s.npcs.push_back(n);
            });

        return s;
    }

    // Rebuild the authoritative sim from a PersistState (warm restart). Destroys all
    // current entities and respawns bases/ships/NPCs at their persisted net ids, then
    // restores layered HP / storage / cargo / fuel / nav / build / ownership exactly,
    // so a subsequent CaptureState reproduces 'state' (the testrunner round-trip gate).
    // Beacons/scenery are reconstructed from the cooked dataset on a separate load, not
    // the blob (map infrastructure is game data, §15) — call LoadUniverse before this if
    // the beacon graph is needed; restore does not respawn props.
    void RestoreState(const Neuron::Persist::PersistState& state)
    {
        // Wipe every live entity + side tables (a fresh shard, §9 stateless restore).
        std::vector<Neuron::ECS::EntityHandle> all;
        all.reserve(m_netIdToEntity.size());
        for (const auto& [netId, e] : m_netIdToEntity) all.push_back(e);
        for (const auto e : all) if (m_world.IsAlive(e)) m_world.DestroyEntity(e);
        m_netIdToEntity.clear();
        m_interest = Neuron::Sim::InterestGrid{};
        m_repl     = Neuron::Sim::ReplicationStamps{};
        m_baselines.clear();
        m_known.clear();
        m_lastSent.clear();
        m_buildCompleted.clear();
        m_econEvents.clear();
        m_killmails.clear();
        m_lowHullAlerts.clear();
        m_simTime = 0.0;

        m_tick = static_cast<uint32_t>(state.tick);
        uint32_t maxNetId = 0;

        for (const auto& b : state.bases) {
            auto e = m_world.CreateEntity();
            m_world.AddComponent<Transform>(e).pos = { b.x, b.y, b.z };
            m_world.AddComponent<Velocity>(e);
            m_world.AddComponent<NetId>(e).value = b.netId;
            m_world.AddComponent<BaseTag>(e);
            m_world.AddComponent<ShapeId>(e) = { BaseShapeId(), EntityKind::Base };
            auto& fuel = m_world.AddComponent<Fuel>(e);
            fuel = { b.fuel, m_nav.baseFuelMax };
            m_world.AddComponent<NavState>(e).phase = static_cast<NavPhase>(b.navPhase);
            m_world.AddComponent<OwnerId>(e).player = static_cast<uint32_t>(b.ownerAccount);
            auto& st = m_world.AddComponent<Storage>(e);
            st.capacity = m_economy.storageCapacity;
            for (int i = 0; i < kResourceSlots; ++i) st.amount[i] = b.storage[i];
            auto& q = m_world.AddComponent<BuildQueue>(e);
            m_world.AddComponent<Sensor>(e).range = m_economy.sensorRangeBase;
            // Re-apply this owner's active build (matched below from state.builds).
            for (const auto& bd : state.builds) {
                if (bd.ownerAccount == b.ownerAccount) {
                    q.active = true; q.paid = true; q.recipe = static_cast<uint8_t>(bd.itemDefId);
                    q.progress = bd.progress;
                    break;
                }
            }
            // Combat: the disable-not-destroy state + the capital fit, then restore the
            // persisted Health mirror over the fit's full defaults (the layered split is
            // not separately persisted under the M5 mirror convention, §15 — a restarted
            // base recovers combat HP on its first tick; baseState IS preserved).
            m_world.AddComponent<BaseCombat>(e).state = static_cast<BaseState>(b.baseState);
            InstallFit(e, "base-firesupport");
            if (Health* h = m_world.GetComponent<Health>(e)) { h->hp = b.hullHp; h->maxHp = b.shieldHp; }
            m_netIdToEntity[b.netId] = e;
            maxNetId = std::max(maxNetId, b.netId);
        }

        for (const auto& sh : state.ships) {
            auto e = m_world.CreateEntity();
            m_world.AddComponent<Transform>(e).pos = { sh.x, sh.y, sh.z };
            m_world.AddComponent<Velocity>(e);
            m_world.AddComponent<NetId>(e).value = sh.netId;
            m_world.AddComponent<ShipTag>(e).shipType = sh.shipType;
            m_world.AddComponent<ShapeId>(e) = { ShipShapeId(), EntityKind::Ship };
            m_world.AddComponent<OwnerId>(e).player = static_cast<uint32_t>(sh.ownerAccount);
            auto& c = m_world.AddComponent<Cargo>(e);
            c.capacity = m_economy.cargoCapacity;
            for (int i = 0; i < kResourceSlots; ++i) c.amount[i] = sh.cargo[i];
            m_world.AddComponent<Fuel>(e) = { m_nav.shipFuelMax, m_nav.shipFuelMax };
            m_world.AddComponent<NavState>(e);
            m_world.AddComponent<Sensor>(e).range = m_economy.sensorRangeShip;
            m_world.AddComponent<FleetMember>(e);
            m_world.AddComponent<FleetOrder>(e);
            InstallFit(e, kDefaultShipFit); // layered HP + resists + fitting + Health mirror
            if (Health* h = m_world.GetComponent<Health>(e)) h->hp = sh.hp; // restore mirror cur
            m_netIdToEntity[sh.netId] = e;
            maxNetId = std::max(maxNetId, sh.netId);
        }

        m_siteAlive.clear();
        m_clearedSites.clear();
        for (const auto& n : state.npcs) {
            auto e = m_world.CreateEntity();
            m_world.AddComponent<Transform>(e).pos = { n.x, n.y, n.z };
            m_world.AddComponent<Velocity>(e);
            m_world.AddComponent<NetId>(e).value = n.netId;
            m_world.AddComponent<ShipTag>(e).shipType = 0;
            m_world.AddComponent<ShapeId>(e) = { ShipShapeId(), EntityKind::NpcUnit };
            m_world.AddComponent<OwnerId>(e).player = 0;
            m_world.AddComponent<FleetOrder>(e);
            InstallFit(e, kNpcFit);
            if (Health* h = m_world.GetComponent<Health>(e)) h->hp = n.hp; // restore mirror cur
            auto& ai = m_world.AddComponent<NpcAi>(e);
            ai.state      = static_cast<AiState>(n.aiState);
            ai.home       = { n.x, n.y, n.z };
            ai.aggroRange = kNpcAggroRange;
            ai.fleeHpFrac = kNpcFleeHpFrac;
            ai.siteId     = n.siteId;
            m_netIdToEntity[n.netId] = e;
            ++m_siteAlive[n.siteId];
            maxNetId = std::max(maxNetId, n.netId);
        }

        m_nextNetId = maxNetId + 1; // future spawns never collide with restored ids
    }

    static constexpr float kMaxBaseSpeed = 50.0f; // m/s cap (server validates intents)

    // Movement / AI constants. The combat BALANCE is now game data (CombatData.h, §15);
    // these are sim-shape constants (fallback speed, NPC sensing), not balance literals.
    static constexpr float   kFleetMoveSpeed  = 2000.0f; // fallback m/s if a unit has no HullInfo
    static constexpr float   kNpcAggroRange   = 6000.0f;
    static constexpr float   kNpcFleeHpFrac   = 0.15f;
    static constexpr float   kScanSeconds     = 3.0f; // dwell to reveal a contact (area E)
    // Default catalog fits (combat-balance.md §6) referenced by spawns / bots by name.
    static constexpr std::string_view kDefaultShipFit = "fighter-kin";
    static constexpr std::string_view kNpcFit         = "fighter-kin";

private:
    [[nodiscard]] Neuron::ECS::EntityHandle EntityOf(uint32_t netId) const
    {
        auto it = m_netIdToEntity.find(netId);
        return it == m_netIdToEntity.end() ? Neuron::ECS::EntityHandle::Null() : it->second;
    }
    // const component read for CaptureState (the ECS GetComponent is non-const but the
    // read is logically const — capture never mutates sim state).
    template <typename T>
    [[nodiscard]] const T* GetC(uint32_t netId) const
    {
        return const_cast<Neuron::ECS::World&>(m_world).GetComponent<T>(EntityOf(netId));
    }
    [[nodiscard]] bool IsBase(uint32_t netId) { return m_world.HasComponent<BaseTag>(EntityOf(netId)); }
    void StopMotion(uint32_t netId)
    {
        if (auto* v = m_world.GetComponent<Velocity>(EntityOf(netId))) v->metresPerSecond = { 0, 0, 0 };
    }

    // Build one snapshot record, reading real Health where present (combat needs
    // live HP for the client's selected/target panels; scenery has no Health).
    [[nodiscard]] SnapshotEntity MakeSnapshotEntity(const NetId& id, const Transform& t, const ShapeId& s)
    {
        SnapshotEntity e;
        e.netId       = id.value;
        e.kind        = s.kind;
        e.pos         = t.pos;
        e.localOffset = t.localOffset;
        const Health* h = m_world.GetComponent<Health>(EntityOf(id.value));
        e.hp          = h ? h->hp : 1000;
        e.shapeId     = s.value;
        const OwnerId* o = m_world.GetComponent<OwnerId>(EntityOf(id.value));
        e.ownerPlayer = o ? o->player : 0;
        return e;
    }

    // --- snapshot scheduler helpers (area E) --------------------------------

    // Classify an entity relative to the client for the priority IFF term: the
    // client's own units, hostiles (another player's units or an NPC), else neutral
    // (scenery, structures, resource nodes).
    [[nodiscard]] static Neuron::Sim::Iff ClassifyIff(uint32_t clientId, const SnapshotEntity& e) noexcept
    {
        if (e.ownerPlayer == clientId)               return Neuron::Sim::Iff::Own;
        if (e.ownerPlayer != 0 || e.kind == EntityKind::NpcUnit) return Neuron::Sim::Iff::Enemy;
        return Neuron::Sim::Iff::Neutral;
    }

    // The netIds the client's units currently have as a command target (Attack/
    // Guard/Orbit/KeepRange) — these get the priority target bonus.
    [[nodiscard]] std::unordered_set<uint32_t> ClientTargets(uint32_t clientId)
    {
        std::unordered_set<uint32_t> targets;
        m_world.ForEach<OwnerId, FleetOrder>([&](OwnerId& o, FleetOrder& fo) {
            if (o.player == clientId && fo.current.targetNetId != 0)
                targets.insert(fo.current.targetNetId);
        });
        return targets;
    }

    // --- fleet command routing (area B) -------------------------------------

    // Route one validated intent to a single owned unit. Move/Attack/Guard/Orbit/
    // KeepRange/Retreat become FleetOrders (steered by FleetOrderSystem); Harvest/
    // Warp/Jump dispatch to their own systems; Stop clears all orders. Returns
    // false if the intent's target is invalid for this unit (rejected, §8.4).
    bool ApplyIntentToUnit(uint32_t unitNet, const FleetCommand& cmd)
    {
        switch (cmd.intent) {
        case IntentType::Stop:
            ClearOrders(unitNet);
            return true;
        case IntentType::Harvest:
            return OrderHarvest(unitNet, cmd.targetNetId);
        case IntentType::Warp:
            return BeginWarpTo(unitNet, cmd.targetPoint);
        case IntentType::Jump:
            return BeginJumpTo(unitNet, cmd.beacon) == JumpReject::Accepted;
        case IntentType::Build:
            return EnqueueBuild(unitNet); // 'unit' is the player's base

        case IntentType::Move:
            return PushOrder(unitNet, { OrderType::Move, 0, cmd.targetPoint, 0.0f }, cmd.queue);
        case IntentType::Retreat: {
            // Fall back to the owner's base position (player ≈ base net id).
            const OwnerId* o = m_world.GetComponent<OwnerId>(EntityOf(unitNet));
            Neuron::Universe::UniversePos home{};
            if (!o || !GetBasePos(o->player, home)) return false;
            return PushOrder(unitNet, { OrderType::Retreat, 0, home, 0.0f }, cmd.queue);
        }
        case IntentType::Attack:
        case IntentType::Guard:
        case IntentType::Orbit:
        case IntentType::KeepRange: {
            if (!m_world.IsAlive(EntityOf(cmd.targetNetId))) return false; // need a live target
            const OrderType ot = (cmd.intent == IntentType::Attack)    ? OrderType::Attack
                               : (cmd.intent == IntentType::Guard)     ? OrderType::Guard
                               : (cmd.intent == IntentType::Orbit)     ? OrderType::Orbit
                                                                       : OrderType::KeepRange;
            return PushOrder(unitNet, { ot, cmd.targetNetId, {}, cmd.range }, cmd.queue);
        }
        }
        return false;
    }

    // Set or queue (shift-chain) a unit's fleet order. Units without a FleetOrder
    // (e.g. the base) can't take steering orders → rejected.
    bool PushOrder(uint32_t unitNet, const FleetOrderEntry& entry, bool queue)
    {
        FleetOrder* fo = m_world.GetComponent<FleetOrder>(EntityOf(unitNet));
        if (!fo) return false;
        if (queue && fo->current.type != OrderType::Idle) {
            fo->queue.push_back(entry);
        } else {
            fo->current = entry;
            fo->queue.clear();
            // A direct order pre-empts the harvest auto-pilot (area C).
            if (HarvestOrder* ho = m_world.GetComponent<HarvestOrder>(EntityOf(unitNet)))
                ho->phase = HarvestPhase::Idle;
        }
        return true;
    }

    void ClearOrders(uint32_t unitNet)
    {
        if (FleetOrder* fo = m_world.GetComponent<FleetOrder>(EntityOf(unitNet))) {
            fo->current = {}; fo->queue.clear();
        }
        if (HarvestOrder* ho = m_world.GetComponent<HarvestOrder>(EntityOf(unitNet)))
            ho->phase = HarvestPhase::Idle;
        StopMotion(unitNet);
    }

    // Advance to the next queued order (or Idle) when the current one completes.
    static void AdvanceOrder(FleetOrder& fo) noexcept
    {
        if (!fo.queue.empty()) { fo.current = fo.queue.front(); fo.queue.erase(fo.queue.begin()); }
        else fo.current = {};
    }

    // Drive every unit's current FleetOrder one tick: steer toward Move/Retreat
    // points and Guard/Orbit/KeepRange/Attack targets (Attack damage is the combat
    // system's job; this just closes to weapon range). Pure-rule steering lives in
    // Fleet.h; this sequences it across targets looked up by net id.
    void FleetOrderSystem(float dt)
    {
        m_world.ForEach<FleetOrder, Transform, NetId>([&](FleetOrder& fo, Transform& tr, NetId& selfId) {
            FleetOrderEntry& o = fo.current;
            // Per-unit sublight speed from the hull (fast lights / slow heavies), scaled
            // by any active web (EWAR slows the target — area E). Falls back to the M3
            // fleet speed if the unit has no HullInfo (e.g. a legacy spawn).
            const HullInfo*   hi  = m_world.GetComponent<HullInfo>(EntityOf(selfId.value));
            const EwarStatus* es  = m_world.GetComponent<EwarStatus>(EntityOf(selfId.value));
            const float       spd = (hi && hi->maxSpeed > 0.0f ? hi->maxSpeed : kFleetMoveSpeed)
                                    * (es ? es->webFactor : 1.0f);
            const double maxStep = static_cast<double>(spd) * static_cast<double>(dt);
            switch (o.type) {
            case OrderType::Idle:
                return;
            case OrderType::Move:
            case OrderType::Retreat:
                if (StepStandoff(tr, o.targetPoint, maxStep, 0.0)) AdvanceOrder(fo);
                return;
            case OrderType::Attack:
            case OrderType::Guard:
            case OrderType::Orbit:
            case OrderType::KeepRange: {
                Transform* tgt = m_world.GetComponent<Transform>(EntityOf(o.targetNetId));
                if (!tgt || !m_world.IsAlive(EntityOf(o.targetNetId))) { AdvanceOrder(fo); return; }
                // Attack closes to *this unit's own* weapon range so it stops and fires;
                // the other stances hold at the order's requested stand-off.
                const double standoff = (o.type == OrderType::Attack)
                                        ? static_cast<double>(AttackStandoff(EntityOf(selfId.value)))
                                        : static_cast<double>(o.range);
                StepStandoff(tr, tgt->pos, maxStep, standoff);
                return;
            }
            }
        });
    }

    // Keep an attacker just inside its own weapon range (so it stops and fires).
    [[nodiscard]] float AttackStandoff(Neuron::ECS::EntityHandle attacker)
    {
        const Weapon* w = m_world.GetComponent<Weapon>(attacker);
        return w ? w->range * 0.9f : 0.0f;
    }

    // --- fitting install (area B) -------------------------------------------

    // Add a component if absent, then assign — so InstallFit can (re)fit an entity.
    template <typename T>
    T& AddOrSet(Neuron::ECS::EntityHandle e, T val)
    {
        T* c = m_world.GetComponent<T>(e);
        if (!c) c = &m_world.AddComponent<T>(e);
        *c = std::move(val);
        return *c;
    }

    // Derived single-weapon summary (range = longest weapon reach, dps = Σ dmg×rof) —
    // used by FleetOrderSystem stand-off + the AI target heuristics, NOT for damage.
    [[nodiscard]] static Weapon DeriveWeaponSummary(const Fitting& fit) noexcept
    {
        Weapon w; float range = 0.0f, dps = 0.0f;
        for (const auto& mi : fit.modules) {
            if (mi.def.kind != ModuleKind::Weapon) continue;
            range = std::max(range, mi.def.optimal + mi.def.falloff);
            dps  += mi.def.baseDamage * mi.def.rateOfFire;
        }
        w.range = range; w.dps = dps; w.pending = 0.0f;
        return w;
    }

    // Install a named catalog fit's combat components (DefenseLayers + ResistProfile +
    // Fitting + HullInfo + EwarStatus + Weapon summary + the synced Health mirror).
    // False if the fit/hull is unknown. ValidateFit gated the catalog, so the fit is
    // never over-budget (§8.4 — server-authoritative).
    bool InstallFit(Neuron::ECS::EntityHandle e, std::string_view fitName)
    {
        const FitTemplate* ft = m_combat.FindFit(fitName);
        if (!ft) return false;
        const HullClass* hull = m_combat.FindHull(ft->hull);
        if (!hull) return false;
        std::vector<const ModuleDef*> mods;
        mods.reserve(ft->modules.size());
        for (const auto& code : ft->modules)
            if (const ModuleDef* m = m_combat.FindModule(code)) mods.push_back(m);
        InstallFitDirect(e, *hull, mods);
        return true;
    }

    void InstallFitDirect(Neuron::ECS::EntityHandle e, const HullClass& hull,
                          const std::vector<const ModuleDef*>& mods)
    {
        // First pass: passive Low/Mid bonuses, baked into layer HP + weapon instances.
        float dmgAmp = 0.0f, trackAmp = 0.0f, shieldBoost = 0.0f;
        int32_t plateArmor = 0;
        for (const ModuleDef* m : mods) {
            switch (m->kind) {
            case ModuleKind::DamageAmp:        dmgAmp      += m->strength;                       break;
            case ModuleKind::TrackingEnhancer: trackAmp    += m->strength;                       break;
            case ModuleKind::ShieldBooster:    shieldBoost += m->strength;                       break;
            case ModuleKind::ArmorPlate:       plateArmor  += static_cast<int32_t>(m->strength); break;
            default: break;
            }
        }

        DefenseLayers d;
        d.shield = { hull.shieldHp, hull.shieldHp };
        d.armor  = { hull.armorHp + plateArmor, hull.armorHp + plateArmor };
        d.hull   = { hull.hullHp, hull.hullHp };
        d.shieldRegenPerSec = hull.shieldRegenPerSec + shieldBoost;

        Fitting fit;
        fit.pgMax = hull.pgMax; fit.cpuMax = hull.cpuMax;
        fit.slots[0] = hull.slotsHigh; fit.slots[1] = hull.slotsMid; fit.slots[2] = hull.slotsLow;
        for (const ModuleDef* m : mods) {
            fit.pgUsed += m->pgCost; fit.cpuUsed += m->cpuCost;
            ModuleInstance mi; mi.def = *m;
            if (m->kind == ModuleKind::Weapon) { // bake the Low-slot passive bonuses in
                mi.def.baseDamage *= (1.0f + dmgAmp);
                mi.def.tracking   += trackAmp;
            }
            fit.modules.push_back(std::move(mi));
        }

        AddOrSet<DefenseLayers>(e, d);
        AddOrSet<ResistProfile>(e, hull.resists);
        HullInfo hi; hi.signature = hull.signature; hi.maxSpeed = hull.maxSpeed; hi.size = hull.size;
        AddOrSet<HullInfo>(e, hi);
        if (!m_world.HasComponent<EwarStatus>(e)) m_world.AddComponent<EwarStatus>(e);
        AddOrSet<Weapon>(e, DeriveWeaponSummary(fit));
        AddOrSet<Fitting>(e, std::move(fit));
        AddOrSet<Health>(e, Health{ d.TotalCur(), d.TotalMax() }); // synced mirror
    }

    // --- IFF helpers (areas D/E/F) ------------------------------------------

    // NPC = owner 0 (one faction); players are hostile across different ids.
    [[nodiscard]] static bool Hostile(uint32_t oa, uint32_t ob) noexcept
    {
        if (oa == 0 && ob == 0) return false;     // NPC vs NPC — same faction
        if (oa == 0 || ob == 0) return true;      // NPC vs player
        return oa != ob;                          // player vs different player
    }
    [[nodiscard]] static bool Ally(uint32_t oa, uint32_t ob) noexcept
    {
        if (oa == 0 && ob == 0) return true;      // NPCs rep their own
        return oa != 0 && oa == ob;
    }

    // Nearest hostile (alive, has DefenseLayers) to 'pos' within 'range'; 0 if none.
    [[nodiscard]] uint32_t NearestHostileInRange(uint32_t selfNetId, const Neuron::Universe::UniversePos& pos,
                                                 uint32_t selfOwner, double range)
    {
        uint32_t best = 0; double bestDist = 0.0;
        m_world.ForEach<DefenseLayers, Transform, OwnerId, NetId>(
            [&](DefenseLayers& dl, Transform& t, OwnerId& o, NetId& id) {
                if (id.value == selfNetId || dl.hull.cur <= 0) return;
                if (!Hostile(selfOwner, o.player)) return;
                const double dd = UniverseDistance(pos, t.pos);
                if (dd > range) return;
                if (best == 0 || dd < bestDist || (dd == bestDist && id.value < best)) { best = id.value; bestDist = dd; }
            });
        return best;
    }

    // Most-damaged ally within 'range' whose 'layer' can still take a rep; 0 if none.
    [[nodiscard]] uint32_t FindRepTarget(uint32_t selfNetId, uint32_t selfOwner,
                                         const Neuron::Universe::UniversePos& pos, double range, DefenseLayer layer)
    {
        uint32_t best = 0; float bestFrac = 2.0f;
        m_world.ForEach<DefenseLayers, Transform, OwnerId, NetId>(
            [&](DefenseLayers& dl, Transform& t, OwnerId& o, NetId& id) {
                if (id.value == selfNetId) return;           // remote rep can't self-target
                if (!Ally(selfOwner, o.player)) return;
                if (dl.hull.cur <= 0) return;
                const LayerHp& l = (layer == DefenseLayer::Shield) ? dl.shield
                                 : (layer == DefenseLayer::Armor)  ? dl.armor : dl.hull;
                if (l.max <= 0 || l.cur >= l.max) return; // that layer is full
                if (UniverseDistance(pos, t.pos) > range) return;
                const float frac = static_cast<float>(l.cur) / static_cast<float>(l.max);
                if (best == 0 || frac < bestFrac || (frac == bestFrac && id.value < best)) { best = id.value; bestFrac = frac; }
            });
        return best;
    }

    // --- combat: EWAR + logistics (area E) ----------------------------------
    //
    // For each fitted unit, apply its non-weapon active modules: remote rep heals the
    // most-damaged ally in range; jam/web/warp-disrupt/sensor-damp debuff the unit's
    // current target (its Attack target, else the nearest hostile in module range — so
    // a fire-support base still projects EWAR). A JAMMED ship can run no targeted module
    // at all (ECM breaks the lock — weapons in CombatSystem, EWAR/logi here).
    void EwarLogiSystem(float dt)
    {
        m_world.ForEach<Fitting, Transform, OwnerId, NetId>(
            [&](Fitting& fit, Transform& tr, OwnerId& owner, NetId& selfId) {
                EwarStatus* selfEwar = m_world.GetComponent<EwarStatus>(EntityOf(selfId.value));
                if (selfEwar && IsJammed(*selfEwar)) return; // jammed — no targeted modules
                const uint32_t attackTarget = AttackTargetOf(selfId.value);
                for (auto& mi : fit.modules) {
                    const ModuleDef& m = mi.def;
                    if (m.kind == ModuleKind::RemoteRep) {
                        const uint32_t ally = FindRepTarget(selfId.value, owner.player, tr.pos,
                                                            static_cast<double>(m.range), m.effectLayer);
                        if (!ally) continue;
                        mi.pending += m.strength * dt; // accumulate sub-1 rep (HP is integer)
                        if (mi.pending >= 1.0f) {
                            const int32_t amt = static_cast<int32_t>(mi.pending);
                            mi.pending -= static_cast<float>(amt);
                            if (DefenseLayers* ad = m_world.GetComponent<DefenseLayers>(EntityOf(ally)))
                                RemoteRep(*ad, m.effectLayer, amt);
                        }
                        continue;
                    }
                    if (m.kind == ModuleKind::Jammer || m.kind == ModuleKind::Web ||
                        m.kind == ModuleKind::WarpDisruptor || m.kind == ModuleKind::SensorDamp) {
                        // Offensive EWAR follows the unit's Attack target; only a base
                        // (no FleetOrder) auto-projects it onto the nearest hostile —
                        // so a commandable ship EWARs what it is told to, not everything.
                        uint32_t tgt = 0;
                        if (attackTarget && Hostile(owner.player, OwnerOf(attackTarget))) tgt = attackTarget;
                        else if (!m_world.HasComponent<FleetOrder>(EntityOf(selfId.value)))
                            tgt = NearestHostileInRange(selfId.value, tr.pos, owner.player, static_cast<double>(m.range));
                        if (!tgt) continue;
                        Transform* tt = m_world.GetComponent<Transform>(EntityOf(tgt));
                        EwarStatus* te = m_world.GetComponent<EwarStatus>(EntityOf(tgt));
                        if (!tt || !te) continue;
                        if (UniverseDistance(tr.pos, tt->pos) > static_cast<double>(m.range)) continue;
                        switch (m.kind) {
                        case ModuleKind::Jammer:        ApplyJam(*te, m.duration); break;
                        case ModuleKind::Web:           ApplyWeb(*te, m.strength, m.duration); break;
                        case ModuleKind::WarpDisruptor: ApplyTackle(*te, m.duration);
                                                        if (NavState* nv = m_world.GetComponent<NavState>(EntityOf(tgt))) nv->interdicted = true;
                                                        break;
                        case ModuleKind::SensorDamp:    ApplySensorDamp(*te, m.strength, m.duration); break;
                        default: break;
                        }
                    }
                }
            });
    }

    // --- combat: weapons + projectiles (area D) -----------------------------

    struct PendingProjectile { uint32_t src; uint32_t tgt; Neuron::Universe::UniversePos origin; DirectX::XMFLOAT3 vel; ModuleDef def; };
    struct KillCand { uint32_t victim; uint32_t killer; };

    // Tick a unit's weapon cooldowns down (used when it has no valid target this tick).
    void TickWeaponCooldowns(Neuron::ECS::EntityHandle e, float dt)
    {
        if (Fitting* f = m_world.GetComponent<Fitting>(e))
            for (auto& mi : f->modules)
                if (mi.def.kind == ModuleKind::Weapon) mi.cooldown = std::max(0.0f, mi.cooldown - dt);
    }

    // Fire every ready weapon on 'self' at 'targetNetId': hitscan resolves immediately,
    // projectile weapons queue a ballistic shot. Cooldowns cycle even when jammed/out of
    // range. Hostility/validity is the caller's responsibility (Attack order or defense).
    void FireWeapons(Neuron::ECS::EntityHandle self, uint32_t selfNetId,
                     const Neuron::Universe::UniversePos& selfPos, uint32_t /*selfOwner*/,
                     uint32_t targetNetId, float dt,
                     std::vector<PendingProjectile>& projOut, std::vector<KillCand>& kills)
    {
        Fitting* fit = m_world.GetComponent<Fitting>(self);
        if (!fit) return;
        const EwarStatus* es = m_world.GetComponent<EwarStatus>(self);
        const bool  jammed = es && IsJammed(*es);
        const float damp   = es ? es->sensorDampFactor : 1.0f; // damp on the SHOOTER cuts reach

        const auto te = EntityOf(targetNetId);
        Transform*     tt  = m_world.GetComponent<Transform>(te);
        DefenseLayers* td  = m_world.GetComponent<DefenseLayers>(te);
        ResistProfile* trp = m_world.GetComponent<ResistProfile>(te);
        if (!tt || !td || !trp || !m_world.IsAlive(te)) { TickWeaponCooldowns(self, dt); return; }

        const double      dist = UniverseDistance(selfPos, tt->pos);
        const HullInfo*   thi  = m_world.GetComponent<HullInfo>(te);
        const EwarStatus* tes  = m_world.GetComponent<EwarStatus>(te);
        const float tgtSig   = thi ? thi->signature : 100.0f;
        const float tgtSpeed = (thi ? thi->maxSpeed : 0.0f) * (tes ? tes->webFactor : 1.0f);

        for (auto& mi : fit->modules) {
            if (mi.def.kind != ModuleKind::Weapon) continue;
            mi.cooldown = std::max(0.0f, mi.cooldown - dt);
            if (jammed || mi.cooldown > 0.0f) continue;
            if (!InEngagementRange(mi.def, dist, damp)) continue;
            mi.cooldown = (mi.def.rateOfFire > 0.0f) ? (1.0f / mi.def.rateOfFire) : 1.0f;
            if (mi.def.projectileSpeed <= 0.0f) {
                // Hitscan (instant): resolve damage now (area C formula).
                const float eff = ResolveShotDamage(mi.def.damageType, mi.def.baseDamage, dist,
                                                     mi.def.optimal, mi.def.falloff, mi.def.tracking, tgtSig, tgtSpeed);
                if (ApplyTypedDamage(*td, *trp, mi.def.damageType, eff) == DamageOutcome::Killed)
                    kills.push_back({ targetNetId, selfNetId });
            } else {
                // Ballistic: queue a projectile aimed at the target's current position.
                PendingProjectile p; p.src = selfNetId; p.tgt = targetNetId; p.origin = selfPos; p.def = mi.def;
                const double dx = static_cast<double>(tt->pos.x - selfPos.x);
                const double dy = static_cast<double>(tt->pos.y - selfPos.y);
                const double dz = static_cast<double>(tt->pos.z - selfPos.z);
                const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (len > 0.0) {
                    const double s = static_cast<double>(mi.def.projectileSpeed) / len;
                    p.vel = { static_cast<float>(dx * s), static_cast<float>(dy * s), static_cast<float>(dz * s) };
                }
                projOut.push_back(std::move(p));
            }
        }
    }

    // Spawn a ballistic projectile entity (replicated via the M4 interest pipeline, D).
    void SpawnProjectileEntity(const PendingProjectile& p)
    {
        const uint32_t id = m_nextNetId++;
        auto e = m_world.CreateEntity();
        m_world.AddComponent<Transform>(e).pos = p.origin;
        m_world.AddComponent<NetId>(e).value   = id;
        m_world.AddComponent<ShapeId>(e)       = { ProjShapeId(), EntityKind::Projectile };
        Projectile pr;
        pr.sourceNetId = p.src; pr.targetNetId = p.tgt; pr.damageType = p.def.damageType;
        pr.baseDamage  = p.def.baseDamage; pr.vel = p.vel; pr.origin = p.origin;
        pr.optimal = p.def.optimal; pr.falloff = p.def.falloff; pr.tracking = p.def.tracking;
        const float reach = p.def.optimal + p.def.falloff;
        pr.ttl = (p.def.projectileSpeed > 0.0f) ? (reach / p.def.projectileSpeed) * 1.5f + 0.1f : 0.1f;
        m_world.AddComponent<Projectile>(e) = pr;
        m_netIdToEntity[id] = e;
    }

    // Weapons fire from fleet units (Attack order) + bases (defensive auto-target).
    void CombatSystem(float dt)
    {
        std::vector<PendingProjectile> projs;
        std::vector<KillCand>          kills;

        m_world.ForEach<FleetOrder, Fitting, Transform, NetId>(
            [&](FleetOrder& fo, Fitting&, Transform& tr, NetId& id) {
                const auto e = EntityOf(id.value);
                if (fo.current.type != OrderType::Attack || fo.current.targetNetId == 0) { TickWeaponCooldowns(e, dt); return; }
                const OwnerId* o = m_world.GetComponent<OwnerId>(e);
                FireWeapons(e, id.value, tr.pos, o ? o->player : 0, fo.current.targetNetId, dt, projs, kills);
            });

        // Bases have no FleetOrder; while Active they fire defensively at the nearest
        // hostile in range (§13.1 fire-support + light defensive weapons).
        m_world.ForEach<BaseTag, BaseCombat, Transform, NetId, Weapon>(
            [&](BaseTag&, BaseCombat& bc, Transform& tr, NetId& id, Weapon& w) {
                const auto e = EntityOf(id.value);
                if (bc.state != BaseState::Active) { TickWeaponCooldowns(e, dt); return; }
                const OwnerId* o = m_world.GetComponent<OwnerId>(e);
                const uint32_t tgt = NearestHostileInRange(id.value, tr.pos, o ? o->player : 0, static_cast<double>(w.range));
                if (tgt) FireWeapons(e, id.value, tr.pos, o ? o->player : 0, tgt, dt, projs, kills);
                else     TickWeaponCooldowns(e, dt);
            });

        for (const auto& p : projs) SpawnProjectileEntity(p);
        ProcessKills(kills);
    }

    // Advance every projectile in N local sub-steps; on intercept resolve damage (C),
    // else expire on ttl/miss. Anti-tunneling: the swept intercept catches a fast shot
    // that a single tick-boundary check would skip past (area D).
    void ProjectileSystem(float dt)
    {
        std::vector<uint32_t> expired;
        std::vector<KillCand> kills;
        m_world.ForEach<Projectile, Transform, NetId>([&](Projectile& pr, Transform& tr, NetId& id) {
            const auto te = EntityOf(pr.targetNetId);
            Transform*     tt  = m_world.GetComponent<Transform>(te);
            DefenseLayers* td  = m_world.GetComponent<DefenseLayers>(te);
            ResistProfile* trp = m_world.GetComponent<ResistProfile>(te);
            if (!tt || !td || !trp || !m_world.IsAlive(te)) {
                // Target gone — keep flying ballistically until ttl lapses, then expire.
                tr.pos.x += static_cast<int64_t>(std::llround(static_cast<double>(pr.vel.x) * dt));
                tr.pos.y += static_cast<int64_t>(std::llround(static_cast<double>(pr.vel.y) * dt));
                tr.pos.z += static_cast<int64_t>(std::llround(static_cast<double>(pr.vel.z) * dt));
                pr.ttl -= dt;
                if (pr.ttl <= 0.0f) expired.push_back(id.value);
                return;
            }
            const HullInfo* thi = m_world.GetComponent<HullInfo>(te);
            const double hitRadius = std::max<double>(thi ? static_cast<double>(thi->signature) : 100.0, 80.0);
            const SubStepResult r = StepProjectile(tr.pos, pr.vel, tt->pos, tt->pos, pr.ttl, dt, hitRadius, m_projectileSubSteps);
            if (r.hit) {
                const EwarStatus* tes = m_world.GetComponent<EwarStatus>(te);
                const float tgtSig   = thi ? thi->signature : 100.0f;
                const float tgtSpeed = (thi ? thi->maxSpeed : 0.0f) * (tes ? tes->webFactor : 1.0f);
                const double dist = UniverseDistance(pr.origin, tr.pos);
                const float eff = ResolveShotDamage(pr.damageType, pr.baseDamage, dist, pr.optimal, pr.falloff,
                                                    pr.tracking, tgtSig, tgtSpeed);
                if (ApplyTypedDamage(*td, *trp, pr.damageType, eff) == DamageOutcome::Killed)
                    kills.push_back({ pr.targetNetId, pr.sourceNetId });
                expired.push_back(id.value);
            } else if (pr.ttl <= 0.0f) {
                expired.push_back(id.value);
            }
        });
        for (uint32_t pid : expired) DestroyUnit(pid); // projectiles aren't NPCs → just removed
        ProcessKills(kills);
    }

    // Resolve queued kills once (deduped: a victim destroyed by the first blow is skipped
    // for later overkill entries). Bases are NEVER killed (disable-not-destroy, area G).
    void ProcessKills(const std::vector<KillCand>& kills)
    {
        for (const auto& k : kills) {
            const auto e = EntityOf(k.victim);
            if (!m_world.IsAlive(e)) continue;
            const DefenseLayers* d = m_world.GetComponent<DefenseLayers>(e);
            if (!d || d->hull.cur > 0) continue; // not actually dead
            OnUnitKilled(k.victim, k.killer);
        }
    }

    // --- combat: shield regen + EWAR decay (area C/E) -----------------------
    void RegenSystem(float dt)
    {
        m_world.ForEach<DefenseLayers>([&](DefenseLayers& d) { RegenDefenses(d, dt); });
        m_world.ForEach<EwarStatus>([&](EwarStatus& s) { TickEwar(s, dt); });
    }

    // --- combat: base disable-not-destroy (area G, §13.1) -------------------
    void BaseRetreatSystem()
    {
        m_world.ForEach<BaseTag, BaseCombat, DefenseLayers, Storage, NetId>(
            [&](BaseTag&, BaseCombat& bc, DefenseLayers& d, Storage& st, NetId& id) {
                if (bc.state == BaseState::Active && d.HullFrac() <= m_baseRetreatHullFrac) {
                    // Forced emergency jump: retreat, stabilise hull (never destroyed),
                    // lose cargo (an economy event, outbox), raise the low-hull alert.
                    bc.state        = BaseState::Retreating;
                    bc.retreatUntil = static_cast<float>(m_simTime) + m_baseRetreatSeconds;
                    d.hull.cur      = std::max(d.hull.cur, d.hull.max / 4); // survives the jump
                    if (!bc.cargoLost) {
                        int32_t lost = 0;
                        for (int i = 0; i < kResourceSlots; ++i) { lost += static_cast<int32_t>(st.amount[i]); st.amount[i] = 0.0f; }
                        bc.cargoLost = true;
                        m_econEvents.push_back({ EconEventType::CargoLost, id.value, 0, lost, m_tick });
                    }
                    m_lowHullAlerts.push_back(id.value); // area H — base low-hull → retreat alert
                } else if (bc.state == BaseState::Retreating && static_cast<float>(m_simTime) >= bc.retreatUntil) {
                    bc.state = BaseState::Disabled; // jump complete; inert but never removed
                }
            });
    }

    // --- combat: loot-on-kill + killmail (area G, §13.2/§15/§24) ------------
    void OnUnitKilled(uint32_t victimNetId, uint32_t killerNetId)
    {
        const auto e = EntityOf(victimNetId);
        if (m_world.HasComponent<BaseTag>(e)) return; // bases are never destroyed (§13.1)

        // Killmail on every kill → KillmailLog row + an offline notification (§24), and
        // an economy event for the outbox (§15).
        uint8_t kind = 0; int32_t value = 0;
        if (const ShapeId* s = m_world.GetComponent<ShapeId>(e)) kind = static_cast<uint8_t>(s->kind);
        if (const DefenseLayers* d = m_world.GetComponent<DefenseLayers>(e)) value = d->TotalMax();
        m_killmails.push_back({ victimNetId, killerNetId, kind, value, m_tick });
        m_econEvents.push_back({ EconEventType::Killmail, victimNetId, killerNetId, value, m_tick });

        // Loot-on-kill: a destroyed SHIP drops a recoverable container (economy event).
        if (m_world.HasComponent<ShipTag>(e)) SpawnLoot(victimNetId);
        DestroyUnit(victimNetId);
    }

    void SpawnLoot(uint32_t victimNetId)
    {
        const auto e = EntityOf(victimNetId);
        const Transform* t = m_world.GetComponent<Transform>(e);
        if (!t) return;
        LootContainer lc;
        float value = 0.0f;
        if (const Cargo* c = m_world.GetComponent<Cargo>(e))
            for (int i = 0; i < kResourceSlots; ++i) { lc.items[i] = c->amount[i] * m_lootFraction; value += lc.items[i]; }
        lc.expiresAt = static_cast<float>(m_simTime) + m_lootExpireSeconds;
        const uint32_t lootId = m_nextNetId++;
        auto le = m_world.CreateEntity();
        m_world.AddComponent<Transform>(le).pos = t->pos;
        m_world.AddComponent<NetId>(le).value   = lootId;
        m_world.AddComponent<ShapeId>(le)       = { LootShapeId(), EntityKind::LootContainer };
        m_world.AddComponent<LootContainer>(le) = lc;
        m_netIdToEntity[lootId] = le;
        m_econEvents.push_back({ EconEventType::LootDrop, lootId, victimNetId, static_cast<int32_t>(value), m_tick });
    }

    // Remove a destroyed unit; if an NPC guardian, decrement its site and fire the
    // "cleared" hook once the last guardian dies. Low-level (loot/killmail is OnUnitKilled).
    void DestroyUnit(uint32_t netId)
    {
        const auto e = EntityOf(netId);
        if (NpcAi* ai = m_world.GetComponent<NpcAi>(e)) {
            auto it = m_siteAlive.find(ai->siteId);
            if (it != m_siteAlive.end() && --it->second <= 0) {
                it->second = 0;
                m_clearedSites.push_back(ai->siteId);
            }
        }
        if (m_world.IsAlive(e)) m_world.DestroyEntity(e);
        m_netIdToEntity.erase(netId);
        m_interest.Remove(netId); // drop residency (area A); TombstonesFor evicts it per client (area D)
        m_repl.Remove(netId);     // drop replication version (area B)
    }

    // The net id a unit is currently ordered to Attack (0 = none). Used by EWAR target
    // selection (area E) so a fleet's EWAR follows its primary call.
    [[nodiscard]] uint32_t AttackTargetOf(uint32_t netId)
    {
        const FleetOrder* fo = m_world.GetComponent<FleetOrder>(EntityOf(netId));
        return (fo && fo->current.type == OrderType::Attack) ? fo->current.targetNetId : 0;
    }
    [[nodiscard]] uint32_t OwnerOf(uint32_t netId)
    {
        const OwnerId* o = m_world.GetComponent<OwnerId>(EntityOf(netId));
        return o ? o->player : 0;
    }

    // Shape for a small loot container (a crate). Projectiles are sim-only; the client
    // maps EntityKind::Projectile → a tracer VFX (area H), so any shape id works.
    static uint16_t LootShapeId()
    {
        const uint16_t id = ShapeIdByName("Crate01");
        return id != kInvalidShapeId ? id : 0;
    }
    static uint16_t ProjShapeId() { return 0; }

    // Sync the single-layer Health MIRROR to the layered defense totals (the wire,
    // SimHash, HUD and the M5 persistence mapping read Health; DefenseLayers is the
    // truth). Deterministic, once per Step before replication is stamped.
    void SyncHealthMirror()
    {
        m_world.ForEach<DefenseLayers, Health>([&](DefenseLayers& d, Health& h) {
            h.hp = d.TotalCur(); h.maxHp = d.TotalMax();
        });
    }

    // --- NPC AI (area F) -----------------------------------------------------

    uint32_t SpawnNpcGuardian(Neuron::Universe::UniversePos pos, uint16_t siteId,
                              std::string_view fitName = kNpcFit)
    {
        const uint32_t netId = m_nextNetId++;
        auto e = m_world.CreateEntity();
        m_world.AddComponent<Transform>(e).pos = pos;
        m_world.AddComponent<Velocity>(e);
        m_world.AddComponent<NetId>(e).value = netId;
        m_world.AddComponent<ShipTag>(e).shipType = 0;
        m_world.AddComponent<ShapeId>(e) = { ShipShapeId(), EntityKind::NpcUnit };
        m_world.AddComponent<OwnerId>(e).player = 0; // unowned = NPC
        m_world.AddComponent<FleetOrder>(e);
        // NPCs fight with REAL fits (area F): difficulty scales by which fit is spawned.
        if (!InstallFit(e, fitName)) InstallFit(e, kNpcFit);
        auto& ai = m_world.AddComponent<NpcAi>(e);
        ai.state      = AiState::Defend;
        ai.home       = pos;
        ai.aggroRange = kNpcAggroRange;
        ai.fleeHpFrac = kNpcFleeHpFrac;
        ai.siteId     = siteId;
        m_netIdToEntity[netId] = e;
        return netId;
    }

    // Combat target priority (combat-balance.md §4): a hostile carrying logistics is
    // the primary (2), then EWAR (1), then anything else (0) — so NPCs "primary the
    // logi/EWAR" the same way the balance gate rewards a player fleet for doing.
    [[nodiscard]] int TargetCombatPriority(uint32_t netId)
    {
        const Fitting* f = m_world.GetComponent<Fitting>(EntityOf(netId));
        if (!f) return 0;
        bool logi = false, ewar = false;
        for (const auto& mi : f->modules) {
            if (mi.def.kind == ModuleKind::RemoteRep) logi = true;
            else if (mi.def.kind == ModuleKind::Jammer || mi.def.kind == ModuleKind::Web ||
                     mi.def.kind == ModuleKind::WarpDisruptor || mi.def.kind == ModuleKind::SensorDamp) ewar = true;
        }
        return logi ? 2 : (ewar ? 1 : 0);
    }

    // Drive every NPC with the FULL combat model (area F): pick a primary by target
    // priority (logi → EWAR → nearest), update patrol/aggro/flee/defend (flee on the
    // HULL danger threshold, not a flat bar), and write the NPC's FleetOrder so the
    // shared EWAR/logi + weapon + movement systems carry it out with real fits.
    void AiSystem(float dt)
    {
        (void)dt;
        struct Target { uint32_t netId; Neuron::Universe::UniversePos pos; int prio; };
        std::vector<Target> targets;
        m_world.ForEach<OwnerId, Transform, NetId, DefenseLayers>(
            [&](OwnerId& o, Transform& t, NetId& id, DefenseLayers& d) {
                if (o.player != 0 && d.hull.cur > 0) targets.push_back({ id.value, t.pos, TargetCombatPriority(id.value) });
            });

        m_world.ForEach<NpcAi, Transform, DefenseLayers, FleetOrder>(
            [&](NpcAi& ai, Transform& tr, DefenseLayers& d, FleetOrder& fo) {
                uint32_t nearestId = 0; double nearestDist = 0.0;
                uint32_t primaryId = 0; int primaryPrio = -1; double primaryDist = 0.0;
                for (const Target& t : targets) {
                    const double dd = UniverseDistance(tr.pos, t.pos);
                    if (nearestId == 0 || dd < nearestDist) { nearestId = t.netId; nearestDist = dd; }
                    if (dd <= static_cast<double>(ai.aggroRange) &&
                        (t.prio > primaryPrio || (t.prio == primaryPrio && (primaryId == 0 || dd < primaryDist)))) {
                        primaryId = t.netId; primaryPrio = t.prio; primaryDist = dd;
                    }
                }
                const bool  hasTarget     = nearestId != 0;
                const bool  targetInAggro = primaryId != 0;
                const float hullFrac      = d.HullFrac(); // flee on hull, not a flat bar

                ai.state = NextAiState(ai, hasTarget, targetInAggro, hullFrac);
                ai.targetNetId = targetInAggro ? primaryId : 0;
                switch (ai.state) {
                case AiState::Aggro:
                    fo.current = { OrderType::Attack, primaryId, {}, 0.0f };
                    break;
                case AiState::Flee:
                case AiState::Defend:
                case AiState::Patrol:
                    fo.current = { OrderType::Move, 0, ai.home, 0.0f }; // hold/return home
                    break;
                }
            });
    }

    // Nearest beacon to 'pos'; returns its dataset index if within 'range' metres, else -1.
    [[nodiscard]] int NearestBeaconWithin(const Neuron::Universe::UniversePos& pos, float range) const
    {
        int best = -1; double bestDist = 0.0;
        for (size_t i = 0; i < m_universe.beacons.size(); ++i) {
            const double d = UniverseDistance(pos, m_universe.beacons[i].pos);
            if (best < 0 || d < bestDist) { best = static_cast<int>(i); bestDist = d; }
        }
        return (best >= 0 && bestDist <= static_cast<double>(range)) ? best : -1;
    }

    // Interest prefetch (R21): at warp/jump start, the destination sector's
    // interest set would be prefetched so fast cross-sector travel doesn't stall
    // replication. M3 keeps the full-snapshot path, so this just records the
    // target sector; full interest management is M4.
    void OnTravelStart(const Neuron::Universe::UniversePos& dest) noexcept
    {
        m_lastTravelSector = Neuron::Universe::UniverseToSector(dest);
    }

    // Pre-subscribe the travelling unit's owner to the destination cells (R21), so
    // a warp/jump that crosses sectors faster than a tick has its target already
    // replicated on arrival. Pinned until the owner's sensor neighbourhood reaches
    // it (InterestGrid::SetSubscription unpins on arrival).
    void PrefetchTravelInterest(uint32_t netId, const Neuron::Universe::UniversePos& dest)
    {
        const OwnerId* o = m_world.GetComponent<OwnerId>(EntityOf(netId));
        if (!o || o->player == 0) return;
        const Sensor* s = m_world.GetComponent<Sensor>(EntityOf(netId));
        const int radius = s ? Neuron::Sim::SectorRadiusForRange(s->range) : 1;
        m_interest.PreSubscribe(o->player, Neuron::Universe::UniverseToSector(dest), radius);
    }

    // Advance every base's build queue; on completion spawn a ship for the owner
    // at the base and record it for the "build complete" feedback hook. Entities
    // are spawned after the iteration so the ECS isn't mutated mid-ForEach.
    void BuildSystem(float dt)
    {
        struct Done { uint32_t player; Neuron::Universe::UniversePos pos; };
        std::vector<Done> done;
        m_world.ForEach<BuildQueue, Storage, OwnerId, Transform>(
            [&](BuildQueue& q, Storage& s, OwnerId& o, Transform& t) {
                if (BuildStep(q, s, m_economy, dt) == BuildResult::Completed)
                    done.push_back({ o.player, t.pos });
            });
        for (const auto& d : done) {
            const uint32_t ship = SpawnFleetShip(d.player, ShipShapeId(), d.pos);
            if (ship) m_buildCompleted.push_back(ship);
        }
    }

    // Drive each ordered harvester through travel → mine → return → deposit (area C).
    // The pure transfer rules (HarvestStep/DepositAll) live in Economy.h; this just
    // sequences them across the harvester, its target node, and its home base.
    void HarvestSystem(float dt)
    {
        const double maxStep = static_cast<double>(m_economy.harvesterSpeed) * static_cast<double>(dt);
        const double range   = static_cast<double>(m_economy.harvestRange);
        m_world.ForEach<HarvestOrder, Transform, Cargo>([&](HarvestOrder& ord, Transform& tr, Cargo& cargo) {
            if (ord.phase == HarvestPhase::Idle) return;
            Transform*       nodeTr  = m_world.GetComponent<Transform>(EntityOf(ord.nodeNetId));
            ResourceNodeTag* node    = m_world.GetComponent<ResourceNodeTag>(EntityOf(ord.nodeNetId));
            Transform*       baseTr  = m_world.GetComponent<Transform>(EntityOf(ord.baseNetId));
            Storage*         storage = m_world.GetComponent<Storage>(EntityOf(ord.baseNetId));

            switch (ord.phase) {
            case HarvestPhase::ToNode:
                if (!nodeTr || !node || node->remaining <= 0.0f)
                    ord.phase = (SumSlots(cargo.amount) > 0.0f) ? HarvestPhase::ToBase : HarvestPhase::Idle;
                else if (UniverseDistance(tr.pos, nodeTr->pos) <= range)
                    ord.phase = HarvestPhase::Harvesting;
                else
                    StepToward(tr, nodeTr->pos, maxStep);
                break;
            case HarvestPhase::Harvesting:
                if (!node || node->remaining <= 0.0f) {
                    ord.phase = HarvestPhase::ToBase;
                } else {
                    HarvestStep(*node, cargo, m_economy.harvestRate, dt);
                    if (CargoFree(cargo) <= 0.0f) ord.phase = HarvestPhase::ToBase;
                }
                break;
            case HarvestPhase::ToBase:
                if (!baseTr || !storage)
                    ord.phase = HarvestPhase::Idle;
                else if (UniverseDistance(tr.pos, baseTr->pos) <= range)
                    ord.phase = HarvestPhase::Depositing;
                else
                    StepToward(tr, baseTr->pos, maxStep);
                break;
            case HarvestPhase::Depositing:
                if (storage) DepositAll(cargo, *storage);
                ord.phase = (node && node->remaining > 0.0f) ? HarvestPhase::ToNode : HarvestPhase::Idle;
                break;
            case HarvestPhase::Idle:
                break;
            }
        });
    }

    // Pick a node's resource type from a field's composition (deterministic walk).
    [[nodiscard]] static ResourceType PickFieldType(const ResourceFieldDef& f, int i, int count)
    {
        if (f.nodes.empty()) return ResourceType::Ore;
        const double frac = (static_cast<double>(i) + 0.5) / static_cast<double>(count);
        double cum = 0.0;
        for (const auto& w : f.nodes) { cum += static_cast<double>(w.weight); if (frac <= cum) return w.type; }
        return f.nodes.back().type;
    }

    // Spawn harvestable nodes from the cooked dataset's resource fields (area C):
    // a deterministic ring within each field, typed by its composition.
    void SpawnFieldNodes()
    {
        for (const auto& f : m_universe.fields) {
            const int   count = std::clamp(static_cast<int>(f.countMin), 1, 8);
            const float yield = 0.5f * (static_cast<float>(f.yieldMin) + static_cast<float>(f.yieldMax));
            for (int i = 0; i < count; ++i) {
                const double ang = (2.0 * 3.14159265358979323846 * static_cast<double>(i)) / static_cast<double>(count);
                const int64_t ox = static_cast<int64_t>(std::llround(std::cos(ang) * static_cast<double>(f.radius) * 0.5));
                const int64_t oz = static_cast<int64_t>(std::llround(std::sin(ang) * static_cast<double>(f.radius) * 0.5));
                SpawnResourceNode(PickFieldType(f, i, count), yield,
                                  { f.center.x + ox, f.center.y, f.center.z + oz });
            }
        }
    }

    // Dev seed (live only): two linked jump beacons + a harvestable resource node +
    // a small starter fleet near the spawn, so the M3 navigation/economy entities
    // are visible in the client before the cooked universe + command UI (B/C/G) land.
    void SpawnDemoSeed()
    {
        const int64_t bx = Neuron::Universe::kSectorSize - 200; // matches ServerHost's first spawn

        // Two linked public beacons flanking the cluster — real jumps work on them.
        UniverseDataset demo;
        RegionDef reg; reg.name = "DEMO"; reg.security = SecurityTier::High; reg.yieldMult = 1.0f;
        demo.regions.push_back(reg);
        BeaconDef gw; gw.name = "DEMO_GATE_W"; gw.region = "DEMO"; gw.pos = { bx - 900, 0, 260 }; gw.links = { "DEMO_GATE_E" };
        BeaconDef ge; ge.name = "DEMO_GATE_E"; ge.region = "DEMO"; ge.pos = { bx + 900, 0, 260 }; ge.links = { "DEMO_GATE_W" };
        demo.beacons.push_back(gw);
        demo.beacons.push_back(ge);
        // Snappy self-contained economy so the eXploit loop visibly runs (ore-only build).
        demo.economy.cargoCapacity  = 200.0f;
        demo.economy.harvestRate    = 200.0f;
        demo.economy.harvesterSpeed = 1500.0f;
        demo.economy.harvestRange   = 600.0f;
        demo.economy.buildOreCost   = 200.0f;
        demo.economy.buildIceCost   = 0.0f;
        demo.economy.buildSeconds   = 6.0f;
        LoadUniverse(demo);

        // An autonomous mini-economy below the cluster: a station + a harvester that
        // mines an ore asteroid, returns, deposits, and the station builds a ship.
        const uint32_t demoBase = SpawnBase({ bx, -1500, 360 }, { 0, 0, 0 });
        const uint32_t node     = SpawnResourceNode(ResourceType::Ore, 100000.0f, { bx + 1600, -1500, 700 });
        const uint32_t harv     = SpawnFleetShip(demoBase, ShipShapeId(), { bx + 220, -1500, 360 });
        OrderHarvest(harv, node);
        EnqueueBuild(demoBase);

        // A basic guardian site a connecting player can warp their fleet to and
        // clear (§13.7; area F). Placed well clear of the demo economy (> NPC aggro
        // range) so it doesn't molest the autonomous harvester.
        SpawnNpcSite({ bx, -1500, 16000 }, 3, 1500.0f);
    }

    // Populate the universe with a spread of static catalog props clustered around
    // the player spawn point so the client exercises the whole shape catalog (a
    // jumpgate, a few stations, asteroids, debris and a sampling of ship hulls).
    void SpawnScenery()
    {
        const int64_t bx = Neuron::Universe::kSectorSize - 200;
        struct Placement { const char* name; int64_t dx, dy, dz; };
        static constexpr Placement kProps[] = {
            { "Jumpgate01",            0,   0,  380 }, // big landmark dead ahead
            { "Science01",          -260,   0,  120 },
            { "Mining01",            260,   0,  120 },
            { "Asteroid01Rock",     -180,  70,  250 },
            { "Asteroid04Ice",       180, -50,  280 },
            { "Asteroid06Lava",        0,  90,  320 },
            { "Satellite01",        -300,  60,  200 },
            { "Crate01",             120,   0,   60 },
            { "DebrisGenericWreck01", -120,  0,   80 },
            { "HullFreighter",       220,   0,  150 },
            { "HullShuttle",        -220,   0,  150 },
            { "HullAurora",           60,  40,  220 },
        };
        for (const auto& p : kProps) {
            const uint16_t id = ShapeIdByName(p.name);
            if (id != kInvalidShapeId)
                SpawnProp(id, { bx + p.dx, p.dy, p.dz });
        }
    }

    Neuron::ECS::World m_world;
    std::unordered_map<uint32_t, Neuron::ECS::EntityHandle> m_netIdToEntity;
    UniverseDataset                            m_universe;
    NavTuning                                  m_nav{};
    EconomyTuning                              m_economy{};
    std::vector<uint32_t>                      m_buildCompleted;
    std::unordered_map<uint16_t, uint32_t>     m_beaconEntity; // beaconIndex → netId
    std::unordered_map<std::string, uint16_t>  m_beaconName;   // beacon name → beaconIndex
    Neuron::Universe::SectorId                 m_lastTravelSector{};
    Neuron::Sim::InterestGrid                  m_interest; // cell pub/sub interest (area A)
    Neuron::Sim::ReplicationStamps             m_repl;     // per-entity versions (area B)
    std::unordered_map<uint32_t, Neuron::Sim::ClientBaseline> m_baselines; // player → baseline (area B)
    // Snapshot scheduler (area E): per-client delta-base cache + last-sent staleness
    // clock, plus the named-priority tunables and the R16 visible-entity cap.
    std::unordered_map<uint32_t, Neuron::Sim::ClientKnownState> m_known;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> m_lastSent; // client → netId → tick
    Neuron::Sim::RelevanceWeights              m_weights{};
    size_t                                     m_visibleCap{ 256 }; // R16 hard cap (tunable)
    // NPC sites (area F): site id → guardians still alive; cleared sites pending drain.
    std::unordered_map<uint16_t, int>          m_siteAlive;
    std::vector<uint16_t>                      m_clearedSites;
    uint16_t                                   m_nextSiteId{ 1 };
    // Fog (area E): per-player permanently-revealed contacts + in-progress scan dwell.
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> m_revealed;
    std::unordered_map<uint64_t, float>        m_scanDwell; // (player<<32 | target) → seconds
    // M6 combat model (areas A–G).
    CombatCatalog          m_combat{ DefaultCombatCatalog() }; // active balance data (area A)
    double                 m_simTime{ 0.0 };                   // accumulated sim seconds (loot/retreat clocks)
    int                    m_projectileSubSteps{ 4 };          // area D — local sub-steps/tick (tunable)
    float                  m_baseRetreatHullFrac{ 0.15f };     // area G — base retreats below this hull frac
    float                  m_baseRetreatSeconds{ 30.0f };      // area G — retreat → disabled cooldown
    float                  m_lootFraction{ 0.5f };             // area G — victim cargo fraction dropped
    float                  m_lootExpireSeconds{ 300.0f };
    bool                   m_combatEnabled{ true };            // M4 load harness sets false
    std::vector<EconEvent> m_econEvents;   // loot/kill/cargo-loss → M5 outbox (drained, §15)
    std::vector<Killmail>  m_killmails;     // KillmailLog rows + §24 notifications (drained)
    std::vector<uint32_t>  m_lowHullAlerts; // base low-hull → retreat alerts (area H, drained)
    uint32_t m_nextNetId{ 1 };
    uint32_t m_tick{ 0 };
};

} // namespace Neuron::Sim
