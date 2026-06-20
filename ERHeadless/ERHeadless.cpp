// ERHeadless.cpp — headless client host (M1a).
//
// Hosts N NeuronClient bot sessions in one process (each on its own UDP port),
// each driven by a Neuron::Net::ClientConnection over a real WinsockSocket. Bots
// run the full §8.5 connection sequence against ERServer, then watch their base
// drift across a sector boundary — the M1a acceptance scenario, here against a
// live server (the same logic is verified deterministically over the loopback in
// the test runner's Integration suite).
//
// Bots != PvE NPCs. Bots are client sessions; PvE NPCs are server-side AI.
//
// Pinning: bots load the server's static public key from 'er_server_pub.bin'
// (written by ERServer on startup) — overridable via ER_SERVER_PUB.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "Debug.h"
#include "CngCrypto.h"
#include "WinsockSocket.h"
#include "Connection.h"
#include "Command.h"
#include "Snapshot.h"
#include "WorldPos.h"

namespace
{
constexpr uint32_t kDefaultBotCount = 3;

std::string EnvOr(const char* name, const char* fallback)
{
    char buf[256]{};
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) return std::string(buf, n);
    return fallback;
}

bool LoadPinnedPub(Neuron::Net::EcPubKey& out)
{
    const std::string path = EnvOr("ER_SERVER_PUB", "er_server_pub.bin");
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<size_t>(f.gcount()) == out.size();
}

struct Bot
{
    std::unique_ptr<Neuron::Net::WinsockSocket>    sock;
    std::unique_ptr<Neuron::Net::ClientConnection> conn;
    std::set<int64_t> observedSectorsX;
    bool sentCommand{ false };
};
} // namespace

int main(int argc, char* argv[])
{
    const uint32_t botCount = (argc >= 2) ? static_cast<uint32_t>(std::atoi(argv[1]))
                                          : kDefaultBotCount;
    const std::string host = EnvOr("ER_SERVER_HOST", "127.0.0.1");
    const uint16_t    port = static_cast<uint16_t>(std::atoi(EnvOr("ER_SERVER_PORT", "7777").c_str()));

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
    if (!LoadPinnedPub(pinnedPub)) {
        EARTHRISE_LOG_ERROR("Could not load pinned key (er_server_pub.bin). Start ERServer first.\n");
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
                            for (const auto& e : snap.entities) {
                                if (e.netId == b.conn->PlayerNetId()) {
                                    const auto sec = Neuron::World::WorldToSector(e.pos);
                                    b.observedSectorsX.insert(sec.x);
                                }
                            }
                        }
                    }
                }
            }

            if (!b.conn->IsConnected()) {
                std::vector<std::vector<uint8_t>> rs;
                b.conn->ResendPending(rs);
                for (auto& d : rs) b.sock->SendTo(serverEp, d);
            } else if (!b.sentCommand) {
                Neuron::Sim::MoveCommand cmd;
                cmd.velX = 40.0f;
                if (auto dg = b.conn->SendCommand(Neuron::Sim::EncodeMoveCommand(cmd)))
                    b.sock->SendTo(serverEp, *dg);
                b.sentCommand = true;
            }
        }

        // Termination: all bots connected and each saw its base cross a boundary.
        size_t connected = 0, crossed = 0;
        for (auto& b : bots) {
            if (b.conn->IsConnected()) ++connected;
            if (b.observedSectorsX.size() >= 2) ++crossed;
        }
        if (!bots.empty() && connected == bots.size() && crossed == bots.size()) {
            EARTHRISE_LOG_INFO("ERHeadless: all {} bots connected and observed a sector crossing.\n",
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
