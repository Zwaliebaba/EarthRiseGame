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
                HandleAppMessage(m, appOut);
            return true;
        }
        return true;
    }

    // Queue a command to the server (ReliableOrdered). Returns the datagram, if ready.
    std::optional<std::vector<uint8_t>> SendCommand(std::span<const uint8_t> body)
    {
        if (!IsConnected() || !m_channel) return std::nullopt;
        std::vector<uint8_t> payload;
        WriteMessage(payload, Channel::ReliableOrdered, MsgType::Command, body);
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

    void SetLoginName(std::string name) { m_loginName = std::move(name); }

private:
    void SendLogin(std::vector<std::vector<uint8_t>>& sendOut)
    {
        LoginRequestBody req;
        req.username = m_loginName.empty() ? "bot" : m_loginName;
        std::vector<uint8_t> body;
        req.Encode(body);
        std::vector<uint8_t> payload;
        WriteMessage(payload, Channel::ReliableOrdered, MsgType::LoginRequest, body);
        std::vector<uint8_t> dg;
        if (SealEncrypted(payload, dg)) sendOut.push_back(std::move(dg));
    }

    void HandleAppMessage(const DecodedMessage& m, std::vector<AppMessage>& appOut)
    {
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
