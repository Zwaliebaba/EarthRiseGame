// ERServer.cpp — EarthRise dedicated server entry point (M1a).
//
// Architecture (§9):
//   net I/O -> decode/reliability/decrypt -> sim -> snapshots -> net
//   Single-threaded 30 Hz fixed-step sim owns the authoritative ECS state.
//   (IOCP multi-threading via ERServer/IocpUdpListener is the M4 scaling
//    lever; M1a uses a correct single-threaded non-blocking Winsock loop — the
//    same logic verified end-to-end over the loopback integration tests.)
//   Persistence (ODBC outbox + write-behind) lands in M5.
//
// Connection sequence (§8.5): stateless cookie -> version gate -> CNG ECDH with
// server-key signature -> clock sync -> login -> snapshot loop. Driven by the
// platform-independent Neuron::Net::ServerHost over a real WinsockSocket.
//
// Logging: ERServer is a console daemon, so operational logs go to stdout via
// ConsoleLog() (below) in *all* build configs. (EARTHRISE_LOG_* routes through
// Neuron::DebugTrace -> OutputDebugString, which is debugger-only and compiled
// out in Release — useless for watching a running server, so it's not used for
// the operational path here.)

#include "pch.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
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

  // Operational console logging (stdout, timestamped, flushed) — visible in
  // every build config, unlike the debugger-only EARTHRISE_LOG_* shim.
  template <class... Types>
  void ConsoleLog(std::string_view fmt, Types&&... args)
  {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const std::string body = std::vformat(fmt, std::make_format_args(args...));
    std::printf("[%02u:%02u:%02u.%03u] %s", static_cast<unsigned>(t.wHour),
                static_cast<unsigned>(t.wMinute), static_cast<unsigned>(t.wSecond),
                static_cast<unsigned>(t.wMilliseconds), body.c_str());
    std::fflush(stdout);
  }

  std::string EndpointStr(const Neuron::Net::Endpoint& e)
  {
    return e.ip + ":" + std::to_string(e.port);
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

  // Exercise the real CNG crypto along the exact sequence the server handshake
  // uses (ECDH keygen -> shared secret -> ECDSA sign/verify -> HKDF -> AES-GCM).
  // The connection layer's own tests use FakeCrypto (Handshake.h), so this is the
  // first place the production primitives run end-to-end; a FAIL here is exactly
  // why a client's CookieResponse never yields a HandshakeResponse.
  bool RunCryptoSelfTest(Neuron::Net::CngCrypto& crypto, const std::vector<uint8_t>& staticPriv,
                         const Neuron::Net::EcPubKey& staticPub)
  {
    using namespace Neuron::Net;
    ConsoleLog("[INFO] Crypto self-test (real CNG handshake path)...\n");

    bool allOk = true;
    auto step = [&](const char* name, bool ok) {
      ConsoleLog("[{}] self-test {}: {}\n", ok ? "INFO" : "ERROR", name, ok ? "PASS" : "FAIL");
      allOk = allOk && ok;
      return ok;
    };

    EcdhKeyPair clientKp = crypto.GenerateEcdhKeyPair();
    EcdhKeyPair serverKp = crypto.GenerateEcdhKeyPair();
    step("ECDH keygen", !clientKp.privateBlob.empty() && !serverKp.privateBlob.empty());

    SharedSecret sA{}, sB{};
    const bool dhOk = crypto.DeriveSharedSecret(serverKp, clientKp.publicKey, sA) &&
                      crypto.DeriveSharedSecret(clientKp, serverKp.publicKey, sB);
    step("ECDH shared-secret agreement", dhOk && sA == sB);

    const std::span<const uint8_t> serverPub(serverKp.publicKey.data(), serverKp.publicKey.size());
    EcSignature sig{};
    step("ECDSA sign (server static key)", crypto.Sign(staticPriv, serverPub, sig));
    step("ECDSA verify (pinned static key)", crypto.Verify(staticPub, serverPub, sig));

    AeadKey c2s{}, s2c{};
    step("HKDF AEAD key derivation", crypto.DeriveAeadKeys(sA, 0, c2s, s2c));

    const std::array<uint8_t, 4> aad{0xDE, 0xAD, 0xBE, 0xEF};
    const std::vector<uint8_t> msg{'h', 'e', 'l', 'l', 'o'};
    std::vector<uint8_t> ct, pt;
    const std::span<const uint8_t> aadSpan(aad.data(), aad.size());
    const bool encOk = crypto.Encrypt(c2s, Direction::ClientToServer, 1, aadSpan, msg, ct);
    const bool decOk = encOk && crypto.Decrypt(c2s, Direction::ClientToServer, 1, aadSpan, ct, pt);
    step("AES-256-GCM round-trip", encOk && decOk && pt == msg);

    if (allOk)
      ConsoleLog("[INFO] Crypto self-test PASSED - handshake crypto path is healthy.\n");
    else
      ConsoleLog("[ERROR] Crypto self-test FAILED - the FAIL step(s) above are why clients "
                 "cannot complete the handshake.\n");
    return allOk;
  }
} // namespace

int main()
{
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
  const uint16_t port = ListenPortFromEnv();
  ConsoleLog("[INFO] ERServer starting (M1a) — 30 Hz sim, 20 Hz snapshots.\n");

  if (!Neuron::Net::WinsockSocket::GlobalStartup())
  {
    ConsoleLog("[ERROR] WSAStartup failed.\n");
    return 1;
  }

  // Crypto: open CNG and load/generate the pinned static server key (§8.3/§14).
  Neuron::Net::CngCrypto crypto;
  if (!crypto.Initialize())
  {
    ConsoleLog("[ERROR] CNG init failed.\n");
    return 1;
  }
  crypto.LoadOrGenerateStaticKey({}); // M1a: ephemeral static key; M5 persists it.

  // Publish the pinned static public key so dev clients / ERHeadless bots can
  // pin it (§8.3). Real clients ship with this key baked in.
  {
    const auto pub = crypto.GetStaticPublicKey();
    std::ofstream f("er_server_pub.bin", std::ios::binary);
    if (f)
    {
      f.write(reinterpret_cast<const char*>(pub.data()), pub.size());
      ConsoleLog("[INFO] Wrote pinned static public key to er_server_pub.bin ({} bytes).\n",
                 pub.size());
    }
    else
    {
      ConsoleLog("[WARN] Could not write er_server_pub.bin (cwd not writable?).\n");
    }
  }

  // Server secret for stateless cookies; random per process for M1a.
  std::vector<uint8_t> serverSecret(32);
  crypto.RandomBytes(serverSecret);

  // Validate the production crypto path up front (see RunCryptoSelfTest).
  const std::vector<uint8_t> staticPriv(crypto.GetStaticPrivateBlob().begin(),
                                        crypto.GetStaticPrivateBlob().end());
  const Neuron::Net::EcPubKey staticPub = crypto.GetStaticPublicKey();
  RunCryptoSelfTest(crypto, staticPriv, staticPub);

  Neuron::Sim::ServerWorld world;
  Neuron::Net::ServerHost host(&crypto, staticPriv, serverSecret, &world);

  Neuron::Net::WinsockSocket sock;
  if (!sock.Open(port))
  {
    ConsoleLog("[ERROR] Failed to bind UDP port {} — is it already in use?\n", port);
    return 1;
  }
  ConsoleLog("[INFO] Listening on UDP 0.0.0.0:{}. Waiting for clients (Ctrl+C to stop)...\n", port);

  Neuron::Sim::FixedStepAccumulator acc;
  acc.Start();

  std::array<uint8_t, 2048> buf{};
  uint64_t lastSnapshotTick = 0;
  uint64_t lastLogTick = 0;

  // Diagnostics: cumulative traffic + connection-state tracking.
  uint64_t rxDatagrams = 0, rxBytes = 0, txDatagrams = 0, txBytes = 0;
  std::unordered_set<std::string> seenEndpoints;
  int prevConnections = -1, prevConnected = -1;

  while (g_running)
  {
    // 1) Drain inbound datagrams and route them through the host.
    std::vector<Neuron::Net::OutDatagram> out;
    Neuron::Net::Endpoint from;
    int n;
    while ((n = sock.RecvFrom(from, buf)) > 0)
    {
      ++rxDatagrams;
      rxBytes += static_cast<uint64_t>(n);
      // First contact from a new endpoint is the key signal the client reached us.
      if (seenEndpoints.insert(EndpointStr(from)).second)
        ConsoleLog("[INFO] First datagram from {} ({} bytes) — beginning handshake.\n",
                   EndpointStr(from), n);
      host.OnDatagram(from, std::span<const uint8_t>(buf.data(), static_cast<size_t>(n)), out);
    }

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
    {
      sock.SendTo(od.to, od.data);
      ++txDatagrams;
      txBytes += od.data.size();
    }

    // 5) Log connection-state transitions immediately (the interesting events).
    const int connections = static_cast<int>(host.ConnectionCount());
    const int connected = static_cast<int>(host.ConnectedCount());
    if (connections != prevConnections || connected != prevConnected)
    {
      ConsoleLog("[INFO] Connections: {} pending/active, {} fully connected.\n", connections,
                 connected);
      prevConnections = connections;
      prevConnected = connected;
    }

    // 6) Periodic heartbeat (~every 5 s at 30 Hz) so it's obvious the server is alive.
    if (simTick - lastLogTick >= 150)
    {
      ConsoleLog("[INFO] alive — sim tick {}, conns {}/{}, rx {} dgrams ({} B), tx {} dgrams ({} B).\n",
                 simTick, connected, connections, rxDatagrams, rxBytes, txDatagrams, txBytes);
      if (rxDatagrams == 0)
        ConsoleLog("[WARN] No datagrams received yet. If a client is running, check: UWP loopback "
                   "exemption (CheckNetIsolation.exe LoopbackExempt -a -n=<PackageFamilyName>), "
                   "Windows Firewall, and that the client targets UDP {}.\n",
                   port);
      lastLogTick = simTick;
    }

    Sleep(1); // yield; M4 replaces this with IOCP-driven wakeups.
  }

  sock.Close();
  Neuron::Net::WinsockSocket::GlobalCleanup();
  ConsoleLog("[INFO] ERServer shutting down (rx {} dgrams / tx {} dgrams).\n", rxDatagrams,
             txDatagrams);
  return 0;
}
