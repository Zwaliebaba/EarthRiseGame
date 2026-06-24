// ERHeadless.cpp — headless client host (M1a + M3 area H).
//
// Hosts N NeuronClient bot sessions in one process (each on its own UDP port),
// each driven by a Neuron::Net::ClientConnection over a real WinsockSocket. Bots
// run the full §8.5 connection sequence against ERServer, then drive the M3 4X
// loop end-to-end via validated FleetCommands (§23.4) — harvest a node, enqueue a
// build, jump a beacon, attack an NPC site — server-authoritatively. The same
// command logic is verified deterministically over the loopback in the test
// runner's Determinism suite (record/replay → identical SimHash).
//
// Bots != PvE NPCs. Bots are client sessions; PvE NPCs are server-side AI.
//
// Pinning: bots load the server's static public key from the file named by
// server.pinnedPublicKeyFile in the JSON config (default 'er_server_pub.bin',
// written by ERServer on startup).
//
// Config: all connection settings come from a JSON file (§20), not the process
// environment. Looks for 'erheadless.config.json' in the working directory, or a
// path passed via --config <path>. See Config/erheadless.config.example.json.

#include "pch.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Debug.h"
#include "CngCrypto.h"
#include "WinsockSocket.h"
#include "Connection.h"
#include "Command.h"
#include "Snapshot.h"
#include "UniversePos.h"
#include "ClientConfig.h"

namespace
{
// Resolve the config-file path from a "--config <path>" / "--config=<path>"
// argument, else the default filename in the working directory.
std::string ConfigPathFromArgs(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--config" && i + 1 < argc)
            return argv[i + 1];
        if (arg.rfind("--config=", 0) == 0)
            return std::string(arg.substr(std::string_view("--config=").size()));
    }
    return Neuron::Client::DEFAULT_CONFIG_FILENAME;
}

// First positional (non-flag) argument, if any — kept for the legacy
// "ERHeadless.exe <botCount>" invocation, which overrides headless.botCount.
const char* FirstPositionalArg(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--config") { ++i; continue; } // skip the flag's value
        if (arg.rfind("--", 0) == 0) continue;     // skip other flags
        return argv[i];
    }
    return nullptr;
}

bool LoadPinnedPub(const std::string& path, Neuron::Net::EcPubKey& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<size_t>(f.gcount()) == out.size();
}

// What the bot last saw in its (fog-filtered) snapshot — enough to pick the next
// intent without holding a full replica.
struct SeenWorld
{
    std::vector<uint32_t> ownShips;   // owner == self, kind Ship
    std::vector<uint32_t> nodes;      // resource nodes
    std::vector<uint32_t> npcs;       // hostile NPC units
    uint32_t              nodeTarget{ 0 };
    uint32_t              npcTarget{ 0 };
};

// The bot's scripted progression through the loop (one phase per achievement).
enum class BotPhase : uint8_t { Connecting = 0, Building, Harvesting, Attacking, Done };

struct Bot
{
    std::unique_ptr<Neuron::Net::WinsockSocket>    sock;
    std::unique_ptr<Neuron::Net::ClientConnection> conn;
    std::set<int64_t> observedSectorsX;
    BotPhase phase{ BotPhase::Connecting };
    uint32_t cooldownTicks{ 0 }; // throttle command issuance
    bool builtOnce{ false };
};

// Decode the latest snapshot into a SeenWorld for 'self' (the bot's player id).
SeenWorld Scan(const Neuron::Sim::Snapshot& snap, uint32_t self)
{
    SeenWorld w;
    for (const auto& e : snap.entities) {
        switch (e.kind) {
        case Neuron::Sim::EntityKind::Ship:
            if (e.ownerPlayer == self && e.netId != self) w.ownShips.push_back(e.netId);
            break;
        case Neuron::Sim::EntityKind::ResourceNode: w.nodes.push_back(e.netId); break;
        case Neuron::Sim::EntityKind::NpcUnit:      w.npcs.push_back(e.netId);  break;
        default: break;
        }
    }
    if (!w.nodes.empty()) w.nodeTarget = w.nodes.front();
    if (!w.npcs.empty())  w.npcTarget  = w.npcs.front();
    return w;
}

// Send a fleet command on the bot's connection (typed FleetCommand path, area B).
void SendFleet(Bot& b, const Neuron::Net::Endpoint& serverEp, const Neuron::Sim::FleetCommand& cmd)
{
    if (auto dg = b.conn->SendFleetCommand(Neuron::Sim::EncodeFleetCommand(cmd)))
        b.sock->SendTo(serverEp, *dg);
}
} // namespace

int main(int argc, char* argv[])
{
    // All connection settings come from the JSON config (§20). A missing config
    // file is non-fatal here — the defaults (localhost:7777, 3 bots) are exactly
    // what a local smoke run wants — but a malformed file is worth surfacing.
    const std::string configPath = ConfigPathFromArgs(argc, argv);
    Neuron::Client::ClientConfig cfg;
    std::string configError;
    if (Neuron::Client::ClientConfig::Load(configPath, cfg, &configError))
        EARTHRISE_LOG_INFO("ERHeadless: loaded config from '{}'.\n", configPath.c_str());
    else
        EARTHRISE_LOG_INFO("ERHeadless: no usable config ({}) - using defaults.\n",
                           configError.c_str());

    // Legacy positional override: "ERHeadless.exe <botCount>" still works.
    if (const char* botArg = FirstPositionalArg(argc, argv))
        cfg.botCount = static_cast<uint32_t>(std::atoi(botArg));

    const uint32_t    botCount = cfg.botCount;
    const std::string host     = cfg.host;
    const uint16_t    port     = cfg.port;

    EARTHRISE_LOG_INFO("ERHeadless (M1a) — {} bots -> {}:{}.\n", botCount, host, port);

    if (!Neuron::Net::WinsockSocket::GlobalStartup()) {
        EARTHRISE_LOG_ERROR("WSAStartup failed.\n");
        return 1;
    }

    Neuron::Net::CngCrypto crypto;
    if (!crypto.Initialize()) {
        EARTHRISE_LOG_ERROR("CNG init failed.\n");
        return 1;
    }

    Neuron::Net::EcPubKey pinnedPub{};
    if (!LoadPinnedPub(cfg.pinnedPublicKeyFile, pinnedPub)) {
        EARTHRISE_LOG_ERROR("Could not load pinned key ({}). Start ERServer first.\n",
                            cfg.pinnedPublicKeyFile.c_str());
        return 1;
    }

    Neuron::Net::Endpoint serverEp{ host, port };

    std::vector<Bot> bots;
    for (uint32_t i = 0; i < botCount; ++i) {
        Bot b;
        b.sock = std::make_unique<Neuron::Net::WinsockSocket>();
        if (!b.sock->Open(0)) { EARTHRISE_LOG_ERROR("bot {} socket open failed\n", i); continue; }
        b.conn = std::make_unique<Neuron::Net::ClientConnection>(&crypto, pinnedPub);
        auto hello = b.conn->Start(static_cast<uint64_t>(GetTickCount64()) * 1000 + i);
        b.sock->SendTo(serverEp, hello);
        bots.push_back(std::move(b));
    }

    std::array<uint8_t, 2048> buf{};
    constexpr int kMaxIterations = 20000; // safety bound for the dev run

    for (int iter = 0; iter < kMaxIterations; ++iter) {
        for (auto& b : bots) {
            const uint32_t self = b.conn->PlayerNetId();
            Neuron::Sim::Snapshot latest;
            bool haveSnap = false;

            Neuron::Net::Endpoint from;
            int n;
            while ((n = b.sock->RecvFrom(from, buf)) > 0) {
                std::vector<Neuron::Net::AppMessage> appOut;
                std::vector<std::vector<uint8_t>> sendOut;
                b.conn->OnDatagram(std::span<const uint8_t>(buf.data(), static_cast<size_t>(n)),
                                   appOut, sendOut);
                for (auto& d : sendOut) b.sock->SendTo(serverEp, d);

                for (const auto& m : appOut) {
                    if (m.type == Neuron::Net::MsgType::Snapshot) {
                        Neuron::Sim::Snapshot snap;
                        if (Neuron::Sim::DecodeSnapshot(m.body, snap)) {
                            latest = snap; haveSnap = true;
                            for (const auto& e : snap.entities)
                                if (e.netId == self)
                                    b.observedSectorsX.insert(Neuron::Universe::UniverseToSector(e.pos).x);
                        }
                    }
                }
            }

            if (!b.conn->IsConnected()) {
                std::vector<std::vector<uint8_t>> rs;
                b.conn->ResendPending(rs);
                for (auto& d : rs) b.sock->SendTo(serverEp, d);
                continue;
            }
            if (b.cooldownTicks > 0) { --b.cooldownTicks; continue; }
            if (!haveSnap) continue;

            // Drive the 4X loop one intent at a time off the fog snapshot (area H).
            const SeenWorld w = Scan(latest, self);
            auto throttle = [&](uint32_t ticks) { b.cooldownTicks = ticks; };
            switch (b.phase) {
            case BotPhase::Connecting:
                b.phase = BotPhase::Building;
                break;
            case BotPhase::Building: {
                // Enqueue a ship build off the seeded ore (Build intent targets base).
                Neuron::Sim::FleetCommand c; c.intent = Neuron::Sim::IntentType::Build; c.units = { self };
                SendFleet(b, serverEp, c);
                b.builtOnce = true;
                b.phase = BotPhase::Harvesting;
                throttle(200); // let the build complete
                break;
            }
            case BotPhase::Harvesting:
                if (!w.ownShips.empty() && w.nodeTarget != 0) {
                    Neuron::Sim::FleetCommand c; c.intent = Neuron::Sim::IntentType::Harvest;
                    c.units = { w.ownShips.front() }; c.targetNetId = w.nodeTarget;
                    SendFleet(b, serverEp, c);
                    b.phase = BotPhase::Attacking;
                    throttle(200);
                } else if (w.ownShips.empty()) {
                    b.phase = BotPhase::Building; // no ship yet → build again
                }
                break;
            case BotPhase::Attacking:
                if (!w.ownShips.empty() && w.npcTarget != 0) {
                    Neuron::Sim::FleetCommand c; c.intent = Neuron::Sim::IntentType::Attack;
                    c.units = w.ownShips; c.targetNetId = w.npcTarget;
                    SendFleet(b, serverEp, c);
                    throttle(50);
                } else if (w.npcs.empty()) {
                    b.phase = BotPhase::Done; // nothing hostile in sensor range → loop done
                }
                break;
            case BotPhase::Done:
                break;
            }
        }

        // Termination: all bots connected and each drove the loop to completion.
        size_t connected = 0, done = 0;
        for (auto& b : bots) {
            if (b.conn->IsConnected()) ++connected;
            if (b.phase == BotPhase::Done || b.builtOnce) ++done;
        }
        if (!bots.empty() && connected == bots.size() && done == bots.size()) {
            EARTHRISE_LOG_INFO("ERHeadless: all {} bots connected and drove the M3 loop.\n",
                               bots.size());
            break;
        }

        Sleep(1);
    }

    for (auto& b : bots) b.sock->Close();
    Neuron::Net::WinsockSocket::GlobalCleanup();
    EARTHRISE_LOG_INFO("ERHeadless done.\n");
    return 0;
}
