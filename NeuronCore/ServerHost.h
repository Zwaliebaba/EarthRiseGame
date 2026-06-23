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
//
// M5 area C (§14/§8.5): account-bound auth replaces the dev "pick a name" identity.
// After the encrypted channel is up, ServerHost handles register/login over the
// secure channel via the persist AccountStore (→ a 32-byte session token), validates
// that token on subsequent reliable traffic, enforces one active session per account,
// and spawns or restores the player's base bound to the AccountId. The persist deps
// (AccountStore + PersistenceThread) are *injected* by ERServer (SetPersistDeps) and
// are nullable, so ServerHost stays constructible without them for the existing tests;
// a dev-stub flag (default OFF = real auth) keeps the M1-M4 "accept any name" path for
// local iteration. The economy events the auth/base lifecycle produces are routed to
// the persistence thread (write-through outbox), never the 30 Hz tick (§9).
//
// M4/M5: Windows integration — unverified on Linux; validate on the build agent.
// The auth credential exchange rides reliable MsgType::Command frames with a 1-byte
// ServerHost auth opcode (kAuthOpcode*) because the §8.5 wire types (Protocol.h) and
// the M1 LoginRequest body (HandshakeMessages.h) are frozen here and cannot carry a
// password or a 32-byte token; this seam keeps the real-auth flow inside ServerHost
// without touching the connection/protocol layer. See OnAuthMessage.
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
#include "PersistTelemetry.h"

// M5: persist deps injected from ERServer. Resolved via the ERServer project's
// include dirs ($(ProjectDir) = ERServer\, so "persist/..." finds ERServer\persist\,
// the same form ERServer.cpp uses). These are the Win32/ODBC layer (unverified on
// Linux); ServerHost only calls their public APIs and holds *nullable* pointers, so
// the auth path is inert until ERServer injects them (SetPersistDeps). This couples
// ServerHost.h to the persist headers' include path, which is fine: ERServer is the
// only TU that compiles ServerHost.h (no testrunner/integration suite includes it).
#include "persist/AccountStore.h"
#include "persist/PersistenceThread.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
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

    // M5 area C: inject the persist deps + telemetry from ERServer. Nullable — until
    // this is called, real auth has no AccountStore and rejects every login (the host
    // is still constructible for tests with no DB). 'devAuthStub' = true restores the
    // M1-M4 "pick a name" base-at-cookie path (default false = real account-bound auth).
    // 'tel' (optional) records the §21 auth counters (area H) at the live login sites.
    void SetPersistDeps(Neuron::Persist::AccountStore* accounts,
                        Neuron::Persist::PersistenceThread* persist,
                        bool devAuthStub = false,
                        Neuron::Sim::PersistTelemetry* tel = nullptr) noexcept
    {
        m_accounts    = accounts;
        m_persist     = persist;
        m_devAuthStub = devAuthStub;
        m_persistTel  = tel;
    }

    // A wall-clock seconds source for the auth layer (session TTL / lockout windows
    // are Unix-time, distinct from the monotonic ms clock SetClockMs feeds the reaper).
    // ERServer pushes std::time(nullptr) each loop; defaults to 0 (tests).
    void SetUnixTime(int64_t nowUnix) noexcept { m_nowUnix = nowUnix; }

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

            // Cookie valid → allocate the connection and begin ECDH. The base is NOT
            // spawned here under real auth (M5 area C): identity is unknown until the
            // player registers/logs in over the secure channel, so the spawn/restore
            // is deferred to OnAuthMessage. The dev stub keeps the M1-M4 behaviour of
            // spawning a "pick a name" base at cookie time (flag default OFF).
            const uint64_t token = NextToken();
            auto conn = std::make_unique<ServerConnection>(m_crypto, m_staticPriv, token, m_serverTime);
            conn->SetDilationFactor(m_dilationFactor); // M4 area H — current load floor

            uint32_t netId = 0;
            if (m_devAuthStub) {
                // Dev stub (§14 "pick a name behind a dev flag for M1-M4"): spawn the
                // player's base + starter fleet at cookie time, identified by net id.
                netId = SpawnPlayerWorld(static_cast<int64_t>(m_conns.size()));
                conn->SetPlayerNetId(netId);
            }

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
            entry.authed      = m_devAuthStub; // stub is "pre-authed"; real auth gates below
            m_conns.emplace(token, std::move(entry));
            m_addrToToken[key] = token;
            if (netId != 0) (void)m_universe->Baseline(netId); // pre-create baseline (area B/F)
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
        // M5 area C — account binding. 'authed' gates gameplay: true once the player
        // has registered/logged in (or, under the dev stub, at cookie time). The
        // session token + account id back the one-session rule + reconnect validation.
        bool                              authed{ false };
        int64_t                           accountId{ 0 };
        Neuron::Persist::SessionToken     sessionToken{};
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
                // M5 area C: the auth credential exchange rides reliable Command frames
                // with a 1-byte ServerHost auth opcode (the §8.5 wire types are frozen
                // here — see the header note). A real-auth connection that is not yet
                // authenticated routes EVERY Command through the auth handler; a stray
                // gameplay Command before login is dropped (server-authoritative).
                if (!m.body.empty() && IsAuthOpcode(m.body[0])) {
                    OnAuthMessage(entry, from, m.body, out);
                    continue;
                }
                if (!entry.authed) continue; // gate gameplay until logged in (real auth)

                // Legacy M1a base-velocity move intent.
                Neuron::Sim::MoveCommand cmd;
                if (Neuron::Sim::DecodeMoveCommand(m.body, cmd))
                    m_universe->SetBaseVelocity(entry.playerNetId,
                                             { cmd.velX, cmd.velY, cmd.velZ });
            } else if (m.type == MsgType::FleetCommand) {
                if (!entry.authed) continue; // gate gameplay until logged in (real auth)
                // M3 RTS fleet intent (§23.4) — ownership/target validated inside.
                Neuron::Sim::FleetCommand cmd;
                if (Neuron::Sim::DecodeFleetCommand(m.body, cmd))
                    m_universe->ApplyFleetCommand(entry.playerNetId, cmd);
            }
            // M4 area D/B: a client's snapshot ack advances its baselines so the next
            // delta re-deltas from the acked state and tombstones clear. (The Ack
            // message carries the acked snapshot tick; wired when the client emits it.)
        }
    }

    // --- M5 area C: account-bound auth over the secure channel (§14/§8.5) ---------
    // ServerHost auth sub-protocol opcodes (first byte of a reliable Command frame —
    // see the header note on why this isn't a new MsgType). Request bodies carry
    // [opcode u8][u16 userLen][user][u16 passLen][pass]; the token reply rides an
    // existing LoginResponse-shaped Command frame echoing a session-token digest.
    enum : uint8_t {
        kAuthOpcodeRegister = 0xA0, // register a new account, then auto-login
        kAuthOpcodeLogin    = 0xA1, // login to an existing account
        kAuthOpcodeResult   = 0xA2, // server → client: [opcode][AuthResult u8][netId u32][tokenLo u64]
    };
    [[nodiscard]] static bool IsAuthOpcode(uint8_t b) noexcept
    {
        return b == kAuthOpcodeRegister || b == kAuthOpcodeLogin;
    }

    // Handle a register/login frame on an established secure channel. Validates input,
    // calls the persist AccountStore (real PBKDF2 + lockout + one-session rule), and on
    // success spawns (first login) or restores (returning) the account's base bound to
    // its AccountId, then replies with the auth result + the player net id. All DB work
    // is synchronous on the AccountStore here — that runs on the host loop thread, which
    // is acceptable because login is rare and off the per-tick path; the high-frequency
    // economy/state writes go through the persistence thread, never here (§9).
    void OnAuthMessage(ConnEntry& entry, const Endpoint& from,
                       const std::vector<uint8_t>& body, std::vector<OutDatagram>& out)
    {
        const uint8_t opcode = body[0];
        std::string user, pass;
        if (!DecodeAuthRequest(body, user, pass)) {
            SendAuthResult(entry, from, Neuron::Persist::AuthResult::BadInput, out);
            return;
        }

        // No AccountStore injected (e.g. degraded/no-DB run) → auth is unavailable.
        if (!m_accounts) {
            SendAuthResult(entry, from, Neuron::Persist::AuthResult::DbUnavailable, out);
            return;
        }

        if (m_persistTel) m_persistTel->RecordLoginAttempt();

        Neuron::Persist::SessionInfo session;
        Neuron::Persist::AuthResult res = Neuron::Persist::AuthResult::InvalidCredentials;
        if (opcode == kAuthOpcodeRegister) {
            // Register, then immediately log in to mint the session token (§14).
            res = m_accounts->Register(user, pass, m_nowUnix, session);
            if (res == Neuron::Persist::AuthResult::Ok)
                res = m_accounts->Login(user, pass, entry.endpoint.ip, m_nowUnix, session);
        } else { // kAuthOpcodeLogin
            res = m_accounts->Login(user, pass, entry.endpoint.ip, m_nowUnix, session);
        }

        if (res != Neuron::Persist::AuthResult::Ok) {
            if (m_persistTel) {
                m_persistTel->RecordLoginFailure();
                if (res == Neuron::Persist::AuthResult::AccountLocked) m_persistTel->RecordLockout();
                if (res == Neuron::Persist::AuthResult::RateLimited)   m_persistTel->RecordRateLimitHit();
            }
            SendAuthResult(entry, from, res, out);
            return;
        }

        // One active session per account at the ServerHost layer too: AccountStore
        // already revoked the prior DB session (atomic, §14), so kick any currently-
        // connected peer bound to this account before binding the new one (no double-
        // spawn; the reaped peer's base is handed over, not duplicated).
        KickExistingSessionForAccount(session.accountId, entry.conn->Token());

        // Spawn (first login, baseId==0) or restore (returning) the account's base,
        // bound to the AccountId (§14 "login binds the session to the Base/entity").
        const uint32_t netId = BindAccountBase(session);
        entry.conn->SetPlayerNetId(netId);
        entry.playerNetId  = netId;
        entry.authed       = true;
        entry.accountId    = session.accountId;
        entry.sessionToken = session.token;
        (void)m_universe->Baseline(netId); // pre-create per-client baseline (area B/F)

        SendAuthResult(entry, from, Neuron::Persist::AuthResult::Ok, out);
    }

    // Validate a session token presented on reconnect/per-request reliable traffic
    // (§14). True if the token matches the connection's bound session and the store
    // still considers it live. Public seam for the reconnect path / tests.
    [[nodiscard]] bool ValidateSession(ConnEntry& entry)
    {
        if (!entry.authed) return false;
        if (!m_accounts) return true; // no store → trust the in-process bind (dev/no-DB)
        auto info = m_accounts->ValidateToken(entry.sessionToken, m_nowUnix);
        return info.has_value() && info->accountId == entry.accountId;
    }

private:
    // Spawn a player's base + a starter harvester + seed ore (the M1-M4 dev seed; under
    // real auth this is the first-login bootstrap). 'slot' spaces bases out on Y so a
    // pileup of fresh logins doesn't co-locate. Returns the base net id.
    uint32_t SpawnPlayerWorld(int64_t slot)
    {
        const int64_t startX = Neuron::Universe::kSectorSize - 200;
        const int64_t startY = slot * 2000;
        const uint32_t netId = m_universe->SpawnBase({ startX, startY, 0 }, { 0.0f, 0.0f, 0.0f });
        m_universe->SpawnFleetShip(netId, Neuron::Sim::ServerUniverse::ShipShapeId(),
                                   { startX + 220, startY, 0 });
        if (auto* st = m_universe->StorageOf(netId))
            st->amount[static_cast<int>(Neuron::Sim::ResourceType::Ore)] = 600.0f;
        return netId;
    }

    // Bind a logged-in account to its base entity. First login (baseId==0) spawns a
    // fresh world; a returning account whose base was rebuilt from the warm-restart
    // snapshot at startup (ERServer → ServerUniverse::RestoreState) re-attaches to the
    // existing entity by its persisted net id (the BaseId column holds the netId at this
    // layer). The DB binding is (re)written via SetAccountBase. NOTE: the per-account
    // restore *of the base components* lives in the startup RestoreState; here we only
    // resolve the live net id and persist the binding. The OwnerId↔AccountId mapping is
    // NOT mutated on the live entity — ServerUniverse::CaptureState records OwnerId.player
    // as ownerAccount and the auth layer keeps the AccountId↔netId map in SQL (§14/§15);
    // ServerUniverse exposes no OwnerId setter and is read-only at this layer.
    uint32_t BindAccountBase(const Neuron::Persist::SessionInfo& session)
    {
        uint32_t netId = 0;
        const bool firstLogin =
            !(session.baseId != 0 &&
              m_universe->GetBasePos(static_cast<uint32_t>(session.baseId), m_scratchPos));
        if (!firstLogin) {
            // Returning player: the warm-restart snapshot already rebuilt this base.
            netId = static_cast<uint32_t>(session.baseId);
        } else {
            // First login (or base missing from the restore) → spawn the starter world.
            netId = SpawnPlayerWorld(static_cast<int64_t>(m_conns.size()));
        }
        // Persist the netId↔account binding (idempotent) so a later restart's restore +
        // login resolves the same base. Off-tick (login is rare); fine to call here.
        if (m_accounts) (void)m_accounts->SetAccountBase(session.accountId, netId);

        // First-login starting credit → the write-through economy outbox via the
        // persistence thread (M5 area D, §15): enqueue ONLY (O(1), non-blocking); the
        // thread commits it transactionally (zero-loss). The idemKey is derived from the
        // account id so a replay after a crash cannot double-credit (Outbox idempotency,
        // migration 004). This is the ServerHost-side economy hook; gameplay economy
        // (build completion) is routed from ERServer's drain (§9 — never on the tick).
        if (firstLogin && m_persist) {
            Neuron::Persist::EconomyMutation seed;
            seed.idemKey   = 0x5EED0000ull ^ static_cast<uint64_t>(session.accountId);
            seed.accountId = session.accountId;
            seed.amount    = kStartingCredits;
            seed.kind      = Neuron::Persist::EconomyEventKind::WalletDelta;
            seed.reason    = "starting_credits";
            seed.refType   = "Account";
            seed.refId     = session.accountId;
            (void)m_persist->EnqueueEconomy(seed);
        }
        return netId;
    }

    // Kick any currently-connected peer already bound to 'accountId' (one active session
    // per account, §14) — except 'keepToken' (the new session). The kicked peer keeps
    // its base (the new session re-binds to it); only the connection is dropped.
    void KickExistingSessionForAccount(int64_t accountId, uint64_t keepToken)
    {
        std::vector<uint64_t> kick;
        for (auto& [token, e] : m_conns)
            if (token != keepToken && e.authed && e.accountId == accountId)
                kick.push_back(token);
        for (uint64_t token : kick) {
            // Drop the duplicate connection WITHOUT despawning the base (the new session
            // owns it now). RemoveConnection despawns, so close the routing slot directly.
            if (auto it = m_conns.find(token); it != m_conns.end()) {
                m_addrToToken.erase(Key(it->second.endpoint));
                m_table.Close(token);
                m_conns.erase(it);
            }
        }
    }

    // [opcode u8][u16 userLen][user][u16 passLen][pass] → (user, pass). Bounds-checked.
    [[nodiscard]] static bool DecodeAuthRequest(const std::vector<uint8_t>& body,
                                                std::string& user, std::string& pass)
    {
        size_t off = 1; // skip opcode
        auto readStr = [&](std::string& s) -> bool {
            if (off + 2 > body.size()) return false;
            const uint16_t n = static_cast<uint16_t>(body[off] | (body[off + 1] << 8));
            off += 2;
            if (off + n > body.size()) return false;
            s.assign(reinterpret_cast<const char*>(body.data() + off), n);
            off += n;
            return true;
        };
        return readStr(user) && readStr(pass);
    }

    // Reply [kAuthOpcodeResult][AuthResult u8][netId u32][tokenLo u64] on the reliable
    // channel. tokenLo is the low 8 bytes of the 32-byte session token — a digest the
    // client echoes on reconnect; the full token never leaves the server in clear-shape
    // (the §8.5 channel is already encrypted, this is just the wire-frozen reply shape).
    void SendAuthResult(ConnEntry& entry, const Endpoint& from,
                        Neuron::Persist::AuthResult res, std::vector<OutDatagram>& out)
    {
        std::vector<uint8_t> b;
        b.reserve(1 + 1 + 4 + 8);
        b.push_back(kAuthOpcodeResult);
        b.push_back(static_cast<uint8_t>(res));
        const uint32_t netId = entry.playerNetId;
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(netId >> (i * 8)));
        uint64_t tokenLo = 0;
        std::memcpy(&tokenLo, entry.sessionToken.data(), sizeof(tokenLo));
        for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(tokenLo >> (i * 8)));
        if (auto dg = entry.conn->SealApp(Channel::ReliableOrdered, MsgType::Command, b))
            out.push_back({ from, std::move(*dg) });
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

    // First-login wallet seed (M5 area D). A named code constant; the real onboarding
    // grant is data-driven at M7. Routed through the zero-loss outbox, idempotent.
    static constexpr int64_t kStartingCredits = 1000;

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

    // M5 area C — injected persist deps (nullable; see SetPersistDeps). The auth path
    // is inert until ERServer injects them. m_devAuthStub keeps the M1-M4 "pick a name"
    // base-at-cookie behaviour (default OFF = real account-bound auth).
    Neuron::Persist::AccountStore*      m_accounts{ nullptr };
    Neuron::Persist::PersistenceThread* m_persist{ nullptr };
    Neuron::Sim::PersistTelemetry*      m_persistTel{ nullptr };
    bool                                m_devAuthStub{ false };
    int64_t                             m_nowUnix{ 0 };       // wall-clock seconds (SetUnixTime)
    Neuron::Universe::UniversePos       m_scratchPos{};       // scratch for GetBasePos probes
};

} // namespace Neuron::Net
