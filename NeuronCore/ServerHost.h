// ServerHost — accepts clients, runs the stateless cookie phase, owns the
// per-connection map, and bridges the network to the authoritative ServerUniverse
// (§8.5, §9). Platform-independent: the host loop (ERServer / the integration
// test) supplies datagrams and ferries the produced datagrams to the socket.
#pragma once

#include "Connection.h"
#include "Handshake.h"
#include "ICrypto.h"
#include "ISocket.h"
#include "Protocol.h"
#include "Command.h"
#include "ServerUniverse.h"

#include <cstdint>
#include <memory>
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
        const std::string key = Key(from);

        // Route to an existing connection first.
        if (auto it = m_conns.find(key); it != m_conns.end()) {
            m_lastSeenMs[key] = m_nowMs;
            DispatchToConnection(*it->second, from, dg, out);
            // A client that sent a graceful Disconnect is freed immediately.
            if (it->second->State() == ConnState::Disconnected)
                RemoveConnection(key);
            return;
        }

        // Otherwise this must be part of the stateless cookie phase.
        if (kind != DatagramKind::ClearHandshake || dg.size() < 2) return;
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

            // Each base starts just inside sector 0 (separated on Y) at the
            // centre of the catalog scenery cluster (ServerUniverse::SpawnScenery).
            // Stationary at spawn so the props stay in frame; movement comes from
            // client intents (SetBaseVelocity).
            const int64_t startX = Neuron::Universe::kSectorSize - 200;
            const int64_t startY = static_cast<int64_t>(m_conns.size()) * 2000;
            const uint32_t netId = m_universe->SpawnBase(
                { startX, startY, 0 }, { 0.0f, 0.0f, 0.0f });
            conn->SetPlayerNetId(netId);

            std::vector<std::vector<uint8_t>> sendOut;
            conn->BeginFromCookieResponse(body, sendOut);
            for (auto& d : sendOut) out.push_back({ from, std::move(d) });

            m_conns.emplace(key, std::move(conn));
            m_lastSeenMs[key] = m_nowMs;
            return;
        }
    }

    // Build and queue a snapshot for every connected client.
    void BroadcastSnapshots(std::vector<OutDatagram>& out)
    {
        const auto snap = m_universe->BuildSnapshot();
        const auto body = Neuron::Sim::EncodeSnapshot(snap);
        for (auto& [key, conn] : m_conns) {
            if (!conn->IsConnected()) continue;
            if (auto dg = conn->SealApp(Channel::Unreliable, MsgType::Snapshot, body)) {
                if (auto ep = EndpointFor(key))
                    out.push_back({ *ep, std::move(*dg) });
            }
        }
    }

    [[nodiscard]] size_t ConnectionCount() const noexcept { return m_conns.size(); }
    [[nodiscard]] size_t ConnectedCount() const noexcept
    {
        size_t n = 0;
        for (auto& [k, c] : m_conns) if (c->IsConnected()) ++n;
        return n;
    }

    // The host supplies a monotonic clock (ms) so stale peers can be reaped.
    void SetClockMs(uint64_t nowMs) noexcept { m_nowMs = nowMs; }

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
        for (auto it = m_conns.begin(); it != m_conns.end();) {
            const bool disconnected = it->second->State() == ConnState::Disconnected;
            bool timedOut = false;
            if (!disconnected && timeoutMs != 0) {
                auto ls = m_lastSeenMs.find(it->first);
                if (ls != m_lastSeenMs.end() && m_nowMs >= ls->second &&
                    (m_nowMs - ls->second) > timeoutMs)
                    timedOut = true;
            }
            if (disconnected || timedOut) {
                const uint32_t netId = it->second->PlayerNetId();
                if (m_universe) m_universe->DespawnBase(netId);
                closed.push_back({ it->first, netId, timedOut });
                m_lastSeenMs.erase(it->first);
                it = m_conns.erase(it);
            } else {
                ++it;
            }
        }
        return closed;
    }

private:
    // Despawn the base and drop all per-connection state for one peer.
    void RemoveConnection(const std::string& key)
    {
        auto it = m_conns.find(key);
        if (it == m_conns.end()) return;
        if (m_universe) m_universe->DespawnBase(it->second->PlayerNetId());
        m_conns.erase(it);
        m_lastSeenMs.erase(key);
    }

    void DispatchToConnection(ServerConnection& conn, const Endpoint& from,
                              std::span<const uint8_t> dg, std::vector<OutDatagram>& out)
    {
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

        // Apply received commands (move intents) to the authoritative universe.
        for (const auto& m : appOut) {
            if (m.type == MsgType::Command) {
                Neuron::Sim::MoveCommand cmd;
                if (Neuron::Sim::DecodeMoveCommand(m.body, cmd))
                    m_universe->SetBaseVelocity(conn.PlayerNetId(),
                                             { cmd.velX, cmd.velY, cmd.velZ });
            }
        }
    }

    static std::string Key(const Endpoint& e) { return e.ip + ":" + std::to_string(e.port); }

    std::optional<Endpoint> EndpointFor(const std::string& key) const
    {
        const auto pos = key.rfind(':');
        if (pos == std::string::npos) return std::nullopt;
        Endpoint e;
        e.ip   = key.substr(0, pos);
        e.port = static_cast<uint16_t>(std::stoi(key.substr(pos + 1)));
        return e;
    }

    uint64_t NextToken() noexcept
    {
        // Mix a counter through a splitmix step for non-sequential tokens.
        uint64_t x = (++m_tokenCounter) * 0x9E3779B97F4A7C15ull + 0xABCDEF;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        return x ^ (x >> 31);
    }

    ICrypto*                  m_crypto{ nullptr };
    std::vector<uint8_t>      m_staticPriv;
    std::vector<uint8_t>      m_serverSecret;
    Neuron::Sim::ServerUniverse* m_universe{ nullptr };
    uint64_t                  m_serverTime{ 0 };
    HandshakeServerStateless  m_stateless;
    std::unordered_map<std::string, std::unique_ptr<ServerConnection>> m_conns;
    std::unordered_map<std::string, uint64_t> m_lastSeenMs; // peer key -> last activity (host clock)
    uint64_t                  m_nowMs{ 0 };
    uint64_t                  m_tokenCounter{ 0 };
};

} // namespace Neuron::Net
