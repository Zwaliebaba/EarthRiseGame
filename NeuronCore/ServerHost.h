// ServerHost — accepts clients, runs the stateless cookie phase, owns the
// per-connection map, and bridges the network to the authoritative ServerUniverse
// (§8.5, §9). Platform-independent: the host loop (ERServer / the integration
// test) supplies datagrams and ferries the produced datagrams to the socket.
//
// M4 (areas F/G/H/I): routing is token-indexed via ConnectionTable (generation-
// tagged slot table + per-connection affinity lane), not an ip:port string hash;
// a small ip:port→token association survives only for the pre-token cookie phase.
// BroadcastSnapshots drives the M4 delta pipeline (per-client baseline +
// BuildClientSnapshot via the job-pool seam) instead of the M3 full snapshot. The
// server's time-dilation factor is pushed to connections so the clock echo carries
// it (§8.5). Telemetry sampling sites (downstream bytes, baseline RAM, cap-bind)
// are wired here; ERServer instantiates the ServerTelemetry and reads it.
//
// M4: Windows integration — unverified on Linux; the routing/broadcast rewrite is
// not exercised by the testrunner (no integration suite compiles ServerHost.h).
// Validate on the Windows build agent + the loopback integration tests.
#pragma once

#include "Connection.h"
#include "ConnectionTable.h"
#include "Handshake.h"
#include "ICrypto.h"
#include "ISocket.h"
#include "Protocol.h"
#include "Command.h"
#include "ServerUniverse.h"
#include "SnapshotJobs.h"
#include "Telemetry.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Neuron::Net
{

// A datagram addressed to a specific peer (host loop sends these out).
struct OutDatagram
{
    Endpoint              to;
    std::vector<uint8_t>  data;
};

class ServerHost
{
public:
    ServerHost(ICrypto* crypto,
               std::vector<uint8_t> staticPriv,
               std::vector<uint8_t> serverSecret,
               Neuron::Sim::ServerUniverse* universe,
               uint64_t serverTimeMicros = 0)
        : m_crypto(crypto)
        , m_staticPriv(std::move(staticPriv))
        , m_serverSecret(std::move(serverSecret))
        , m_universe(universe)
        , m_serverTime(serverTimeMicros)
        , m_stateless{ crypto, m_serverSecret }
    {
    }

    // Process one inbound datagram from 'from'. Queues replies into 'out'.
    void OnDatagram(const Endpoint& from, std::span<const uint8_t> dg,
                    std::vector<OutDatagram>& out)
    {
        if (dg.empty()) return;
        const auto kind = static_cast<DatagramKind>(dg[0]);

        // M4 area G: route an *encrypted* datagram by the 64-bit connection token
        // peeked from its clear header — a single u64 lookup into the generation-
        // tagged ConnectionTable, no per-datagram string hash. The token is AEAD
        // AAD, so SecureChannel still authenticates it after routing.
        if (kind == DatagramKind::Encrypted) {
            if (const auto token = PeekConnectionToken(dg)) {
                if (ConnEntry* e = EntryForToken(*token)) {
                    e->lastSeenMs = m_nowMs;
                    DispatchToConnection(*e, from, dg, out);
                    if (e->conn->State() == ConnState::Disconnected)
                        RemoveConnection(*token);
                    return;
                }
            }
            // Unknown token (e.g. our HandshakeResponse was lost and the peer is
            // still on the clear path, or a stale/forged datagram): drop. The
            // client re-drives the handshake on the clear channel.
            return;
        }

        // Clear handshake (cookie phase) — no token yet, so it routes by ip:port.
        if (kind != DatagramKind::ClearHandshake || dg.size() < 2) return;

        // A handshaking peer we already allocated (ECDH/clock-sync still in clear):
        // route by its first-packet ip:port→token association.
        const std::string key = Key(from);
        if (auto it = m_addrToToken.find(key); it != m_addrToToken.end()) {
            if (ConnEntry* e = EntryForToken(it->second)) {
                e->lastSeenMs = m_nowMs;
                DispatchToConnection(*e, from, dg, out);
                if (e->conn->State() == ConnState::Disconnected)
                    RemoveConnection(it->second);
                return;
            }
        }

        // Otherwise this is a brand-new peer in the stateless cookie phase.
        const auto type = static_cast<MsgType>(dg[1]);
        std::span<const uint8_t> body = dg.subspan(2);
        const std::vector<uint8_t> addr = from.AddrBytes();

        if (type == MsgType::ClientHello) {
            HsOutput o = m_stateless.OnClientHello(body, addr);
            if (o.send)
                out.push_back({ from, MakeHandshakeDatagram(o.type, o.body) });
            return;
        }

        if (type == MsgType::CookieResponse) {
            CookieResponseBody cr;
            if (!CookieResponseBody::Decode(body, cr)) return;
            if (!m_stateless.VerifyCookie(cr.cookie, addr)) return; // drop (DoS guard)

            // Cookie valid → allocate connection + spawn the player's base.
            const uint64_t token = NextToken();
            auto conn = std::make_unique<ServerConnection>(m_crypto, m_staticPriv, token, m_serverTime);
            conn->SetDilationFactor(m_dilationFactor); // M4 area H — current load floor

            // Each base starts just inside sector 0 (separated on Y) at the
            // centre of the catalog scenery cluster (ServerUniverse::SpawnScenery).
            // Stationary at spawn so the props stay in frame; movement comes from
            // client intents (SetBaseVelocity).
            const int64_t startX = Neuron::Universe::kSectorSize - 200;
            const int64_t startY = static_cast<int64_t>(m_conns.size()) * 2000;
            const uint32_t netId = m_universe->SpawnBase(
                { startX, startY, 0 }, { 0.0f, 0.0f, 0.0f });
            conn->SetPlayerNetId(netId);

            // Give the player a starter harvester + seed ore so the eXploit loop is
            // bootstrappable from a fresh connection (a minimal "fleet + base",
            // §13.1; full onboarding is M7). Harmless if SpawnFleetShip is capped.
            m_universe->SpawnFleetShip(netId, Neuron::Sim::ServerUniverse::ShipShapeId(),
                                       { startX + 220, startY, 0 });
            if (auto* st = m_universe->StorageOf(netId))
                st->amount[static_cast<int>(Neuron::Sim::ResourceType::Ore)] = 600.0f;

            std::vector<std::vector<uint8_t>> sendOut;
            conn->BeginFromCookieResponse(body, sendOut);
            for (auto& d : sendOut) out.push_back({ from, std::move(d) });

            // Open the routing slot (token → slot/generation + affinity lane, area G)
            // and store the connection. Pre-create the per-client baseline so the
            // area-F encode workers touch disjoint state with no allocation race.
            const ConnHandle handle = m_table.Open(token);
            ConnEntry entry;
            entry.conn      = std::move(conn);
            entry.endpoint  = from;
            entry.handle    = handle;
            entry.lastSeenMs = m_nowMs;
            entry.playerNetId = netId;
            m_conns.emplace(token, std::move(entry));
            m_addrToToken[key] = token;
            (void)m_universe->Baseline(netId); // pre-create baseline (area B/F)
            return;
        }
    }

    // Build and queue a snapshot for every connected client via the M4 delta
    // pipeline (areas A–F, §8.4): per-client baseline + BuildClientSnapshot
    // (priority/quota/quantized delta/tombstones) encoded over the read-only job
    // pool seam (EncodeClientsPooled) against the frozen post-tick universe, then
    // sealed per connection (seal/AEAD stays per-connection, area G). The encode is
    // a pure read of post-tick state; ERServer runs it on an OS thread pool by
    // passing workerCount > 1 (the gathered output is partition-count-independent).
    //
    // 'tel' (optional) records the App. B telemetry at the live sites: per-client
    // downstream bytes, baseline RAM, and the R16 cap-bind evidence.
    void BroadcastSnapshots(std::vector<OutDatagram>& out, size_t workerCount = 1,
                            Neuron::Sim::ServerTelemetry* tel = nullptr)
    {
        // Collect the connected clients in a deterministic order (sorted by netId)
        // so the partition/gather is stable across runs (matches the area-F gate).
        std::vector<uint32_t> clients = ConnectedClientIds();
        if (clients.empty()) return;

        // Encode every client's delta snapshot off the frozen post-tick state. The
        // pooled path partitions clients across workers and gathers back into
        // client order; ERServer injects real threads via workerCount.
        size_t totalCapped = 0;
        std::vector<Neuron::Sim::EncodeResult> encoded =
            EncodeClientsPooledCounting(*m_universe, clients, kSnapshotByteBudget,
                                        workerCount, totalCapped);
        if (tel) tel->RecordCapBind(totalCapped);

        // Seal + send each client's snapshot on its own connection (per-connection
        // AEAD/sequence state — area G affinity keeps this single-threaded per conn).
        for (const auto& er : encoded) {
            ConnEntry* e = EntryForClient(er.clientId);
            if (!e || !e->conn->IsConnected()) continue;

            const auto body = Neuron::Sim::EncodeDeltaSnapshot(er.snap);
            if (auto dg = e->conn->SealApp(Channel::Unreliable, MsgType::Snapshot, body)) {
                if (tel) {
                    tel->RecordClientDownstream(dg->size());
                    tel->Net().AddDown(dg->size());
                }
                out.push_back({ e->endpoint, std::move(*dg) });
            }
            // Stage what this snapshot carried so the client's ack advances every
            // per-client baseline (version baseline, delta base, staleness, tombstones).
            m_universe->RecordClientSnapshotSent(er.clientId, er.snap);
        }

        if (tel) tel->RecordBaselineBytes(m_universe->TotalClientBaselineBytes());
    }

    [[nodiscard]] size_t ConnectionCount() const noexcept { return m_conns.size(); }
    [[nodiscard]] size_t ConnectedCount() const noexcept
    {
        size_t n = 0;
        for (auto& [t, e] : m_conns) if (e.conn->IsConnected()) ++n;
        return n;
    }

    // The host supplies a monotonic clock (ms) so stale peers can be reaped.
    void SetClockMs(uint64_t nowMs) noexcept { m_nowMs = nowMs; }

    // M4 area H (§7.2/§8.5): the server loop pushes the FixedStepAccumulator's
    // current dilation factor each iteration. New connections inherit it at accept;
    // existing connections are updated so the next clock-sync echo (re-)publishes it.
    void SetDilationFactor(double factor) noexcept
    {
        m_dilationFactor = factor;
        for (auto& [t, e] : m_conns) e.conn->SetDilationFactor(factor);
    }
    [[nodiscard]] double DilationFactor() const noexcept { return m_dilationFactor; }

    struct ClosedConn
    {
        std::string endpoint;
        uint32_t    netId{ 0 };
        bool        timedOut{ false }; // true = idle timeout, false = graceful Disconnect
    };

    // Remove connections that gracefully disconnected or have sent no traffic
    // within 'timeoutMs' (0 disables the idle timeout). Despawns each base and
    // returns the removed peers for logging.
    std::vector<ClosedConn> PruneStale(uint64_t timeoutMs)
    {
        std::vector<ClosedConn> closed;
        std::vector<uint64_t> toRemove;
        for (auto& [token, e] : m_conns) {
            const bool disconnected = e.conn->State() == ConnState::Disconnected;
            bool timedOut = false;
            if (!disconnected && timeoutMs != 0 && m_nowMs >= e.lastSeenMs &&
                (m_nowMs - e.lastSeenMs) > timeoutMs)
                timedOut = true;
            if (disconnected || timedOut) {
                closed.push_back({ Key(e.endpoint), e.playerNetId, timedOut });
                toRemove.push_back(token);
            }
        }
        for (uint64_t token : toRemove) RemoveConnection(token);
        return closed;
    }

    // Connection-table accessor (diagnostics / tests): the area-G routing table.
    [[nodiscard]] const ConnectionTable& Table() const noexcept { return m_table; }

private:
    // One accepted connection: the driver, its endpoint, its routing handle/lane,
    // last-activity stamp and player net id. Keyed by the 64-bit connection token.
    struct ConnEntry
    {
        std::unique_ptr<ServerConnection> conn;
        Endpoint                          endpoint;
        ConnHandle                        handle;       // routing slot + generation (area G)
        uint64_t                          lastSeenMs{ 0 };
        uint32_t                          playerNetId{ 0 };
    };

    [[nodiscard]] ConnEntry* EntryForToken(uint64_t token)
    {
        auto it = m_conns.find(token);
        return it == m_conns.end() ? nullptr : &it->second;
    }
    [[nodiscard]] ConnEntry* EntryForClient(uint32_t netId)
    {
        for (auto& [t, e] : m_conns)
            if (e.playerNetId == netId) return &e;
        return nullptr;
    }

    [[nodiscard]] std::vector<uint32_t> ConnectedClientIds() const
    {
        std::vector<uint32_t> ids;
        ids.reserve(m_conns.size());
        for (auto& [t, e] : m_conns)
            if (e.conn->IsConnected()) ids.push_back(e.playerNetId);
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    // EncodeClientsPooled wrapper that also accumulates the R16 cap-bind total
    // across the gathered clients (area I evidence) for the telemetry site above.
    static std::vector<Neuron::Sim::EncodeResult>
    EncodeClientsPooledCounting(Neuron::Sim::ServerUniverse& su,
                                const std::vector<uint32_t>& clients,
                                size_t byteBudget, size_t workerCount, size_t& totalCapped)
    {
        // PartitionClients + per-slice EncodeClientsSerial (which sums capped),
        // gathered into client order. Mirrors EncodeClientsPooled but threads the
        // cap-bind count back out (the public seam discards it).
        const auto parts = Neuron::Sim::PartitionClients(clients, workerCount);
        std::vector<std::vector<Neuron::Sim::EncodeResult>> perWorker(parts.size());
        totalCapped = 0;
        for (size_t w = 0; w < parts.size(); ++w) {  // ← each iteration is one worker
            size_t capped = 0;
            perWorker[w] = Neuron::Sim::EncodeClientsSerial(su, parts[w], byteBudget, &capped);
            totalCapped += capped;
        }
        std::vector<Neuron::Sim::EncodeResult> outRes;
        outRes.reserve(clients.size());
        std::vector<size_t> cursor(parts.size(), 0);
        for (size_t i = 0; i < clients.size(); ++i) {
            const size_t w = i % parts.size();
            outRes.push_back(perWorker[w][cursor[w]++]);
        }
        return outRes;
    }

    // Despawn the base and drop all per-connection state for one peer (by token).
    void RemoveConnection(uint64_t token)
    {
        auto it = m_conns.find(token);
        if (it == m_conns.end()) return;
        if (m_universe) m_universe->DespawnBase(it->second.playerNetId);
        m_addrToToken.erase(Key(it->second.endpoint));
        m_table.Close(token);   // free the slot + bump generation (area G)
        m_conns.erase(it);
    }

    void DispatchToConnection(ConnEntry& entry, const Endpoint& from,
                              std::span<const uint8_t> dg, std::vector<OutDatagram>& out)
    {
        ServerConnection& conn = *entry.conn;

        // A repeated CookieResponse from a still-handshaking client means our
        // HandshakeResponse was lost — re-send it.
        if (!conn.IsConnected() && dg.size() >= 2 &&
            static_cast<DatagramKind>(dg[0]) == DatagramKind::ClearHandshake &&
            static_cast<MsgType>(dg[1]) == MsgType::CookieResponse) {
            std::vector<std::vector<uint8_t>> rs;
            conn.ResendHandshake(rs);
            for (auto& d : rs) out.push_back({ from, std::move(d) });
            return;
        }

        std::vector<AppMessage>            appOut;
        std::vector<std::vector<uint8_t>>  sendOut;
        conn.OnDatagram(dg, appOut, sendOut);
        for (auto& d : sendOut) out.push_back({ from, std::move(d) });

        // Apply received commands to the authoritative universe (server validates
        // everything — never client-authoritative, §8.4).
        for (const auto& m : appOut) {
            if (m.type == MsgType::Command) {
                // Legacy M1a base-velocity move intent.
                Neuron::Sim::MoveCommand cmd;
                if (Neuron::Sim::DecodeMoveCommand(m.body, cmd))
                    m_universe->SetBaseVelocity(conn.PlayerNetId(),
                                             { cmd.velX, cmd.velY, cmd.velZ });
            } else if (m.type == MsgType::FleetCommand) {
                // M3 RTS fleet intent (§23.4) — ownership/target validated inside.
                Neuron::Sim::FleetCommand cmd;
                if (Neuron::Sim::DecodeFleetCommand(m.body, cmd))
                    m_universe->ApplyFleetCommand(conn.PlayerNetId(), cmd);
            }
            // M4 area D/B: a client's snapshot ack advances its baselines so the next
            // delta re-deltas from the acked state and tombstones clear. (The Ack
            // message carries the acked snapshot tick; wired when the client emits it.)
        }
    }

    static std::string Key(const Endpoint& e) { return e.ip + ":" + std::to_string(e.port); }

    uint64_t NextToken() noexcept
    {
        // Mix a counter through a splitmix step for non-sequential tokens.
        uint64_t x = (++m_tokenCounter) * 0x9E3779B97F4A7C15ull + 0xABCDEF;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        return x ^ (x >> 31);
    }

    // Per-client snapshot byte budget — the safe-MTU payload less AEAD/header
    // overhead (App. B). BuildBudgetedSnapshot keeps the priority prefix that fits.
    static constexpr size_t kSnapshotByteBudget =
        kMaxPayloadBytes - PacketHeader::kWireSize - kAeadTagBytes;

    ICrypto*                  m_crypto{ nullptr };
    std::vector<uint8_t>      m_staticPriv;
    std::vector<uint8_t>      m_serverSecret;
    Neuron::Sim::ServerUniverse* m_universe{ nullptr };
    uint64_t                  m_serverTime{ 0 };
    HandshakeServerStateless  m_stateless;

    // M4 area G: token-indexed routing. m_table is the generation-tagged slot table
    // (routing + affinity lane); m_conns holds the connection objects keyed by token;
    // m_addrToToken is the *cookie-phase only* ip:port→token association (a peer is
    // routed by ip:port until its first encrypted datagram carries the token).
    ConnectionTable                                       m_table;
    std::unordered_map<uint64_t, ConnEntry>               m_conns;       // token → connection
    std::unordered_map<std::string, uint64_t>             m_addrToToken; // "ip:port" → token (pre-token)

    uint64_t                  m_nowMs{ 0 };
    uint64_t                  m_tokenCounter{ 0 };
    double                    m_dilationFactor{ 1.0 }; // M4 area H — current load floor
};

} // namespace Neuron::Net
