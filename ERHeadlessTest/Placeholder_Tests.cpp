#include "CppUnitTest.h"
#include "net/FakeCrypto.h"
#include "net/LoopbackNetwork.h"
#include "net/ServerHost.h"
#include "net/Connection.h"
#include "net/Protocol.h"
#include "sim/ServerWorld.h"
#include "sim/Components.h"
#include "sim/Snapshot.h"
#include "world/WorldPos.h"

#include <array>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

NEURON_DEFINE_COMPONENT(Neuron::Sim::Transform, Neuron::Sim::Slot_Transform);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Velocity,  Neuron::Sim::Slot_Velocity);
NEURON_DEFINE_COMPONENT(Neuron::Sim::BaseTag,   Neuron::Sim::Slot_BaseTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::ShipTag,   Neuron::Sim::Slot_ShipTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::NetId,     Neuron::Sim::Slot_NetId);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Health,    Neuron::Sim::Slot_Health);

namespace HeadlessTestHelpers
{
    // One round: server drains its queue and sends replies, then client drains and sends replies.
    static void PumpRound(
        Neuron::Net::LoopbackSocket& serverSock,
        Neuron::Net::ServerHost& host,
        Neuron::Net::LoopbackSocket& clientSock,
        Neuron::Net::ClientConnection& conn,
        const Neuron::Net::Endpoint& serverEp,
        std::vector<Neuron::Net::AppMessage>* snapshots = nullptr)
    {
        std::array<uint8_t, 4096> buf;

        // Drain server
        {
            Neuron::Net::Endpoint from;
            int n;
            while ((n = serverSock.RecvFrom(from, buf)) > 0) {
                std::vector<Neuron::Net::OutDatagram> out;
                host.OnDatagram(from, { buf.data(), static_cast<size_t>(n) }, out);
                for (auto& d : out)
                    serverSock.SendTo(d.to, d.data);
            }
        }

        // Drain client
        {
            Neuron::Net::Endpoint from;
            int n;
            while ((n = clientSock.RecvFrom(from, buf)) > 0) {
                std::vector<Neuron::Net::AppMessage> app;
                std::vector<std::vector<uint8_t>> send;
                conn.OnDatagram({ buf.data(), static_cast<size_t>(n) }, app, send);
                for (auto& d : send)
                    clientSock.SendTo(serverEp, d);
                if (snapshots) {
                    for (auto& m : app)
                        if (m.type == Neuron::Net::MsgType::Snapshot)
                            snapshots->push_back(std::move(m));
                }
            }
        }
    }
} // namespace HeadlessTestHelpers

TEST_CLASS(HeadlessMultiClientTests)
{
public:
    TEST_METHOD(SingleClientConnectsViaLoopback)
    {
        Neuron::Net::LoopbackNetwork net;
        Neuron::Net::LoopbackSocket serverSock(&net);
        serverSock.Open(17777);
        Neuron::Net::Endpoint serverEp;
        serverEp.ip = "127.0.0.1";
        serverEp.port = 17777;

        Neuron::Net::FakeCrypto crypto;
        Neuron::Net::EcPubKey pub{};
        std::vector<uint8_t> priv;
        Neuron::Net::FakeCrypto::MakeFakeStaticKey(priv, pub);
        Neuron::Sim::ServerWorld world;
        Neuron::Net::ServerHost host(&crypto, priv, { 's', 'e', 'c' }, &world, 0);

        Neuron::Net::LoopbackSocket clientSock(&net);
        clientSock.Open(18001);
        Neuron::Net::ClientConnection conn(&crypto, pub);

        auto hello = conn.Start(0);
        clientSock.SendTo(serverEp, hello);

        for (int round = 0; round < 10 && !conn.IsConnected(); ++round)
            HeadlessTestHelpers::PumpRound(serverSock, host, clientSock, conn, serverEp);

        Assert::IsTrue(conn.IsConnected());
        Assert::AreEqual(size_t(1), host.ConnectionCount());
        Assert::AreEqual(size_t(1), host.ConnectedCount());
    }

    TEST_METHOD(SnapshotDeliveredAfterConnect)
    {
        Neuron::Net::LoopbackNetwork net;
        Neuron::Net::LoopbackSocket serverSock(&net);
        serverSock.Open(17778);
        Neuron::Net::Endpoint serverEp;
        serverEp.ip = "127.0.0.1";
        serverEp.port = 17778;

        Neuron::Net::FakeCrypto crypto;
        Neuron::Net::EcPubKey pub{};
        std::vector<uint8_t> priv;
        Neuron::Net::FakeCrypto::MakeFakeStaticKey(priv, pub);
        Neuron::Sim::ServerWorld world;
        Neuron::Net::ServerHost host(&crypto, priv, { 's', 'e', 'c' }, &world, 0);

        Neuron::Net::LoopbackSocket clientSock(&net);
        clientSock.Open(18002);
        Neuron::Net::ClientConnection conn(&crypto, pub);

        auto hello = conn.Start(0);
        clientSock.SendTo(serverEp, hello);

        for (int round = 0; round < 10 && !conn.IsConnected(); ++round)
            HeadlessTestHelpers::PumpRound(serverSock, host, clientSock, conn, serverEp);

        Assert::IsTrue(conn.IsConnected());

        // Server advances one tick and broadcasts snapshot
        world.Step(1.0f / 30.0f);
        std::vector<Neuron::Net::OutDatagram> snapOut;
        host.BroadcastSnapshots(snapOut);
        for (auto& d : snapOut)
            serverSock.SendTo(d.to, d.data);

        std::vector<Neuron::Net::AppMessage> received;
        HeadlessTestHelpers::PumpRound(serverSock, host, clientSock, conn, serverEp, &received);

        Assert::IsTrue(!received.empty());
        const auto& msg = received.front();
        Assert::IsTrue(msg.type == Neuron::Net::MsgType::Snapshot);

        Neuron::Sim::Snapshot snap;
        Assert::IsTrue(Neuron::Sim::DecodeSnapshot(msg.body, snap));
        Assert::AreEqual(uint32_t(1), snap.tick);
        Assert::AreEqual(size_t(1), snap.entities.size()); // one base spawned for this client
    }

    TEST_METHOD(TwoClientsConnectIndependently)
    {
        Neuron::Net::LoopbackNetwork net;
        Neuron::Net::LoopbackSocket serverSock(&net);
        serverSock.Open(17779);
        Neuron::Net::Endpoint serverEp;
        serverEp.ip = "127.0.0.1";
        serverEp.port = 17779;

        Neuron::Net::FakeCrypto crypto;
        Neuron::Net::EcPubKey pub{};
        std::vector<uint8_t> priv;
        Neuron::Net::FakeCrypto::MakeFakeStaticKey(priv, pub);
        Neuron::Sim::ServerWorld world;
        Neuron::Net::ServerHost host(&crypto, priv, { 's', 'e', 'c' }, &world, 0);

        Neuron::Net::LoopbackSocket sock1(&net);
        sock1.Open(18003);
        Neuron::Net::ClientConnection conn1(&crypto, pub);

        Neuron::Net::LoopbackSocket sock2(&net);
        sock2.Open(18004);
        Neuron::Net::ClientConnection conn2(&crypto, pub);

        sock1.SendTo(serverEp, conn1.Start(0));
        sock2.SendTo(serverEp, conn2.Start(0));

        for (int r = 0; r < 10 && (!conn1.IsConnected() || !conn2.IsConnected()); ++r) {
            HeadlessTestHelpers::PumpRound(serverSock, host, sock1, conn1, serverEp);
            HeadlessTestHelpers::PumpRound(serverSock, host, sock2, conn2, serverEp);
        }

        Assert::IsTrue(conn1.IsConnected());
        Assert::IsTrue(conn2.IsConnected());
        Assert::AreEqual(size_t(2), host.ConnectedCount());

        // Each client gets its own base in the snapshot
        std::vector<Neuron::Net::OutDatagram> snapOut;
        host.BroadcastSnapshots(snapOut);
        Assert::AreEqual(size_t(2), snapOut.size()); // one snapshot datagram per connected client
    }
};

TEST_CLASS(LoopbackNetworkTests)
{
public:
    TEST_METHOD(ZeroLossDeliversAllDatagrams)
    {
        Neuron::Net::LoopbackNetwork net;
        net.Configure({ 0.0, 0.0, 0, 42 });

        Neuron::Net::LoopbackSocket sender(&net);
        sender.Open(19001);
        Neuron::Net::LoopbackSocket receiver(&net);
        receiver.Open(19002);

        Neuron::Net::Endpoint dest;
        dest.ip = "127.0.0.1";
        dest.port = 19002;
        const std::array<uint8_t, 4> payload{ 0xDE, 0xAD, 0xBE, 0xEF };

        for (int i = 0; i < 5; ++i)
            sender.SendTo(dest, payload);

        int received = 0;
        std::array<uint8_t, 16> buf;
        Neuron::Net::Endpoint from;
        while (receiver.RecvFrom(from, buf) > 0)
            ++received;

        Assert::AreEqual(5, received);
    }

    TEST_METHOD(ConfigureWithImpairmentsDoesNotCrash)
    {
        Neuron::Net::LoopbackNetwork net;
        Neuron::Net::Impairments imp;
        imp.lossProbability = 0.1;
        imp.dupProbability  = 0.05;
        imp.reorderDepth    = 3;
        imp.seed            = 9999;
        net.Configure(imp);

        Neuron::Net::LoopbackSocket sock(&net);
        sock.Open(19003);
        Neuron::Net::Endpoint dest;
        dest.ip = "127.0.0.1";
        dest.port = 19003;
        const std::array<uint8_t, 2> data{ 0xAB, 0xCD };
        sock.SendTo(dest, data); // loopback to self
        net.Step(); // flush hold buffer
        // Just verify no crash and we can receive
        std::array<uint8_t, 8> buf;
        Neuron::Net::Endpoint from;
        sock.RecvFrom(from, buf); // may or may not have something due to loss
        Assert::IsTrue(true);    // no crash = pass
    }
};
