#include "CppUnitTest.h"
#include "net/FakeCrypto.h"
#include "net/ServerHost.h"
#include "net/ISocket.h"
#include "sim/ServerWorld.h"
#include "sim/Components.h"
#include "sim/Snapshot.h"
#include "world/WorldPos.h"

#include <cmath>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

NEURON_DEFINE_COMPONENT(Neuron::Sim::Transform, Neuron::Sim::Slot_Transform);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Velocity,  Neuron::Sim::Slot_Velocity);
NEURON_DEFINE_COMPONENT(Neuron::Sim::BaseTag,   Neuron::Sim::Slot_BaseTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::ShipTag,   Neuron::Sim::Slot_ShipTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::NetId,     Neuron::Sim::Slot_NetId);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Health,    Neuron::Sim::Slot_Health);

TEST_CLASS(ServerWorldTests)
{
public:
    TEST_METHOD(SpawnBaseReturnsSequentialIds)
    {
        Neuron::Sim::ServerWorld world;
        Assert::AreEqual(uint32_t(1), world.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 }));
        Assert::AreEqual(uint32_t(2), world.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 }));
        Assert::AreEqual(uint32_t(3), world.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 }));
    }

    TEST_METHOD(GetBasePosAfterSpawn)
    {
        Neuron::Sim::ServerWorld world;
        const uint32_t id = world.SpawnBase({ 1000, 2000, 3000 }, { 0, 0, 0 });
        Neuron::World::WorldPos pos{};
        Assert::IsTrue(world.GetBasePos(id, pos));
        Assert::AreEqual(int64_t(1000), pos.x);
        Assert::AreEqual(int64_t(2000), pos.y);
        Assert::AreEqual(int64_t(3000), pos.z);
    }

    TEST_METHOD(GetBasePosReturnsFalseForUnknownId)
    {
        Neuron::Sim::ServerWorld world;
        Neuron::World::WorldPos pos{};
        Assert::IsFalse(world.GetBasePos(99, pos));
    }

    TEST_METHOD(StepMovesBase)
    {
        Neuron::Sim::ServerWorld world;
        const uint32_t id = world.SpawnBase({ 0, 0, 0 }, { 10.0f, 0.0f, 0.0f });
        world.Step(1.0f);
        Neuron::World::WorldPos pos{};
        Assert::IsTrue(world.GetBasePos(id, pos));
        Assert::AreEqual(int64_t(10), pos.x);
    }

    TEST_METHOD(VelocityClampedAtMaxSpeed)
    {
        Neuron::Sim::ServerWorld world;
        // 100 m/s exceeds kMaxBaseSpeed=50; SpawnBase must clamp it
        const uint32_t id = world.SpawnBase({ 0, 0, 0 }, { 100.0f, 0.0f, 0.0f });
        world.Step(1.0f);
        Neuron::World::WorldPos pos{};
        Assert::IsTrue(world.GetBasePos(id, pos));
        Assert::IsTrue(pos.x <= static_cast<int64_t>(Neuron::Sim::ServerWorld::kMaxBaseSpeed));
        Assert::IsTrue(pos.x > 0);
    }

    TEST_METHOD(SetBaseVelocityClamps)
    {
        Neuron::Sim::ServerWorld world;
        const uint32_t id = world.SpawnBase({ 0, 0, 0 }, { 0.0f, 0.0f, 0.0f });
        world.SetBaseVelocity(id, { 200.0f, 0.0f, 0.0f });
        world.Step(1.0f);
        Neuron::World::WorldPos pos{};
        Assert::IsTrue(world.GetBasePos(id, pos));
        Assert::IsTrue(pos.x <= static_cast<int64_t>(Neuron::Sim::ServerWorld::kMaxBaseSpeed));
    }

    TEST_METHOD(BuildSnapshotContainsAllBases)
    {
        Neuron::Sim::ServerWorld world;
        world.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
        world.SpawnBase({ 1000, 0, 0 }, { 0, 0, 0 });
        const auto snap = world.BuildSnapshot();
        Assert::AreEqual(size_t(2), snap.entities.size());
    }

    TEST_METHOD(TickIncreasesAfterStep)
    {
        Neuron::Sim::ServerWorld world;
        Assert::AreEqual(uint32_t(0), world.Tick());
        world.Step(1.0f / 30.0f);
        Assert::AreEqual(uint32_t(1), world.Tick());
        world.Step(1.0f / 30.0f);
        Assert::AreEqual(uint32_t(2), world.Tick());
    }

    TEST_METHOD(BaseCrossesSectorBoundary)
    {
        using namespace Neuron::World;
        Neuron::Sim::ServerWorld world;
        // Start 10 m west of the sector-0/1 boundary; 20 m/s east crosses in 0.5 s = 15 ticks
        const int64_t startX = kSectorSize - 10;
        const uint32_t id = world.SpawnBase({ startX, 0, 0 }, { 20.0f, 0.0f, 0.0f });
        for (int i = 0; i < 20; ++i)   // 20 ticks > 0.5 s
            world.Step(1.0f / 30.0f);
        Neuron::World::WorldPos pos{};
        Assert::IsTrue(world.GetBasePos(id, pos));
        Assert::AreEqual(int64_t(1), WorldToSector(pos).x);
    }
};

TEST_CLASS(ServerHostTests)
{
public:
    TEST_METHOD(InitialConnectionCountsAreZero)
    {
        Neuron::Net::FakeCrypto crypto;
        Neuron::Net::EcPubKey pub{};
        std::vector<uint8_t> priv;
        Neuron::Net::FakeCrypto::MakeFakeStaticKey(priv, pub);
        Neuron::Sim::ServerWorld world;
        Neuron::Net::ServerHost host(&crypto, priv, { 's', 'e', 'c' }, &world, 0);
        Assert::AreEqual(size_t(0), host.ConnectionCount());
        Assert::AreEqual(size_t(0), host.ConnectedCount());
    }

    TEST_METHOD(EmptyDatagramIsIgnored)
    {
        Neuron::Net::FakeCrypto crypto;
        Neuron::Net::EcPubKey pub{};
        std::vector<uint8_t> priv;
        Neuron::Net::FakeCrypto::MakeFakeStaticKey(priv, pub);
        Neuron::Sim::ServerWorld world;
        Neuron::Net::ServerHost host(&crypto, priv, { 's' }, &world, 0);
        Neuron::Net::Endpoint ep;
        ep.ip = "127.0.0.1";
        ep.port = 9001;
        std::vector<Neuron::Net::OutDatagram> out;
        host.OnDatagram(ep, {}, out);
        Assert::AreEqual(size_t(0), out.size());
        Assert::AreEqual(size_t(0), host.ConnectionCount());
    }

    TEST_METHOD(BroadcastWithNoClientsProducesNoDatagrams)
    {
        Neuron::Net::FakeCrypto crypto;
        Neuron::Net::EcPubKey pub{};
        std::vector<uint8_t> priv;
        Neuron::Net::FakeCrypto::MakeFakeStaticKey(priv, pub);
        Neuron::Sim::ServerWorld world;
        Neuron::Net::ServerHost host(&crypto, priv, { 's' }, &world, 0);
        std::vector<Neuron::Net::OutDatagram> out;
        host.BroadcastSnapshots(out);
        Assert::AreEqual(size_t(0), out.size());
    }
};
