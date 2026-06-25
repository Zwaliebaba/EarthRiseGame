#pragma once
// SessionImpl — concrete Session backed by ClientConnection (§8.5, §10.1).
//
// Inject ICrypto* + ISocket* at construction time so the same implementation
// compiles in EarthRise.Client (CngCrypto + WinRT DatagramSocketAdapter, §8.1),
// ERHeadless (CngCrypto + WinsockSocket) and in tests (FakeCrypto + LoopbackSocket).
//
// Thread-model: all calls from the same game-loop thread; no locking.

#include "Session.h"
#include "Connection.h"
#include "ReconnectController.h"
#include "ICrypto.h"
#include "ISocket.h"
#include "Protocol.h"

#include <array>
#include <chrono>
#include <memory>
#include <queue>
#include <span>
#include <string>
#include <vector>

namespace Neuron::Client
{

class SessionImpl final : public Session
{
public:
    SessionImpl(Neuron::Net::ICrypto*          crypto,
                const Neuron::Net::EcPubKey&   pinnedPub,
                Neuron::Net::ISocket*           socket,
                std::string                     loginName = {})
        : m_crypto(crypto)
        , m_pinnedPub(pinnedPub)
        , m_socket(socket)
        , m_loginName(std::move(loginName))
    {}

    // M5 real auth (§14): connect bound to an account (username + password) instead of
    // the dev-stub name-only login. Call before Connect(). The client logs in, auto-
    // registering on first run. Selecting this requires the server to run real auth
    // (server.devAuthStub = false); against a dev-stub server, use the name-only ctor.
    void SetCredentials(std::string username, std::string password)
    {
        m_loginName   = std::move(username);
        m_password    = std::move(password);
        m_useRealAuth = true;
    }

    // Last auth verdict (Neuron::Net::kAuthResult* wire codes; 0xFF until the server
    // answers) — a login screen reads this to show "wrong password / locked / ...".
    [[nodiscard]] uint8_t LastAuthResult() const noexcept
        { return m_conn ? m_conn->LastAuthResult() : 0xFF; }

    // -----------------------------------------------------------------------
    // Session interface
    // -----------------------------------------------------------------------
    bool Connect(std::string_view host, uint16_t port) override
    {
        m_serverEp = Neuron::Net::Endpoint{ std::string(host), port };
        // Seed the reconnect jitter per-client so a fleet that all drops on one
        // warm-restart fans its retries out instead of synchronising (R22). The login
        // name is the natural per-client salt; bots use distinct names.
        m_reconnect = ReconnectController{ Neuron::Net::ReconnectPolicy{}, HashSeed(m_loginName) };
        return DoConnect();
    }

    void Disconnect() override
    {
        // Best-effort graceful notice so the server frees the slot immediately
        // instead of waiting for the inactivity timeout.
        if (m_conn) {
            if (auto dg = m_conn->BuildDisconnect())
                m_socket->SendTo(m_serverEp, *dg);
        }
        m_conn.reset();
        m_state = SessionState::Disconnected;
        while (!m_snapQueue.empty()) m_snapQueue.pop();
    }

    void Tick() override
    {
        const uint64_t nowMs = NowMs();

        // Between reconnect attempts there is no live connection to pump — just wait
        // for the backoff timer, then re-run the handshake with the stored credentials.
        if (m_state == SessionState::Reconnecting) {
            if (m_reconnect.Poll(nowMs) == ReconnectAction::Attempt)
                DoConnect();
            return;
        }

        if (!m_conn) return;

        // Drain inbound datagrams.
        Neuron::Net::Endpoint from;
        int n;
        bool gotTraffic = false;
        while ((n = m_socket->RecvFrom(from, m_recvBuf)) > 0) {
            gotTraffic = true;
            std::vector<Neuron::Net::AppMessage>       appOut;
            std::vector<std::vector<uint8_t>> sendOut;
            m_conn->OnDatagram(
                std::span<const uint8_t>(m_recvBuf.data(), static_cast<size_t>(n)),
                appOut, sendOut);
            for (auto& d : sendOut) m_socket->SendTo(m_serverEp, d);
            for (auto& msg : appOut) {
                if (msg.type == Neuron::Net::MsgType::Snapshot)
                    m_snapQueue.push(std::move(msg.body));
            }
        }
        if (gotTraffic) m_lastRecvMs = nowMs; // liveness: any datagram proves the link

        // Update local state / retransmit as needed.
        if (m_conn->IsConnected()) {
            if (m_state != SessionState::Connected) {
                // Just (re)connected: clear the backoff and seed the liveness clock so a
                // freshly-established link isn't instantly judged stale.
                m_state         = SessionState::Connected;
                m_everConnected = true;
                m_reconnect.OnConnected();
                m_lastRecvMs    = nowMs;
            }
            // Periodic keepalive so the server can distinguish idle-but-alive
            // from gone (it reaps connections with no recent traffic).
            const auto now = std::chrono::steady_clock::now();
            if (now - m_lastKeepalive >= std::chrono::seconds(1)) {
                if (auto dg = m_conn->BuildKeepalive())
                    m_socket->SendTo(m_serverEp, *dg);
                m_lastKeepalive = now;
            }
            // Liveness timeout: no traffic for kConnLostMs (server warm-restart / network
            // drop) → tear down and reconnect with backoff (§26, R22).
            if (nowMs - m_lastRecvMs > kConnLostMs)
                BeginReconnect(nowMs);
        } else if (m_state != SessionState::Disconnected) {
            // Still handshaking/authenticating. A *permanent* auth rejection (wrong
            // password / locked) drops the connection: give up on a first connect, but
            // after a previously-live session treat it as a transient outage and back off.
            if (m_conn->State() == Neuron::Net::ConnState::Disconnected) {
                if (m_everConnected) BeginReconnect(nowMs);
                else                 m_state = SessionState::Disconnected;
                return;
            }
            // A stalled handshake against a still-down server (the warm-restart window):
            // after a live session, give each attempt a bounded budget then back off so we
            // don't sit forever resending into the void (R22).
            if (m_everConnected && nowMs - m_attemptStartMs > kHandshakeTimeoutMs) {
                BeginReconnect(nowMs);
                return;
            }
            // Retransmit the pending handshake/auth frame, but throttled — auth re-sends
            // re-run the server's PBKDF2 (§14), so firing every tick would flood expensive
            // login attempts while a reply is in flight. ~5/s is ample for loss recovery.
            const auto now = std::chrono::steady_clock::now();
            if (now - m_lastResend >= std::chrono::milliseconds(200)) {
                std::vector<std::vector<uint8_t>> rs;
                m_conn->ResendPending(rs);
                for (auto& d : rs) m_socket->SendTo(m_serverEp, d);
                m_lastResend = now;
            }
        }
    }

    [[nodiscard]] SessionState GetState() const noexcept override { return m_state; }
    [[nodiscard]] bool         IsConnected()     const noexcept override
        { return m_state == SessionState::Connected; }
    [[nodiscard]] uint64_t     GetSessionToken() const noexcept override
        { return m_conn ? m_conn->SessionToken() : 0; }

    void SendFleetCommand(std::span<const uint8_t> body) override
    {
        if (!m_conn) return;
        if (auto dg = m_conn->SendFleetCommand(body))
            m_socket->SendTo(m_serverEp, *dg);
    }

    bool PollSnapshot(std::span<uint8_t> outBuf, size_t& outSize) override
    {
        if (m_snapQueue.empty()) return false;
        const auto& front = m_snapQueue.front();
        outSize = std::min(front.size(), outBuf.size());
        std::copy_n(front.begin(), outSize, outBuf.begin());
        m_snapQueue.pop();
        return true;
    }

    // Extended accessors (not on the Session interface).
    [[nodiscard]] uint32_t PlayerNetId() const noexcept
        { return m_conn ? m_conn->PlayerNetId() : 0; }

    // Server time-dilation factor from the last clock-sync echo (M4 area H, §8.5);
    // 1.0 = full speed. The client loop feeds this into InterpBuffer::SetServerDilation
    // so interpolation tracks the dilated authoritative clock, not wall-clock.
    [[nodiscard]] double ServerDilation() const noexcept
        { return m_conn ? m_conn->DilationFactor() : 1.0; }

private:
    // (Re)start the connection: fresh ClientConnection + handshake/login with the
    // stored endpoint and credentials. Shared by Connect() and each reconnect attempt.
    bool DoConnect()
    {
        m_conn = std::make_unique<Neuron::Net::ClientConnection>(m_crypto, m_pinnedPub);
        if (m_useRealAuth)             m_conn->SetCredentials(m_loginName, m_password);
        else if (!m_loginName.empty()) m_conn->SetLoginName(m_loginName);
        m_state          = SessionState::Handshaking;
        m_attemptStartMs = NowMs();
        m_lastRecvMs     = m_attemptStartMs; // don't time out before the first reply
        const uint64_t nowUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        auto hello = m_conn->Start(nowUs);
        m_socket->SendTo(m_serverEp, hello);
        return true;
    }

    // Tear down a dead connection and arm a backed-off, jittered retry (§26, R22). The
    // controller (idempotent while waiting) owns the schedule; Tick() fires the attempt.
    void BeginReconnect(uint64_t nowMs)
    {
        m_conn.reset();
        while (!m_snapQueue.empty()) m_snapQueue.pop();
        m_state = SessionState::Reconnecting;
        m_reconnect.OnConnectionLost(nowMs);
    }

    [[nodiscard]] static uint64_t NowMs() noexcept
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // FNV-1a over the login name → a per-client jitter seed (R22 fan-out).
    [[nodiscard]] static uint64_t HashSeed(const std::string& s) noexcept
    {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        return h;
    }

    // Liveness/backoff tuning. kConnLostMs matches the server's idle-reap window so a
    // warm-restart is noticed promptly; kHandshakeTimeoutMs bounds a stalled retry.
    static constexpr uint64_t kConnLostMs         = 8000;
    static constexpr uint64_t kHandshakeTimeoutMs = 5000;

    Neuron::Net::ICrypto*                          m_crypto;
    Neuron::Net::EcPubKey                          m_pinnedPub;
    Neuron::Net::ISocket*                          m_socket;
    std::string                                    m_loginName;
    std::string                                    m_password;
    bool                                           m_useRealAuth{ false };
    std::unique_ptr<Neuron::Net::ClientConnection> m_conn;
    Neuron::Net::Endpoint                          m_serverEp;
    SessionState                                   m_state{ SessionState::Disconnected };
    std::queue<std::vector<uint8_t>>               m_snapQueue;
    std::array<uint8_t, 2048>                      m_recvBuf{};
    std::chrono::steady_clock::time_point          m_lastKeepalive{};
    std::chrono::steady_clock::time_point          m_lastResend{};
    // Reconnect state (M5 area G, §26, R22).
    ReconnectController                            m_reconnect{};
    bool                                           m_everConnected{ false };
    uint64_t                                       m_lastRecvMs{ 0 };
    uint64_t                                       m_attemptStartMs{ 0 };
};

} // namespace Neuron::Client
