#pragma once
// Datagram (de)serialization — Appendix A wire layout.
//
// Writes/reads the clear-text PacketHeader (which is authenticated as AEAD AAD)
// and the per-message framing inside the payload. Byte order is little-endian
// (all EarthRise targets are little-endian: x64 / ARM64). Platform-independent.

#include "Protocol.h"

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace Neuron::Net
{

// ---------------------------------------------------------------------------
// Little-endian primitive helpers
// ---------------------------------------------------------------------------
inline void PutU16(std::vector<uint8_t>& b, uint16_t v)
{
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
inline void PutU32(std::vector<uint8_t>& b, uint32_t v)
{
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
inline void PutU64(std::vector<uint8_t>& b, uint64_t v)
{
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

inline uint16_t GetU16(std::span<const uint8_t> b, size_t& off)
{
    const uint16_t v = static_cast<uint16_t>(b[off]) |
                       (static_cast<uint16_t>(b[off + 1]) << 8);
    off += 2;
    return v;
}
inline uint32_t GetU32(std::span<const uint8_t> b, size_t& off)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b[off + i]) << (8 * i);
    off += 4;
    return v;
}
inline uint64_t GetU64(std::span<const uint8_t> b, size_t& off)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[off + i]) << (8 * i);
    off += 8;
    return v;
}

// ---------------------------------------------------------------------------
// PacketHeader (clear-text, used as AEAD AAD)
// ---------------------------------------------------------------------------
inline void WritePacketHeader(std::vector<uint8_t>& out, const PacketHeader& h)
{
    PutU32(out, h.protocolId);
    PutU64(out, h.connectionToken);
    PutU64(out, h.packetNumber);
}

// Returns false if there are not enough bytes for a full header.
[[nodiscard]] inline bool ReadPacketHeader(std::span<const uint8_t> in, PacketHeader& h, size_t& off)
{
    if (in.size() < PacketHeader::kWireSize) return false;
    off = 0;
    h.protocolId      = GetU32(in, off);
    h.connectionToken = GetU64(in, off);
    h.packetNumber    = GetU64(in, off);
    return true;
}

// ---------------------------------------------------------------------------
// Message framing (inside the encrypted payload)
//   [channel u8][msgType u8][length u16][body ...]
// ---------------------------------------------------------------------------
inline void WriteMessage(std::vector<uint8_t>& out, Channel ch, MsgType type,
                         std::span<const uint8_t> body)
{
    out.push_back(static_cast<uint8_t>(ch));
    out.push_back(static_cast<uint8_t>(type));
    PutU16(out, static_cast<uint16_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

struct DecodedMessage
{
    Channel                  channel{};
    MsgType                  type{};
    std::span<const uint8_t> body;
};

// Decode all messages in a (decrypted) payload. Returns false on malformed framing.
[[nodiscard]] inline bool ReadMessages(std::span<const uint8_t> payload,
                                       std::vector<DecodedMessage>& out)
{
    size_t off = 0;
    while (off < payload.size()) {
        if (off + 4 > payload.size()) return false;
        DecodedMessage m;
        m.channel = static_cast<Channel>(payload[off++]);
        m.type    = static_cast<MsgType>(payload[off++]);
        const uint16_t len = GetU16(payload, off);
        if (off + len > payload.size()) return false;
        m.body = payload.subspan(off, len);
        off += len;
        out.push_back(m);
    }
    return true;
}

} // namespace Neuron::Net
