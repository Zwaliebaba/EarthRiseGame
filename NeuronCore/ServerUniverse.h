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

#include "Command.h"
#include "Components.h"
#include "Economy.h"
#include "Fleet.h"
#include "Interest.h"
#include "Movement.h"
#include "Navigation.h"
#include "ShapeCatalog.h"
#include "Snapshot.h"
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
        m_world.AddComponent<Health>(e) = { 1000, 1000 };
        m_world.AddComponent<ShapeId>(e) = { BaseShapeId(), EntityKind::Base };
        m_world.AddComponent<Fuel>(e)    = { m_nav.baseFuelMax, m_nav.baseFuelMax };
        m_world.AddComponent<NavState>(e);
        m_world.AddComponent<OwnerId>(e).player   = netId;    // a player ≈ their base
        m_world.AddComponent<Storage>(e).capacity = m_economy.storageCapacity;
        m_world.AddComponent<BuildQueue>(e);
        m_world.AddComponent<Sensor>(e).range     = m_economy.sensorRangeBase;
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
        m_world.AddComponent<Health>(e)  = { kShipHp, kShipHp };
        m_world.AddComponent<FleetOrder>(e);
        m_world.AddComponent<Weapon>(e)  = { kShipWeaponRange, kShipWeaponDps }; // placeholder (M6)
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

    // --- basic PvE NPC site (§13.7; area F) ----------------------------------

    // Spawn a hand-placed guardian site: 'count' NPC units in a deterministic ring
    // of 'radius' around 'center', each defending the site. Returns a site id; the
    // site is "cleared" once every guardian is destroyed (DrainClearedSites). NPCs
    // are server ECS entities (OwnerId.player == 0), distinct from ERHeadless bots.
    uint16_t SpawnNpcSite(Neuron::Universe::UniversePos center, int count, float radius = 1200.0f)
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
            SpawnNpcGuardian(pos, siteId);
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
    // AI sets NPC orders, fleet orders steer, combat applies damage + removes the
    // dead, then movement/navigation/economy integrate.
    void Step(float dtSeconds)
    {
        AiSystem(dtSeconds);
        FleetOrderSystem(dtSeconds);
        CombatSystem(dtSeconds);
        MovementSystem(m_world, dtSeconds);
        NavigationSystem(m_world, m_nav, dtSeconds);
        HarvestSystem(dtSeconds);
        BuildSystem(dtSeconds);
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
            if (vi >= visible.size() || visible[vi] != n) base.Tombstone(n);
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

    static constexpr float kMaxBaseSpeed = 50.0f; // m/s cap (server validates intents)

    // Placeholder fleet/combat balance (§13.7) — flat damage to Health, no fitting/
    // resists; the real model + data-driven tuning land at M6. Kept as named code
    // constants on purpose: M3 only needs "a fleet can clear a basic NPC site".
    static constexpr float   kFleetMoveSpeed  = 2000.0f; // m/s commanded-ship sublight
    static constexpr int32_t kShipHp          = 500;
    static constexpr float   kShipWeaponRange = 1500.0f;
    static constexpr float   kShipWeaponDps   = 60.0f;
    static constexpr int32_t kNpcHp           = 300;
    static constexpr float   kNpcWeaponRange  = 1400.0f;
    static constexpr float   kNpcWeaponDps    = 20.0f;
    static constexpr float   kNpcAggroRange   = 6000.0f;
    static constexpr float   kNpcFleeHpFrac   = 0.15f;
    static constexpr float   kScanSeconds     = 3.0f; // dwell to reveal a contact (area E)

private:
    [[nodiscard]] Neuron::ECS::EntityHandle EntityOf(uint32_t netId) const
    {
        auto it = m_netIdToEntity.find(netId);
        return it == m_netIdToEntity.end() ? Neuron::ECS::EntityHandle::Null() : it->second;
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
        const double maxStep = static_cast<double>(kFleetMoveSpeed) * static_cast<double>(dt);
        m_world.ForEach<FleetOrder, Transform, NetId>([&](FleetOrder& fo, Transform& tr, NetId& selfId) {
            FleetOrderEntry& o = fo.current;
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

    // Apply weapon damage from every unit whose current order is Attack to its
    // target (if a live target is within weapon range). Dead targets are removed
    // after the pass (so the ECS isn't mutated mid-ForEach); NPC deaths decrement
    // their site's alive count and fire the "cleared" hook.
    void CombatSystem(float dt)
    {
        struct Hit { uint32_t target; int32_t dmg; };
        std::vector<Hit> hits;
        m_world.ForEach<FleetOrder, Weapon, Transform, NetId>(
            [&](FleetOrder& fo, Weapon& w, Transform& tr, NetId&) {
                if (fo.current.type != OrderType::Attack) return;
                const auto tgt = EntityOf(fo.current.targetNetId);
                Transform* tt = m_world.GetComponent<Transform>(tgt);
                Health*    th = m_world.GetComponent<Health>(tgt);
                if (!tt || !th || !m_world.IsAlive(tgt)) return;
                if (!InWeaponRange(w, tr.pos, tt->pos)) return;
                const int32_t dmg = WeaponDamage(w, dt); // advances fractional 'pending'
                if (dmg > 0) hits.push_back({ fo.current.targetNetId, dmg });
            });

        std::vector<uint32_t> killed;
        for (const Hit& h : hits) {
            Health* th = m_world.GetComponent<Health>(EntityOf(h.target));
            if (th && th->hp > 0 && ApplyDamage(*th, h.dmg)) killed.push_back(h.target);
        }
        for (uint32_t netId : killed) DestroyUnit(netId);
    }

    // Remove a destroyed unit; if it was an NPC guardian, decrement its site and
    // record the site as cleared once its last guardian dies. (Loot-on-kill is M6.)
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

    // --- NPC AI (area F) -----------------------------------------------------

    uint32_t SpawnNpcGuardian(Neuron::Universe::UniversePos pos, uint16_t siteId)
    {
        const uint32_t netId = m_nextNetId++;
        auto e = m_world.CreateEntity();
        m_world.AddComponent<Transform>(e).pos = pos;
        m_world.AddComponent<Velocity>(e);
        m_world.AddComponent<NetId>(e).value = netId;
        m_world.AddComponent<ShipTag>(e).shipType = 0;
        m_world.AddComponent<ShapeId>(e) = { ShipShapeId(), EntityKind::NpcUnit };
        m_world.AddComponent<OwnerId>(e).player = 0; // unowned = NPC
        m_world.AddComponent<Health>(e)  = { kNpcHp, kNpcHp };
        m_world.AddComponent<FleetOrder>(e);
        m_world.AddComponent<Weapon>(e)  = { kNpcWeaponRange, kNpcWeaponDps };
        auto& ai = m_world.AddComponent<NpcAi>(e);
        ai.state      = AiState::Defend;
        ai.home       = pos;
        ai.aggroRange = kNpcAggroRange;
        ai.fleeHpFrac = kNpcFleeHpFrac;
        ai.siteId     = siteId;
        m_netIdToEntity[netId] = e;
        return netId;
    }

    // Drive every NPC: pick the nearest hostile (player-owned) unit, update the
    // patrol/aggro/flee/defend state (pure NextAiState), and write the NPC's
    // FleetOrder so the shared movement + combat systems carry it out.
    void AiSystem(float dt)
    {
        (void)dt;
        // Snapshot all player-owned combat targets (pos by net id).
        struct Target { uint32_t netId; Neuron::Universe::UniversePos pos; };
        std::vector<Target> targets;
        m_world.ForEach<OwnerId, Transform, NetId, Health>(
            [&](OwnerId& o, Transform& t, NetId& id, Health& h) {
                if (o.player != 0 && h.hp > 0) targets.push_back({ id.value, t.pos });
            });

        m_world.ForEach<NpcAi, Transform, Health, FleetOrder>(
            [&](NpcAi& ai, Transform& tr, Health& hp, FleetOrder& fo) {
                // Nearest hostile.
                uint32_t bestId = 0; double bestDist = 0.0;
                for (const Target& t : targets) {
                    const double d = UniverseDistance(tr.pos, t.pos);
                    if (bestId == 0 || d < bestDist) { bestId = t.netId; bestDist = d; }
                }
                const bool hasTarget    = bestId != 0;
                const bool targetInAggro = hasTarget && bestDist <= static_cast<double>(ai.aggroRange);
                const float hpFrac = hp.maxHp > 0 ? static_cast<float>(hp.hp) / static_cast<float>(hp.maxHp) : 0.0f;

                ai.state = NextAiState(ai, hasTarget, targetInAggro, hpFrac);
                ai.targetNetId = targetInAggro ? bestId : 0;
                switch (ai.state) {
                case AiState::Aggro:
                    fo.current = { OrderType::Attack, bestId, {}, 0.0f };
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
    // NPC sites (area F): site id → guardians still alive; cleared sites pending drain.
    std::unordered_map<uint16_t, int>          m_siteAlive;
    std::vector<uint16_t>                      m_clearedSites;
    uint16_t                                   m_nextSiteId{ 1 };
    // Fog (area E): per-player permanently-revealed contacts + in-progress scan dwell.
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> m_revealed;
    std::unordered_map<uint64_t, float>        m_scanDwell; // (player<<32 | target) → seconds
    uint32_t m_nextNetId{ 1 };
    uint32_t m_tick{ 0 };
};

} // namespace Neuron::Sim
