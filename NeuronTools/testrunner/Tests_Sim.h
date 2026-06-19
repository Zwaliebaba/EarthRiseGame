#pragma once
// M1a unit tests — shared sim: Movement integration + Snapshot codec.

#include "TestRunner.h"
#include "sim/Components.h"
#include "sim/Movement.h"
#include "sim/Snapshot.h"
#include "ecs/Ecs.h"
#include "world/WorldPos.h"

#include <cmath>

// Bind sim component IDs for this TU (one definition site for the test exe;
// the ERServer/ERHeadless build uses sim/SimComponents.cpp instead).
NEURON_DEFINE_COMPONENT(Neuron::Sim::Transform, Neuron::Sim::Slot_Transform);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Velocity,  Neuron::Sim::Slot_Velocity);
NEURON_DEFINE_COMPONENT(Neuron::Sim::BaseTag,   Neuron::Sim::Slot_BaseTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::ShipTag,   Neuron::Sim::Slot_ShipTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::NetId,     Neuron::Sim::Slot_NetId);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Health,    Neuron::Sim::Slot_Health);

TEST_SUITE(Movement)
{
    TEST_CASE(IntegrateStraightLine) {
        Neuron::Sim::Transform t;
        Neuron::Sim::Velocity  v;
        v.metresPerSecond = { 10.0f, 0.0f, 0.0f };

        // 1 second at 10 m/s → +10 m on x.
        Neuron::Sim::IntegrateMovement(t, v, 1.0f);
        CHECK_EQ(t.pos.x, int64_t(10));
        CHECK(std::fabs(t.localOffset.x) < 1e-4f);
    });

    TEST_CASE(FractionalCarry) {
        Neuron::Sim::Transform t;
        Neuron::Sim::Velocity  v;
        v.metresPerSecond = { 0.5f, 0.0f, 0.0f };

        // Two 1 s steps at 0.5 m/s → +1 m total, carried through the float offset.
        Neuron::Sim::IntegrateMovement(t, v, 1.0f);
        CHECK_EQ(t.pos.x, int64_t(0));
        CHECK(std::fabs(t.localOffset.x - 0.5f) < 1e-4f);
        Neuron::Sim::IntegrateMovement(t, v, 1.0f);
        CHECK_EQ(t.pos.x, int64_t(1));
        CHECK(std::fabs(t.localOffset.x) < 1e-4f);
    });

    TEST_CASE(CrossSectorBoundary) {
        // Start near a sector edge; move past it; sector index must increment.
        Neuron::Sim::Transform t;
        t.pos = { Neuron::World::kSectorSize - 5, 0, 0 };
        Neuron::Sim::Velocity v;
        v.metresPerSecond = { 10.0f, 0.0f, 0.0f };

        const auto before = Neuron::World::WorldToSector(t.pos);
        Neuron::Sim::IntegrateMovement(t, v, 1.0f); // +10 m → crosses boundary
        const auto after = Neuron::World::WorldToSector(t.pos);
        CHECK_EQ(before.x, int64_t(0));
        CHECK_EQ(after.x, int64_t(1));
    });

    TEST_CASE(ClampSpeed) {
        DirectX::XMFLOAT3 fast{ 100.0f, 0.0f, 0.0f };
        auto clamped = Neuron::Sim::ClampSpeed(fast, 30.0f);
        const float spd = std::sqrt(clamped.x*clamped.x + clamped.y*clamped.y + clamped.z*clamped.z);
        CHECK(std::fabs(spd - 30.0f) < 1e-3f);

        DirectX::XMFLOAT3 slow{ 5.0f, 0.0f, 0.0f };
        auto unchanged = Neuron::Sim::ClampSpeed(slow, 30.0f);
        CHECK(std::fabs(unchanged.x - 5.0f) < 1e-4f);
    });

    TEST_CASE(MovementSystemDeterministicOrder) {
        Neuron::ECS::World w;
        w.RegisterComponent<Neuron::Sim::Transform>();
        w.RegisterComponent<Neuron::Sim::Velocity>();

        auto a = w.CreateEntity();
        w.AddComponent<Neuron::Sim::Transform>(a);
        auto& va = w.AddComponent<Neuron::Sim::Velocity>(a);
        va.metresPerSecond = { 1.0f, 0.0f, 0.0f };

        auto b = w.CreateEntity();
        w.AddComponent<Neuron::Sim::Transform>(b);
        auto& vb = w.AddComponent<Neuron::Sim::Velocity>(b);
        vb.metresPerSecond = { 0.0f, 2.0f, 0.0f };

        Neuron::Sim::MovementSystem(w, 1.0f);

        CHECK_EQ(w.GetComponent<Neuron::Sim::Transform>(a)->pos.x, int64_t(1));
        CHECK_EQ(w.GetComponent<Neuron::Sim::Transform>(b)->pos.y, int64_t(2));
    });
}

TEST_SUITE(Snapshot)
{
    TEST_CASE(EncodeDecodeRoundTrip) {
        Neuron::Sim::Snapshot snap;
        snap.tick = 12345;

        Neuron::Sim::SnapshotEntity base;
        base.netId = 1;
        base.kind  = Neuron::Sim::EntityKind::Base;
        base.pos   = { 1000, -2000, 3000 };
        base.localOffset = { 0.25f, 0.5f, 0.75f };
        base.hp    = 950;
        snap.entities.push_back(base);

        Neuron::Sim::SnapshotEntity ship;
        ship.netId = 2;
        ship.kind  = Neuron::Sim::EntityKind::Ship;
        ship.pos   = { 1100, -1900, 3100 };
        ship.hp    = 80;
        snap.entities.push_back(ship);

        auto encoded = Neuron::Sim::EncodeSnapshot(snap);

        Neuron::Sim::Snapshot decoded;
        REQUIRE(Neuron::Sim::DecodeSnapshot(encoded, decoded));
        CHECK_EQ(decoded.tick, uint32_t(12345));
        REQUIRE(decoded.entities.size() == 2);

        CHECK_EQ(decoded.entities[0].netId, uint32_t(1));
        CHECK(decoded.entities[0].kind == Neuron::Sim::EntityKind::Base);
        CHECK_EQ(decoded.entities[0].pos.x, int64_t(1000));
        CHECK_EQ(decoded.entities[0].pos.y, int64_t(-2000));
        CHECK_EQ(decoded.entities[0].pos.z, int64_t(3000));
        CHECK(std::fabs(decoded.entities[0].localOffset.y - 0.5f) < 1e-5f);
        CHECK_EQ(decoded.entities[0].hp, int32_t(950));

        CHECK_EQ(decoded.entities[1].netId, uint32_t(2));
        CHECK_EQ(decoded.entities[1].pos.x, int64_t(1100));
        CHECK_EQ(decoded.entities[1].hp, int32_t(80));
    });

    TEST_CASE(EmptySnapshot) {
        Neuron::Sim::Snapshot snap;
        snap.tick = 7;
        auto encoded = Neuron::Sim::EncodeSnapshot(snap);
        Neuron::Sim::Snapshot decoded;
        REQUIRE(Neuron::Sim::DecodeSnapshot(encoded, decoded));
        CHECK_EQ(decoded.tick, uint32_t(7));
        CHECK_EQ(decoded.entities.size(), size_t(0));
    });

    TEST_CASE(TruncatedRejected) {
        Neuron::Sim::Snapshot snap;
        snap.tick = 1;
        Neuron::Sim::SnapshotEntity e;
        e.netId = 1; e.kind = Neuron::Sim::EntityKind::Base;
        snap.entities.push_back(e);
        auto encoded = Neuron::Sim::EncodeSnapshot(snap);
        encoded.resize(encoded.size() / 2); // chop it
        Neuron::Sim::Snapshot decoded;
        CHECK(!Neuron::Sim::DecodeSnapshot(encoded, decoded));
    });
}
