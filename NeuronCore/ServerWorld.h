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

    // Advance the simulation one fixed step.
    void Step(float dtSeconds)
    {
        MovementSystem(m_world, dtSeconds);
        ++m_tick;
    }

    // Build the full snapshot for this tick (M1a: identical for all players).
    [[nodiscard]] Snapshot BuildSnapshot()
    {
        Snapshot snap;
        snap.tick = m_tick;
        m_world.ForEach<NetId, Transform>([&snap, this](NetId& id, Transform& t) {
            SnapshotEntity e;
            e.netId       = id.value;
            e.kind        = EntityKind::Base; // M1a: all replicated entities are bases
            e.pos         = t.pos;
            e.localOffset = t.localOffset;
            e.hp          = 1000;
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
    Neuron::ECS::World m_world;
    std::unordered_map<uint32_t, Neuron::ECS::EntityHandle> m_netIdToEntity;
    uint32_t m_nextNetId{ 1 };
    uint32_t m_tick{ 0 };
};

} // namespace Neuron::Sim
