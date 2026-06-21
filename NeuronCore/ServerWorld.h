#pragma once
// ServerWorld — authoritative simulation state for ERServer (§9).
//
// M1a scope: one mobile Base per player, integrated by the shared MovementSystem
// at the fixed 30 Hz step, with a full snapshot built per tick (interest =
// everything until M4 sector subscriptions land). Platform-independent so the
// whole server→client path runs in the loopback integration test.
//
// Component type IDs (Components.h) must be bound via NEURON_DEFINE_COMPONENT in
// exactly one TU per executable: SimComponents.cpp for ERServer/ERHeadless, and
// Tests_Sim.h for the test runner.

#include "Components.h"
#include "Movement.h"
#include "ShapeCatalog.h"
#include "Snapshot.h"
#include "Ecs.h"
#include "WorldPos.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Neuron::Sim
{

class ServerWorld
{
public:
    ServerWorld()
    {
        m_world.RegisterComponent<Transform>();
        m_world.RegisterComponent<Velocity>();
        m_world.RegisterComponent<NetId>();
        m_world.RegisterComponent<BaseTag>();
        m_world.RegisterComponent<Health>();
        m_world.RegisterComponent<ShapeId>();
        SpawnScenery();
    }

    // Shape used for a player's mobile home base (a station hull reads as a base).
    static uint16_t BaseShapeId()
    {
        const uint16_t id = ShapeIdByName("Outpost01");
        return id != kInvalidShapeId ? id : 0; // 0 is always a valid catalog entry
    }

    // Spawn a mobile base for a player. Returns the assigned network id.
    // Bases are placed on a line and given an eastward drift so the M1a test can
    // watch them cross a sector boundary.
    uint32_t SpawnBase(Neuron::World::WorldPos start, DirectX::XMFLOAT3 vel)
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
        m_netIdToEntity[netId] = e;
        return netId;
    }

    // Spawn a static catalog prop (scenery: stations, asteroids, jumpgates, ...).
    // Replicated like any entity (gets a NetId); no Velocity, so it stays put.
    // Kind defaults to the catalog category's kind. Returns the net id.
    uint32_t SpawnProp(uint16_t shapeId, Neuron::World::WorldPos pos)
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
        auto it = m_netIdToEntity.find(netId);
        if (it == m_netIdToEntity.end()) return;
        if (auto* v = m_world.GetComponent<Velocity>(it->second))
            v->metresPerSecond = ClampSpeed(vel, kMaxBaseSpeed);
    }

    // Remove a player's base from the world (on disconnect/timeout). Returns
    // true if a base for that net id existed.
    bool DespawnBase(uint32_t netId)
    {
        auto it = m_netIdToEntity.find(netId);
        if (it == m_netIdToEntity.end()) return false;
        m_world.DestroyEntity(it->second);
        m_netIdToEntity.erase(it);
        return true;
    }

    // Advance the simulation one fixed step.
    void Step(float dtSeconds)
    {
        MovementSystem(m_world, dtSeconds);
        ++m_tick;
    }

    // Build the full snapshot for this tick (interest = everything until M4).
    // Every replicated entity carries a ShapeId (mesh + kind), so the client can
    // render the right catalog mesh per entity.
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
    [[nodiscard]] bool GetBasePos(uint32_t netId, Neuron::World::WorldPos& out)
    {
        auto it = m_netIdToEntity.find(netId);
        if (it == m_netIdToEntity.end()) return false;
        if (auto* t = m_world.GetComponent<Transform>(it->second)) { out = t->pos; return true; }
        return false;
    }

    static constexpr float kMaxBaseSpeed = 50.0f; // m/s cap (server validates intents)

private:
    // Populate the world with a spread of static catalog props near the sector-0
    // origin so the client exercises the whole shape catalog (a jumpgate, a few
    // stations, asteroids, debris and a sampling of ship hulls). Runs once at
    // construction, before any player connects.
    void SpawnScenery()
    {
        struct Placement { const char* name; int64_t x, y, z; };
        // Ring of landmarks around the origin (metres). Names fall back to the
        // first shape in the matching category if an asset was renamed.
        static constexpr Placement kProps[] = {
            { "Jumpgate01",   0,   0,  600 },
            { "Outpost01",  500,   0,    0 },
            { "Science01", -500,   0,    0 },
            { "Mining01",     0,   0, -500 },
            { "Asteroid01Rock",  300, 100,  300 },
            { "Asteroid04Ice",  -300, -80,  300 },
            { "Asteroid06Lava",  300, -60, -300 },
            { "Satellite01",   -300, 120, -300 },
            { "Crate01",        120,  0,  120 },
            { "DebrisGenericWreck01", -150, 0, 150 },
            { "HullFreighter",  250,  0,   80 },
            { "HullShuttle",   -250,  0,   80 },
            { "HullAurora",       0, 50,  250 },
        };
        for (const auto& p : kProps) {
            const uint16_t id = ShapeIdByName(p.name);
            if (id != kInvalidShapeId)
                SpawnProp(id, { p.x, p.y, p.z });
        }
    }

    Neuron::ECS::World m_world;
    std::unordered_map<uint32_t, Neuron::ECS::EntityHandle> m_netIdToEntity;
    uint32_t m_nextNetId{ 1 };
    uint32_t m_tick{ 0 };
};

} // namespace Neuron::Sim
