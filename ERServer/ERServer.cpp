// ERServer.cpp — EarthRise dedicated server entry point (M1a).
//
// Architecture (§9):
//   net I/O -> decode/reliability/decrypt -> sim -> snapshots -> net
//   Single-threaded 30 Hz fixed-step sim owns the authoritative ECS state.
//   (IOCP multi-threading via ERServer/netio/IocpUdpListener is the M4 scaling
//    lever; M1a uses a correct single-threaded non-blocking Winsock loop — the
//    same logic verified end-to-end over the loopback integration tests.)
//   Persistence (ODBC outbox + write-behind) lands in M5.
//
// Connection sequence (§8.5): stateless cookie -> version gate -> CNG ECDH with
// server-key signature -> clock sync -> login -> snapshot loop. Driven by the
// platform-independent Neuron::Net::ServerHost over a real WinsockSocket.

#include "pch.h"
#include <array>
#include <fstream>
#include <span>
#include <vector>

// NeuronCore
#include "FixedStepAccumulator.h"
#include "ServerWorld.h"
#include "CngCrypto.h"
#include "WinsockSocket.h"
#include "ServerHost.h"

namespace
{
  volatile bool g_running = true;

  BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl)
  {
    if (ctrl == CTRL_C_EVENT || ctrl == CTRL_BREAK_EVENT || ctrl == CTRL_CLOSE_EVENT)
    {
      g_running = false;
      return TRUE;
    }
    return FALSE;
  }

  uint16_t ListenPortFromEnv()
  {
    char buf[16]{};
    DWORD n = GetEnvironmentVariableA("ER_LISTEN_PORT", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf))
    {
      const int p = std::atoi(buf);
      if (p > 0 && p < 65536)
        return static_cast<uint16_t>(p);
    }
    return 7777;
  }
} // namespace

int main()
{
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
  const uint16_t port = ListenPortFromEnv();
  EARTHRISE_LOG_INFO("ERServer starting (M1a) — 30 Hz, UDP port {}.\n", port);

  if (!Neuron::Net::WinsockSocket::GlobalStartup())
  {
    EARTHRISE_LOG_ERROR("WSAStartup failed.\n");
    return 1;
  }

  // Crypto: open CNG and load/generate the pinned static server key (§8.3/§14).
  Neuron::Net::CngCrypto crypto;
  if (!crypto.Initialize())
  {
    EARTHRISE_LOG_ERROR("CNG init failed.\n");
    return 1;
  }
  crypto.LoadOrGenerateStaticKey({}); // M1a: ephemeral static key; M5 persists it.

  // Publish the pinned static public key so dev clients / ERHeadless bots can
  // pin it (§8.3). Real clients ship with this key baked in.
  {
    const auto pub = crypto.GetStaticPublicKey();
    std::ofstream f("er_server_pub.bin", std::ios::binary);
    if (f) { f.write(reinterpret_cast<const char*>(pub.data()), pub.size()); }
    EARTHRISE_LOG_INFO("Wrote pinned static public key to er_server_pub.bin.\n");
  }

  // Server secret for stateless cookies; random per process for M1a.
  std::vector<uint8_t> serverSecret(32);
  crypto.RandomBytes(serverSecret);

  Neuron::Sim::ServerWorld world;
  Neuron::Net::ServerHost host(&crypto, std::vector<uint8_t>(crypto.GetStaticPrivateBlob().begin(), crypto.GetStaticPrivateBlob().end()),
                               serverSecret, &world);

  Neuron::Net::WinsockSocket sock;
  if (!sock.Open(port))
  {
    EARTHRISE_LOG_ERROR("Failed to bind UDP port {}.\n", port);
    return 1;
  }

  Neuron::Sim::FixedStepAccumulator acc;
  acc.Start();

  std::array<uint8_t, 2048> buf{};
  uint64_t lastSnapshotTick = 0;
  uint64_t lastLogTick = 0;

  while (g_running)
  {
    // 1) Drain inbound datagrams and route them through the host.
    std::vector<Neuron::Net::OutDatagram> out;
    Neuron::Net::Endpoint from;
    int n;
    while ((n = sock.RecvFrom(from, buf)) > 0) { host.OnDatagram(from, std::span<const uint8_t>(buf.data(), static_cast<size_t>(n)), out); }

    // 2) Advance the fixed-step simulation.
    acc.Tick();
    bool stepped = false;
    while (acc.ConsumeStep())
    {
      world.Step(Neuron::Sim::kSimDeltaSeconds);
      stepped = true;
    }

    // 3) Emit snapshots at the 20 Hz cadence (every ~1.5 sim ticks).
    const uint64_t simTick = acc.GetSimTickCount();
    if (stepped && simTick - lastSnapshotTick >= 1)
    {
      host.BroadcastSnapshots(out);
      lastSnapshotTick = simTick;
    }

    // 4) Flush all queued outbound datagrams.
    for (auto& od : out)
      sock.SendTo(od.to, od.data);

    if (simTick - lastLogTick >= 300)
    {
      EARTHRISE_LOG_INFO("ERServer tick {} — {} connections ({} connected).\n", simTick, host.ConnectionCount(), host.ConnectedCount());
      lastLogTick = simTick;
    }

    Sleep(1); // yield; M4 replaces this with IOCP-driven wakeups.
  }

  sock.Close();
  Neuron::Net::WinsockSocket::GlobalCleanup();
  EARTHRISE_LOG_INFO("ERServer shutting down.\n");
  return 0;
}
