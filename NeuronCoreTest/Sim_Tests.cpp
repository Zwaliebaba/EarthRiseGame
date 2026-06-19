#include "CppUnitTest.h"
#include "sim/Components.h"
#include "sim/Movement.h"
#include "sim/Snapshot.h"
#include "ecs/Ecs.h"
#include "world/WorldPos.h"

#include <cmath>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Bind sim component IDs for this TU.
NEURON_DEFINE_COMPONENT(Neuron::Sim::Transform, Neuron::Sim::Slot_Transform);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Velocity,  Neuron::Sim::Slot_Velocity);
NEURON_DEFINE_COMPONENT(Neuron::Sim::BaseTag,   Neuron::Sim::Slot_BaseTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::ShipTag,   Neuron::Sim::Slot_ShipTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::NetId,     Neuron::Sim::Slot_NetId);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Health,    Neuron::Sim::Slot_Health);

TEST_CLASS(MovementTests)
{
public:
    TEST_METHOD(IntegrateStraightLine)
    {
        Neuron::Sim::Transform t;
        Neuron::Sim::Velocity  v;
        v.metresPerSecond = { 10.0f, 0.0f, 0.0f };
        Neuron::Sim::IntegrateMovement(t, v, 1.0f);
        Assert::AreEqual(int64_t(10), t.pos.x);
        Assert::IsTrue(std::fabs(t.localOffset.x) < 1e-4f);
    }

    TEST_METHOD(FractionalCarry)
    {
        Neuron::Sim::Transform t;
        Neuron::Sim::Velocity  v;
        v.metresPerSecond = { 0.5f, 0.0f, 0.0f };
        Neuron::Sim::IntegrateMovement(t, v, 1.0f);
        Assert::AreEqual(int64_t(0), t.pos.x);
        Assert::IsTrue(std::fabs(t.localOffset.x - 0.5f) < 1e-4f);
        Neuron::Sim::IntegrateMovement(t, v, 1.0f);
        Assert::AreEqual(int64_t(1), t.pos.x);
        Assert::IsTrue(std::fabs(t.localOffset.x) < 1e-4f);
    }

    TEST_METHOD(CrossSectorBoundary)
    {
        Neuron::Sim::Transform t;
        t.pos = { Neuron::World::kSectorSize - 5, 0, 0 };
        Neuron::Sim::Velocity v;
        v.metresPerSecond = { 10.0f, 0.0f, 0.0f };
        const auto before = Neuron::World::WorldToSector(t.pos);
        Neuron::Sim::IntegrateMovement(t, v, 1.0f);
        const auto after = Neuron::World::WorldToSector(t.pos);
        Assert::AreEqual(int64_t(0), before.x);
        Assert::AreEqual(int64_t(1), after.x);
    }

    TEST_METHOD(ClampSpeed)
    {
        DirectX::XMFLOAT3 fast{ 100.0f, 0.0f, 0.0f };
        auto clamped = Neuron::Sim::ClampSpeed(fast, 30.0f);
        const float spd = std::sqrt(clamped.x*clamped.x + clamped.y*clamped.y + clamped.z*clamped.z);
        Assert::IsTrue(std::fabs(spd - 30.0f) < 1e-3f);

        DirectX::XMFLOAT3 slow{ 5.0f, 0.0f, 0.0f };
        auto unchanged = Neuron::Sim::ClampSpeed(slow, 30.0f);
        Assert::IsTrue(std::fabs(unchanged.x - 5.0f) < 1e-4f);
    }

    TEST_METHOD(MovementSystemDeterministicOrder)
    {
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

        Assert::AreEqual(int64_t(1), w.GetComponent<Neuron::Sim::Transform>(a)->pos.x);
        Assert::AreEqual(int64_t(2), w.GetComponent<Neuron::Sim::Transform>(b)->pos.y);
    }
};

TEST_CLASS(SnapshotTests)
{
public:
    TEST_METHOD(EncodeDecodeRoundTrip)
    {
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
        Assert::IsTrue(Neuron::Sim::DecodeSnapshot(encoded, decoded));
        Assert::AreEqual(uint32_t(12345), decoded.tick);
        Assert::IsTrue(decoded.entities.size() == 2);

        Assert::AreEqual(uint32_t(1),    decoded.entities[0].netId);
        Assert::IsTrue(decoded.entities[0].kind == Neuron::Sim::EntityKind::Base);
        Assert::AreEqual(int64_t(1000),  decoded.entities[0].pos.x);
        Assert::AreEqual(int64_t(-2000), decoded.entities[0].pos.y);
        Assert::AreEqual(int64_t(3000),  decoded.entities[0].pos.z);
        Assert::IsTrue(std::fabs(decoded.entities[0].localOffset.y - 0.5f) < 1e-5f);
        Assert::AreEqual(int32_t(950),   decoded.entities[0].hp);

        Assert::AreEqual(uint32_t(2),    decoded.entities[1].netId);
        Assert::AreEqual(int64_t(1100),  decoded.entities[1].pos.x);
        Assert::AreEqual(int32_t(80),    decoded.entities[1].hp);
    }

    TEST_METHOD(EmptySnapshot)
    {
        Neuron::Sim::Snapshot snap;
        snap.tick = 7;
        auto encoded = Neuron::Sim::EncodeSnapshot(snap);
        Neuron::Sim::Snapshot decoded;
        Assert::IsTrue(Neuron::Sim::DecodeSnapshot(encoded, decoded));
        Assert::AreEqual(uint32_t(7),  decoded.tick);
        Assert::AreEqual(size_t(0),    decoded.entities.size());
    }

    TEST_METHOD(TruncatedRejected)
    {
        Neuron::Sim::Snapshot snap;
        snap.tick = 1;
        Neuron::Sim::SnapshotEntity e;
        e.netId = 1; e.kind = Neuron::Sim::EntityKind::Base;
        snap.entities.push_back(e);
        auto encoded = Neuron::Sim::EncodeSnapshot(snap);
        encoded.resize(encoded.size() / 2);
        Neuron::Sim::Snapshot decoded;
        Assert::IsFalse(Neuron::Sim::DecodeSnapshot(encoded, decoded));
    }
};
