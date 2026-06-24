#pragma once
// Connection driver — ties handshake + SecureChannel + datagram framing into a
// per-peer state machine usable by both the client and the server (§8.5).
//
// Platform-independent: operates on datagram byte buffers (the host loop does the
// socket I/O), so the whole connect→login→snapshot path is exercised end-to-end
// over the in-memory LoopbackNetwork in the integration tests — including the
// M1a loss/reorder/dup acceptance criterion.
//
// Wire framing: every datagram begins with a 1-byte DatagramKind:
//   0x00 ClearHandshake : [kind][msgType u8][handshake body]      (pre-keys)
//   0x01 Encrypted      : [kind][PacketHeader][AES-GCM ct+tag]    (post-keys)
// The encrypted payload carries 1..N framed messages (PacketCodec.h).

#include "Handshake.h"
#include "HandshakeMessages.h"
#include "ICrypto.h"
#include "PacketCodec.h"
#include "Protocol.h"
#include "SecureChannel.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Neuron::Net
{

enum class DatagramKind : uint8_t { ClearHandshake = 0x00, Encrypted = 0x01 };

// Build a clear handshake datagram from a handshake message.
inline std::vector<uint8_t> MakeHandshakeDatagram(MsgType type, std::span<const uint8_t> body)
{
    std::vector<uint8_t> d;
    d.reserve(2 + body.size());
    d.push_back(static_cast<uint8_t>(DatagramKind::ClearHandshake));
    d.push_back(static_cast<uint8_t>(type));
    d.insert(d.end(), body.begin(), body.end());
    return d;
}

// A decoded app message handed to the owner once the channel is established.
struct AppMessage
{
    Channel              channel{};
    MsgType              type{};
    std::vector<uint8_t> body;
};

// ---------------------------------------------------------------------------
// ClientConnection
// ---------------------------------------------------------------------------
class ClientConnection
{
public:
    ClientConnection(ICrypto* crypto, const EcPubKey& pinnedServerPub) noexcept
        : m_crypto(crypto), m_pinnedPub(pinnedServerPub) {}

    // Begin the handshake. Returns the first datagram to send (ClientHello).
    std::vector<uint8_t> Start(uint64_t clientTimeMicros)
    {
        m_client.emplace(m_crypto, m_pinnedPub, clientTimeMicros);
        HsOutput o = m_client->Begin();
        m_lastHandshakeDatagram = MakeHandshakeDatagram(o.type, o.body);
        m_state = ConnState::Handshaking;
        return m_lastHandshakeDatagram;
    }

    [[nodiscard]] ConnState State() const noexcept { return m_state; }
    [[nodiscard]] bool      IsConnected() const noexcept { return m_state == ConnState::Connected; }
    [[nodiscard]] uint64_t  Token() const noexcept { return m_token; }
    [[nodiscard]] uint32_t  PlayerNetId() const noexcept { return m_playerNetId; }
    // Server time-dilation factor from the last clock-sync response (M4 area H,
    // §7.2/§8.5); 1.0 = full speed. The client interpolator scales server-time
    // advance by this so it tracks the dilated authoritative clock, not wall-clock.
    [[nodiscard]] double    DilationFactor() const noexcept
    {
        return m_client ? m_client->DilationFactor() : 1.0;
    }

    // Process an inbound datagram. Appends any decoded app messages to 'appOut'
    // and any datagrams to send back to 'sendOut'. Returns false on fatal error.
    bool OnDatagram(std::span<const uint8_t> dg,
                    std::vector<AppMessage>& appOut,
                    std::vector<std::vector<uint8_t>>& sendOut)
    {
        if (dg.empty()) return true;
        const auto kind = static_cast<DatagramKind>(dg[0]);
        std::span<const uint8_t> rest = dg.subspan(1);

        if (kind == DatagramKind::ClearHandshake) {
            if (rest.size() < 1) return true;
            const auto type = static_cast<MsgType>(rest[0]);
            HsOutput o = m_client->OnMessage(type, rest.subspan(1));
            if (o.failed) { m_state = ConnState::Disconnected; return false; }
            if (o.send) {
                m_lastHandshakeDatagram = MakeHandshakeDatagram(o.type, o.body);
                sendOut.push_back(m_lastHandshakeDatagram);
            }
            // After clock sync the client has keys: build the secure channel,
            // then send the login request.
            if (m_client->IsComplete() && m_client->HasKeys() && !m_channel) {
                m_token = m_client->Token();
                m_channel.emplace(m_crypto, Direction::ClientToServer, m_token);
                m_channel->SetKeys(m_client->KeyC2S(), m_client->KeyS2C(), m_client->Epoch());
                m_state = ConnState::Authenticating;
                SendLogin(sendOut);
            }
            return true;
        }

        if (kind == DatagramKind::Encrypted) {
            if (!m_channel) return true; // not ready yet; drop
            std::vector<uint8_t> payload;
            auto r = m_channel->Open(rest, payload);
            if (r != SecureChannel::OpenResult::Ok) return true; // drop replays/forgeries
            std::vector<DecodedMessage> msgs;
            if (!ReadMessages(payload, msgs)) return true;
            for (const auto& m : msgs)
                HandleAppMessage(m, appOut, sendOut);
            return true;
        }
        return true;
    }

    // Queue a command to the server (ReliableOrdered). Returns the datagram, if ready.
    std::optional<std::vector<uint8_t>> SendCommand(std::span<const uint8_t> body)
    {
        return SendTyped(MsgType::Command, body);
    }

    // Queue an RTS fleet intent (§23.4; M3 area B). Same reliable path, distinct
    // message type so the server routes it to ApplyFleetCommand.
    std::optional<std::vector<uint8_t>> SendFleetCommand(std::span<const uint8_t> body)
    {
        return SendTyped(MsgType::FleetCommand, body);
    }

    std::optional<std::vector<uint8_t>> SendTyped(MsgType type, std::span<const uint8_t> body)
    {
        if (!IsConnected() || !m_channel) return std::nullopt;
        std::vector<uint8_t> payload;
        WriteMessage(payload, Channel::ReliableOrdered, type, body);
        std::vector<uint8_t> dg;
        if (!SealEncrypted(payload, dg)) return std::nullopt;
        return dg;
    }

    // Best-effort graceful disconnect (encrypted Disconnect). Requires keys; the
    // owner sends this on shutdown so the server frees the slot immediately
    // rather than waiting for the inactivity timeout.
    std::optional<std::vector<uint8_t>> BuildDisconnect()
    {
        if (!m_channel) return std::nullopt;
        const uint8_t reason = static_cast<uint8_t>(DisconnectReason::Normal);
        std::vector<uint8_t> payload;
        WriteMessage(payload, Channel::ReliableOrdered, MsgType::Disconnect,
                     std::span<const uint8_t>(&reason, 1));
        std::vector<uint8_t> dg;
        if (!SealEncrypted(payload, dg)) return std::nullopt;
        return dg;
    }

    // Periodic liveness ping so the server can tell an idle-but-alive client from
    // a gone one (the server reaps connections with no recent traffic).
    std::optional<std::vector<uint8_t>> BuildKeepalive()
    {
        if (!IsConnected() || !m_channel) return std::nullopt;
        std::vector<uint8_t> payload;
        // Unreliable: a dropped ping just misses one refresh (sent every 1 s, 8 s
        // timeout), and avoids reliability-window bookkeeping for sustained pings.
        WriteMessage(payload, Channel::Unreliable, MsgType::Keepalive,
                     std::span<const uint8_t>{});
        std::vector<uint8_t> dg;
        if (!SealEncrypted(payload, dg)) return std::nullopt;
        return dg;
    }

    [[nodiscard]] bool HandshakePending() const noexcept
    {
        return m_state == ConnState::Handshaking || m_state == ConnState::Authenticating;
    }

    // Re-send whatever this side is still waiting to be answered (host calls this
    // on a timeout). Handshake datagrams are not under the reliability layer, so
    // the connection drives their retransmission itself.
    void ResendPending(std::vector<std::vector<uint8_t>>& sendOut)
    {
        if (m_state == ConnState::Handshaking) {
            // Clear handshake datagrams carry no AEAD packet number, so replaying
            // the identical bytes is safe.
            if (!m_lastHandshakeDatagram.empty()) sendOut.push_back(m_lastHandshakeDatagram);
        } else if (m_state == ConnState::Authenticating && m_channel) {
            // Re-seal the login with a fresh AEAD packet number — replaying the
            // stored bytes would trip the server's replay window.
            SendLogin(sendOut);
        }
    }

    // Dev-stub login (M1-M4): a bare username, no password. Selects the legacy
    // LoginRequest path the server's dev-auth stub answers at cookie time.
    void SetLoginName(std::string name) { m_loginName = std::move(name); }

    // M5 real auth (§14): bind to an account with a password over the secure channel.
    // The client tries to LOGIN first; if the account does not exist yet it auto-
    // REGISTERS and logs in (friendly first-run provisioning). A login/registration UI
    // can replace this with an explicit register-vs-login choice by setting m_authOpcode
    // before connect. Call before Connect().
    void SetCredentials(std::string username, std::string password)
    {
        m_loginName   = std::move(username);
        m_password    = std::move(password);
        m_realAuth    = true;
        m_authOpcode  = kAuthOpcodeLogin; // first attempt: login; fall back to register
        m_triedRegister = false;
    }

    // Result of the last completed auth exchange (kAuthResult* wire codes); 0xFF until
    // the server replies. A login screen reads this to show "wrong password", etc.
    [[nodiscard]] uint8_t LastAuthResult() const noexcept { return m_lastAuthResult; }

private:
    void SendLogin(std::vector<std::vector<uint8_t>>& sendOut)
    {
        if (m_realAuth) { SendAuth(sendOut, m_authOpcode); return; }

        // Dev-stub path: legacy username-only LoginRequest (no real auth on the server).
        LoginRequestBody req;
        req.username = m_loginName.empty() ? "bot" : m_loginName;
        std::vector<uint8_t> body;
        req.Encode(body);
        std::vector<uint8_t> payload;
        WriteMessage(payload, Channel::ReliableOrdered, MsgType::LoginRequest, body);
        std::vector<uint8_t> dg;
        if (SealEncrypted(payload, dg)) sendOut.push_back(std::move(dg));
    }

    // Send a register/login credential frame: a reliable Command whose body is
    // [opcode u8][u16 userLen LE][user][u16 passLen LE][pass] (matches ServerHost's
    // DecodeAuthRequest). Re-sealed each time so a retransmit gets a fresh AEAD number.
    void SendAuth(std::vector<std::vector<uint8_t>>& sendOut, uint8_t opcode)
    {
        std::vector<uint8_t> body;
        body.reserve(1 + 2 + m_loginName.size() + 2 + m_password.size());
        body.push_back(opcode);
        auto putStr = [&body](const std::string& s) {
            const uint16_t n = static_cast<uint16_t>(s.size());
            body.push_back(static_cast<uint8_t>(n & 0xFF));
            body.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
            body.insert(body.end(), s.begin(), s.end());
        };
        putStr(m_loginName);
        putStr(m_password);
        std::vector<uint8_t> payload;
        WriteMessage(payload, Channel::ReliableOrdered, MsgType::Command, body);
        std::vector<uint8_t> dg;
        if (SealEncrypted(payload, dg)) sendOut.push_back(std::move(dg));
    }

    void HandleAppMessage(const DecodedMessage& m, std::vector<AppMessage>& appOut,
                          std::vector<std::vector<uint8_t>>& sendOut)
    {
        // M5 real-auth result: a Command frame opening with kAuthOpcodeResult, shape
        // [opcode][AuthResult u8][netId u32 LE][tokenLo u64 LE] (ServerHost::SendAuthResult).
        if (m.type == MsgType::Command && !m.body.empty() && m.body[0] == kAuthOpcodeResult) {
            if (m.body.size() < 1 + 1 + 4 + 8) return; // malformed — ignore
            // Auth (re)sends fire each tick until we connect, so several credential frames
            // can be in flight and the server answers each one. Only act while the auth is
            // still unresolved; once we've connected (Ok) or failed, ignore the stragglers.
            if (m_state != ConnState::Authenticating) return;
            const uint8_t res = m.body[1];
            m_lastAuthResult = res;
            if (res == kAuthResultOk) {
                uint32_t netId = 0;
                for (int i = 0; i < 4; ++i) netId |= static_cast<uint32_t>(m.body[2 + i]) << (i * 8);
                uint64_t tokenLo = 0;
                for (int i = 0; i < 8; ++i) tokenLo |= static_cast<uint64_t>(m.body[6 + i]) << (i * 8);
                m_playerNetId  = netId;
                m_sessionToken = tokenLo;
                m_state        = ConnState::Connected;
            } else if (res == kAuthResultInvalidCredentials && !m_triedRegister
                       && m_authOpcode == kAuthOpcodeLogin) {
                // Account doesn't exist yet → auto-provision: register, which auto-logs in.
                m_triedRegister = true;
                m_authOpcode    = kAuthOpcodeRegister;
                SendAuth(sendOut, m_authOpcode);
            } else if (res == kAuthResultInvalidCredentials && m_authOpcode == kAuthOpcodeRegister) {
                // A duplicate login frame's "no such account" arriving after we already
                // switched to register — stale, ignore (the register result is authoritative).
            } else {
                // Wrong password on an existing account (login failed, register → taken),
                // or locked / rate-limited / banned / bad input → give up. The owner reads
                // LastAuthResult() to surface the reason on a login screen.
                m_state = ConnState::Disconnected;
            }
            return;
        }
        if (m.type == MsgType::LoginResponse) {
            LoginResponseBody resp;
            if (LoginResponseBody::Decode(m.body, resp) && resp.success) {
                m_sessionToken = resp.sessionToken;
                m_playerNetId  = resp.playerNetworkId;
                m_state        = ConnState::Connected;
            } else {
                m_state = ConnState::Disconnected;
            }
            return;
        }
        appOut.push_back({ m.channel, m.type, { m.body.begin(), m.body.end() } });
    }

    bool SealEncrypted(std::span<const uint8_t> payload, std::vector<uint8_t>& outDg)
    {
        std::vector<uint8_t> sealed;
        if (!m_channel->Seal(payload, sealed)) return false;
        outDg.clear();
        outDg.push_back(static_cast<uint8_t>(DatagramKind::Encrypted));
        outDg.insert(outDg.end(), sealed.begin(), sealed.end());
        return true;
    }

    ICrypto*  m_crypto{ nullptr };
    EcPubKey  m_pinnedPub{};
    std::optional<HandshakeClient> m_client;
    std::optional<SecureChannel>   m_channel;
    ConnState m_state{ ConnState::Idle };
    uint64_t  m_token{ 0 };
    uint64_t  m_sessionToken{ 0 };
    uint32_t  m_playerNetId{ 0 };
    std::string m_loginName;
    // M5 real-auth state (§14). m_realAuth selects the account/password path over the
    // legacy dev-stub LoginRequest; m_authOpcode is the credential frame in flight
    // (login first, register on auto-provision); m_lastAuthResult is the server's verdict.
    std::string m_password;
    bool        m_realAuth{ false };
    bool        m_triedRegister{ false };
    uint8_t     m_authOpcode{ kAuthOpcodeLogin };
    uint8_t     m_lastAuthResult{ 0xFF };
    std::vector<uint8_t> m_lastHandshakeDatagram;
};
// (login retransmission re-seals via SendLogin to get a fresh AEAD packet number)

// ---------------------------------------------------------------------------
// ServerConnection — one per accepted client (created after cookie verify).
// ---------------------------------------------------------------------------
class ServerConnection
{
public:
    ServerConnection(ICrypto* crypto, std::span<const uint8_t> staticPriv,
                     uint64_t connectionToken, uint64_t serverTimeMicros)
        : m_crypto(crypto)
        , m_server(crypto, staticPriv, connectionToken, serverTimeMicros)
        , m_token(connectionToken) {}

    [[nodiscard]] ConnState State() const noexcept { return m_state; }
    [[nodiscard]] bool      IsConnected() const noexcept { return m_state == ConnState::Connected; }
    [[nodiscard]] uint64_t  Token() const noexcept { return m_token; }
    [[nodiscard]] uint32_t  PlayerNetId() const noexcept { return m_playerNetId; }
    void SetPlayerNetId(uint32_t id) noexcept { m_playerNetId = id; }
    [[nodiscard]] const std::string& LoginName() const noexcept { return m_loginName; }

    // M5 area C (§14): under real auth the account login runs in the host
    // (ServerHost::OnAuthMessage) rather than the connection's app-message handler,
    // so the host flips the connection to Connected once auth succeeds — mirroring
    // what the dev-stub LoginRequest path does inline. No-op unless the secure
    // channel is up (state Authenticating); the snapshot broadcaster only sends to
    // Connected peers, so without this a logged-in client never receives entities.
    void MarkConnected() noexcept
    {
        if (m_state == ConnState::Authenticating) m_state = ConnState::Connected;
    }

    // M4 area H (§7.2/§8.5): the host pushes the server's current time-dilation
    // factor each loop so the clock-sync echo (OnClockSyncRequest) publishes it to
    // the client. Cheap setter; no effect until the next clock-sync response is sent.
    void SetDilationFactor(double f) noexcept { m_server.SetDilationFactor(f); }

    // Feed the (already cookie-verified) CookieResponse to begin ECDH.
    bool BeginFromCookieResponse(std::span<const uint8_t> body,
                                 std::vector<std::vector<uint8_t>>& sendOut)
    {
        HsOutput o = m_server.OnCookieResponse(body);
        if (o.failed) { m_state = ConnState::Disconnected; return false; }
        if (o.send) {
            m_lastHandshakeDatagram = MakeHandshakeDatagram(o.type, o.body);
            sendOut.push_back(m_lastHandshakeDatagram);
        }
        m_state = ConnState::Handshaking;
        return true;
    }

    // Re-send the last handshake datagram (HandshakeResponse). The host calls
    // this when a still-handshaking client repeats its CookieResponse, i.e. our
    // response was lost. Clear handshake datagrams carry no packet number.
    void ResendHandshake(std::vector<std::vector<uint8_t>>& sendOut)
    {
        if (m_state == ConnState::Handshaking && !m_lastHandshakeDatagram.empty())
            sendOut.push_back(m_lastHandshakeDatagram);
    }

    // Process an inbound datagram from this client.
    bool OnDatagram(std::span<const uint8_t> dg,
                    std::vector<AppMessage>& appOut,
                    std::vector<std::vector<uint8_t>>& sendOut)
    {
        if (dg.empty()) return true;
        const auto kind = static_cast<DatagramKind>(dg[0]);
        std::span<const uint8_t> rest = dg.subspan(1);

        if (kind == DatagramKind::ClearHandshake) {
            if (rest.size() < 1) return true;
            const auto type = static_cast<MsgType>(rest[0]);
            if (type == MsgType::ClockSyncRequest) {
                HsOutput o = m_server.OnClockSyncRequest(rest.subspan(1));
                if (o.failed) { m_state = ConnState::Disconnected; return false; }
                if (o.send) sendOut.push_back(MakeHandshakeDatagram(o.type, o.body));
                // Keys are ready; build the secure channel for app traffic.
                if (m_server.IsComplete() && m_server.HasKeys() && !m_channel) {
                    m_channel.emplace(m_crypto, Direction::ServerToClient, m_token);
                    m_channel->SetKeys(m_server.KeyS2C(), m_server.KeyC2S(), m_server.Epoch());
                    m_state = ConnState::Authenticating;
                }
            }
            return true;
        }

        if (kind == DatagramKind::Encrypted) {
            if (!m_channel) return true;
            std::vector<uint8_t> payload;
            auto r = m_channel->Open(rest, payload);
            if (r != SecureChannel::OpenResult::Ok) return true;
            std::vector<DecodedMessage> msgs;
            if (!ReadMessages(payload, msgs)) return true;
            for (const auto& m : msgs)
                HandleAppMessage(m, appOut, sendOut);
            return true;
        }
        return true;
    }

    // Seal an app payload (e.g. a snapshot) into an encrypted datagram.
    std::optional<std::vector<uint8_t>> SealApp(Channel ch, MsgType type,
                                                std::span<const uint8_t> body)
    {
        if (!m_channel) return std::nullopt;
        std::vector<uint8_t> payload;
        WriteMessage(payload, ch, type, body);
        std::vector<uint8_t> sealed;
        if (!m_channel->Seal(payload, sealed)) return std::nullopt;
        std::vector<uint8_t> dg;
        dg.push_back(static_cast<uint8_t>(DatagramKind::Encrypted));
        dg.insert(dg.end(), sealed.begin(), sealed.end());
        return dg;
    }

private:
    void HandleAppMessage(const DecodedMessage& m,
                          std::vector<AppMessage>& appOut,
                          std::vector<std::vector<uint8_t>>& sendOut)
    {
        if (m.type == MsgType::Disconnect) {
            m_state = ConnState::Disconnected; // host reaps the slot + despawns the base
            return;
        }
        if (m.type == MsgType::Keepalive) {
            return; // liveness only; the host stamps activity on every datagram
        }
        if (m.type == MsgType::LoginRequest) {
            LoginRequestBody req;
            (void)LoginRequestBody::Decode(m.body, req); // dev mode: accept any name (§14 stub)
            m_loginName = req.username;

            LoginResponseBody resp;
            resp.success         = 1;
            resp.sessionToken    = m_token ^ 0x5555AAAA5555AAAAull;
            resp.playerNetworkId = m_playerNetId; // host assigns before login completes
            std::vector<uint8_t> body;
            resp.Encode(body);
            if (auto dg = SealApp(Channel::ReliableOrdered, MsgType::LoginResponse, body))
                sendOut.push_back(std::move(*dg));
            m_state = ConnState::Connected;
            return;
        }
        appOut.push_back({ m.channel, m.type, { m.body.begin(), m.body.end() } });
    }

    ICrypto*  m_crypto{ nullptr };
    HandshakeServerConnection m_server;
    std::optional<SecureChannel> m_channel;
    ConnState m_state{ ConnState::Idle };
    uint64_t  m_token{ 0 };
    uint32_t  m_playerNetId{ 0 };
    std::string m_loginName;
    std::vector<uint8_t> m_lastHandshakeDatagram;
};

} // namespace Neuron::Net
