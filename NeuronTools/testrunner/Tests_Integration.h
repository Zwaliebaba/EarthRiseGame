#pragma once
// M1a end-to-end integration test — the headless acceptance criterion (§17):
//   "≥3 bots see the base cross a sector boundary under simulated loss/reorder/dup;
//    MITM/replay tests pass."
//
// Wires the real ServerHost + ServerWorld and N real ClientConnections over the
// in-memory LoopbackNetwork, driving the full path:
//   stateless cookie → version gate → ECDH(+sig verify) → clock sync →
//   login → 30 Hz sim with a drifting base → per-tick snapshots → client replica.
//
// Runs clean, then again under loss + duplication + reorder. Depends on the sim
// component IDs defined in Tests_Sim.h (same executable).

#include "TestRunner.h"
#include "net/Connection.h"
#include "net/FakeCrypto.h"
#include "net/LoopbackNetwork.h"
#include "net/ServerHost.h"
#include "sim/Command.h"
#include "sim/ServerWorld.h"
#include "sim/Snapshot.h"
#include "world/WorldPos.h"

#include <array>
#include <cstdint>
#include <set>
#include <vector>

namespace
{

struct BotClient
{
    std::unique_ptr<Neuron::Net::LoopbackSocket> sock;
    std::unique_ptr<Neuron::Net::ClientConnection> conn;
    std::set<int64_t> observedSectorsX; // sector.x values seen for the tracked base
    bool sentCommand{ false };
};

// Run the full M1a scenario. Returns true iff every bot connected AND every bot
// observed the tracked base (netId 1) in at least two distinct X sectors
// (i.e. it watched the base cross a sector boundary).
inline bool RunScenario(const Neuron::Net::Impairments& imp, int botCount, int iterations)
{
    using namespace Neuron::Net;

    FakeCrypto crypto;
    std::vector<uint8_t> staticPriv;
    EcPubKey pinnedPub;
    FakeCrypto::MakeFakeStaticKey(staticPriv, pinnedPub);
    const std::vector<uint8_t> serverSecret = { 'm','1','a' };

    Neuron::Sim::ServerWorld world;
    ServerHost host(&crypto, staticPriv, serverSecret, &world);

    LoopbackNetwork net;
    net.Configure(imp);

    const uint16_t serverPort = 7777;
    LoopbackSocket serverSock(&net);
    serverSock.Open(serverPort);
    Endpoint serverEp{ "127.0.0.1", serverPort };

    std::vector<BotClient> bots;
    for (int i = 0; i < botCount; ++i) {
        BotClient b;
        b.sock = std::make_unique<LoopbackSocket>(&net);
        b.sock->Open(0); // ephemeral
        b.conn = std::make_unique<ClientConnection>(&crypto, pinnedPub);
        // Kick off the handshake (ClientHello → server).
        auto hello = b.conn->Start(/*clientTime*/ 1000 + i);
        b.sock->SendTo(serverEp, hello);
        bots.push_back(std::move(b));
    }

    std::array<uint8_t, 2048> buf{};
    const float dt = 1.0f / 30.0f; // 30 Hz fixed step (App. B)

    for (int iter = 0; iter < iterations; ++iter) {
        // --- Server: drain inbound, dispatch, send replies ---
        {
            Endpoint from;
            std::vector<OutDatagram> out;
            int n;
            while ((n = serverSock.RecvFrom(from, buf)) > 0) {
                host.OnDatagram(from, std::span<const uint8_t>(buf.data(), n), out);
            }
            // Advance the sim and broadcast a snapshot each iteration.
            world.Step(dt);
            host.BroadcastSnapshots(out);
            for (auto& od : out)
                serverSock.SendTo(od.to, od.data);
        }

        // --- Clients: drain inbound, dispatch, send, observe snapshots ---
        for (auto& b : bots) {
            Endpoint from;
            int n;
            while ((n = b.sock->RecvFrom(from, buf)) > 0) {
                std::vector<AppMessage> appOut;
                std::vector<std::vector<uint8_t>> sendOut;
                b.conn->OnDatagram(std::span<const uint8_t>(buf.data(), n), appOut, sendOut);
                for (auto& d : sendOut) b.sock->SendTo(serverEp, d);

                for (const auto& m : appOut) {
                    if (m.type == MsgType::Snapshot) {
                        Neuron::Sim::Snapshot snap;
                        if (Neuron::Sim::DecodeSnapshot(m.body, snap)) {
                            for (const auto& e : snap.entities) {
                                if (e.netId == 1) {
                                    const auto sec = Neuron::World::WorldToSector(e.pos);
                                    b.observedSectorsX.insert(sec.x);
                                }
                            }
                        }
                    }
                }
            }

            // Retransmit handshake/login while still connecting (recovers from loss).
            if (!b.conn->IsConnected()) {
                std::vector<std::vector<uint8_t>> rs;
                b.conn->ResendPending(rs);
                for (auto& d : rs) b.sock->SendTo(serverEp, d);
            } else if (!b.sentCommand) {
                // Exercise the command path once: nudge the base east (server clamps).
                Neuron::Sim::MoveCommand cmd;
                cmd.velX = 40.0f;
                if (auto dg = b.conn->SendCommand(Neuron::Sim::EncodeMoveCommand(cmd)))
                    b.sock->SendTo(serverEp, *dg);
                b.sentCommand = true;
            }
        }

        // Flush any packets held for reordering.
        net.Step();
    }

    // Verdict: all bots connected and all observed the base crossing a boundary.
    int connected = 0, crossed = 0;
    for (auto& b : bots) {
        if (b.conn->IsConnected()) ++connected;
        if (b.observedSectorsX.size() >= 2) ++crossed;
    }
    return connected == botCount && crossed == botCount;
}

} // anonymous

TEST_SUITE(Integration)
{
    TEST_CASE(ThreeBotsCleanNetwork) {
        Neuron::Net::Impairments clean; // no impairments
        CHECK(RunScenario(clean, /*bots*/ 3, /*iterations*/ 600));
    });

    TEST_CASE(ThreeBotsWithDuplication) {
        Neuron::Net::Impairments imp;
        imp.dupProbability = 0.3;
        imp.seed = 2;
        CHECK(RunScenario(imp, 3, 600));
    });

    TEST_CASE(ThreeBotsWithReorder) {
        Neuron::Net::Impairments imp;
        imp.reorderDepth = 4;
        imp.seed = 7;
        CHECK(RunScenario(imp, 3, 800));
    });

    TEST_CASE(ThreeBotsWithLossReorderDup) {
        // The full M1a acceptance condition: loss + reorder + duplication.
        Neuron::Net::Impairments imp;
        imp.lossProbability = 0.10;
        imp.dupProbability  = 0.10;
        imp.reorderDepth    = 3;
        imp.seed            = 1234;
        CHECK(RunScenario(imp, 3, 2000));
    });
}
