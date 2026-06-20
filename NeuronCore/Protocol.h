#pragma once
// Network protocol contracts — §8 and Appendix A of the masterplan.
//
// This header is the single source of truth for the wire format shared by
// ERServer, NeuronClient and ERHeadless. It is platform-independent (no Winsock,
// no CNG) so it can be unit-tested on any host.
//
// Datagram layout (post-handshake; payload is AES-GCM AEAD):
//   Header (authenticated as AAD, sent in clear):
//     protocol_id      u32   — magic + protocol version gate (§8.5 step 2)
//     connection_token u64   — anti-spoof (§8.3)
//     packet_number    u64   — monotonic; AEAD nonce input + replay window (§8.3)
//   Payload (encrypted): 1..N messages
//     each message: channel u8, msg_type u8, length u16, body[length]
//     reliability framing per channel: sequence u16, ack u16, ack_bits u32
//
// There is no fragmentation: every message fits one safe-MTU datagram by
// construction (§8.4 — no bulk world sync; cold-start streams interest-scoped
// delta snapshots from an empty baseline).
//
// The 64-bit packet_number is the AEAD nonce input and feeds the replay window.
// The 16-bit per-channel sequence is reliability/ordering only (Appendix A note).

#include <cstdint>

namespace Neuron::Net
{

// ---------------------------------------------------------------------------
// Protocol identity / versioning (§8.5 step 2 — version gate)
// ---------------------------------------------------------------------------
// High 16 bits: fixed magic ('ER'). Low 16 bits: protocol version.
inline constexpr uint16_t kProtocolMagic   = 0x4552; // 'E','R'
inline constexpr uint16_t kProtocolVersion = 1;
inline constexpr uint32_t kProtocolId =
    (static_cast<uint32_t>(kProtocolMagic) << 16) | kProtocolVersion;

[[nodiscard]] inline bool IsProtocolCompatible(uint32_t protocolId) noexcept
{
    return protocolId == kProtocolId;
}

// ---------------------------------------------------------------------------
// Transport limits
// ---------------------------------------------------------------------------
inline constexpr uint16_t kMaxPayloadBytes  = 1200; // safe UDP MTU payload (App. B)
inline constexpr uint16_t kMaxDatagramBytes = 1280; // header + payload + AEAD tag
inline constexpr uint16_t kAeadTagBytes     = 16;   // AES-GCM tag
inline constexpr uint16_t kAeadNonceBytes   = 12;   // GCM nonce (dir-bit ‖ counter)

// ---------------------------------------------------------------------------
// Channels (§8.2)
// ---------------------------------------------------------------------------
enum class Channel : uint8_t
{
    Unreliable       = 0, // snapshots
    ReliableOrdered  = 1, // commands / chat / events
    ReliableUnordered= 2, // notifications
    Count
    // No Bulk channel: the world is never shipped as one large artifact (§8.4).
};
inline constexpr uint8_t kChannelCount = static_cast<uint8_t>(Channel::Count);

// ---------------------------------------------------------------------------
// Message types (payload-level; §8.5 connection sequence + gameplay)
// ---------------------------------------------------------------------------
enum class MsgType : uint8_t
{
    // -- Connection establishment (pre-encryption, unauthenticated) --
    ClientHello       = 1,  // step 1: client → server (no state)
    Cookie            = 2,  // step 1: server → client (stateless cookie)
    CookieResponse    = 3,  // step 1: client returns cookie
    VersionReject     = 4,  // step 2: server rejects on protocol mismatch
    HandshakeRequest  = 5,  // step 3: client ECDH pub key
    HandshakeResponse = 6,  // step 3: server ECDH pub key + ECDSA signature
    ClockSyncRequest  = 7,  // step 4: client timestamp echo request
    ClockSyncResponse = 8,  // step 4: server timestamp echo

    // -- Post-encryption (over the encrypted channel) --
    LoginRequest      = 20, // step 5: username/password (dev: name only)
    LoginResponse     = 21, // step 5: session token or failure
    // No WorldSync* messages: a fresh client enters the snapshot loop directly with
    // an empty baseline and converges via interest-scoped delta snapshots (§8.4).

    Snapshot          = 40, // server → client per-tick delta snapshot
    Command           = 41, // client → server intent
    Ack               = 42, // standalone ack (also piggybacked in headers)
    Keepalive         = 43,
    Disconnect        = 44,
};

// ---------------------------------------------------------------------------
// Datagram header (Appendix A) — serialized in clear, authenticated as AAD.
// ---------------------------------------------------------------------------
struct PacketHeader
{
    uint32_t protocolId{ kProtocolId };
    uint64_t connectionToken{ 0 };
    uint64_t packetNumber{ 0 };

    static constexpr size_t kWireSize = sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint64_t); // 20
};

// Per-message framing within the encrypted payload.
struct MessageHeader
{
    uint8_t  channel{ 0 };  // Channel
    uint8_t  msgType{ 0 };  // MsgType
    uint16_t length{ 0 };   // body length in bytes
};

// Reliability framing (carried for reliable channels).
struct ReliabilityHeader
{
    uint16_t sequence{ 0 };
    uint16_t ack{ 0 };       // most recent received sequence
    uint32_t ackBits{ 0 };   // bitfield of the 32 sequences before 'ack'
};

// ---------------------------------------------------------------------------
// Connection lifecycle states (server + client share this enum; §8.5)
// ---------------------------------------------------------------------------
enum class ConnState : uint8_t
{
    Idle,
    SentHello,        // client: waiting for cookie
    SentCookieResp,   // client: waiting for handshake response / version verdict
    Handshaking,      // ECDH in flight
    ClockSyncing,     // step 4
    Authenticating,   // login in flight (encrypted)
    Connected,        // in tick/snapshot loop (cold-start = empty baseline, §8.4)
    Disconnected,
};

// Disconnect reasons (sent in Disconnect / VersionReject bodies).
enum class DisconnectReason : uint8_t
{
    Normal            = 0,
    Timeout           = 1,
    ProtocolMismatch  = 2,
    BadCookie         = 3,
    HandshakeFailure  = 4, // ECDSA verify failed / MITM suspected
    AuthFailure       = 5,
    ReplayDetected    = 6,
    ServerFull        = 7,
    DuplicateSession  = 8, // one active session per account (§14)
};

} // namespace Neuron::Net
