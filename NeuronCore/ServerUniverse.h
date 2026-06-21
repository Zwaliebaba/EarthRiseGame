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

#include "Components.h"
#include "Movement.h"
#include "Navigation.h"
#include "ShapeCatalog.h"
#include "Snapshot.h"
#include "Ecs.h"
#include "UniverseData.h"
#include "UniversePos.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Neuron::Sim
{

class ServerUniverse
{
public:
    ServerUniverse()
    {
        m_world.RegisterComponent<Transform>();
        m_world.RegisterComponent<Velocity>();
        m_world.RegisterComponent<NetId>();
        m_world.RegisterComponent<BaseTag>();
        m_world.RegisterComponent<Health>();
        m_world.RegisterComponent<ShapeId>();
        m_world.RegisterComponent<Fuel>();
        m_world.RegisterComponent<NavState>();
        m_world.RegisterComponent<BeaconTag>();
        SpawnScenery();
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

    // Remove a player's base from the universe (on disconnect/timeout). Returns
    // true if a base for that net id existed.
    bool DespawnBase(uint32_t netId)
    {
        auto it = m_netIdToEntity.find(netId);
        if (it == m_netIdToEntity.end()) return false;
        m_world.DestroyEntity(it->second);
        m_netIdToEntity.erase(it);
        return true;
    }

    // Advance the simulation one fixed step (movement, then navigation).
    void Step(float dtSeconds)
    {
        MovementSystem(m_world, dtSeconds);
        NavigationSystem(m_world, m_nav, dtSeconds);
        ++m_tick;
    }

    // Build the full snapshot for this tick (interest = everything until M4).
    [[nodiscard]] Snapshot BuildSnapshot()
    {
        Snapshot snap;
        snap.tick = m_tick;
        m_world.ForEach<NetId, Transform, ShapeId>([&snap](NetId& id, Transform& t, ShapeId& s) {
            SnapshotEntity e;
            e.netId       = id.value;
            e.kind        = s.kind;
            e.pos         = t.pos;
            e.localOffset = t.localOffset;
            e.hp          = 1000;
            e.shapeId     = s.value;
            snap.entities.push_back(e);
        });
        return snap;
    }

    [[nodiscard]] uint32_t Tick() const noexcept { return m_tick; }
    [[nodiscard]] Neuron::ECS::World& World() noexcept { return m_world; }

    // Read a base's authoritative position (for tests / diagnostics).
    [[nodiscard]] bool GetBasePos(uint32_t netId, Neuron::Universe::UniversePos& out)
    {
        if (auto* t = m_world.GetComponent<Transform>(EntityOf(netId))) { out = t->pos; return true; }
        return false;
    }

    static constexpr float kMaxBaseSpeed = 50.0f; // m/s cap (server validates intents)

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
    std::unordered_map<uint16_t, uint32_t>     m_beaconEntity; // beaconIndex → netId
    std::unordered_map<std::string, uint16_t>  m_beaconName;   // beacon name → beaconIndex
    Neuron::Universe::SectorId                 m_lastTravelSector{};
    uint32_t m_nextNetId{ 1 };
    uint32_t m_tick{ 0 };
};

} // namespace Neuron::Sim
