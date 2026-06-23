#pragma once
// ConnectionTable.h — token-indexed connection routing (M4 area G, §9).
//
// Replaces the M1a shortcut (ServerHost keying an unordered_map<"ip:port"> and
// hashing that string on every datagram) with routing by the 64-bit connectionToken
// (App. A header) into a generation-tagged slot table — the same stale-safe handle
// pattern as the ECS. A datagram carries its token; routing is a single u64 lookup
// into a fixed slot array, never a per-datagram string hash or allocation. Recycled
// slots bump a generation so a stale handle to a reused slot is rejected.
//
// Per-connection state (reliability sequence/ack/replay, decrypt nonce) lives in the
// slot, and each connection is pinned to a lane (index % lanes) so IOCP threads never
// race a connection's state — per-connection affinity (§9). This is the pure routing
// table (mirrored on the Linux testrunner, §16.2); the IOCP lane dispatch is the
// Win32 ERServer side.

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Neuron::Net
{

// Datagram-header layout constants for the token-peek router (App. A), kept here so
// the pure routing helper has no dependency on Connection.h (DatagramKind) or the
// Winsock layers — it operates on raw datagram bytes and is unit-testable on Linux.
//   byte 0            : DatagramKind (0x00 ClearHandshake, 0x01 Encrypted)
//   bytes 1..4        : protocol_id  u32   (clear, AEAD AAD)
//   bytes 5..12       : connection_token u64 (clear, AEAD AAD)   ← what we route on
//   bytes 13..20      : packet_number u64
inline constexpr uint8_t kDatagramKindEncrypted = 0x01;
inline constexpr size_t  kTokenByteOffset       = 1 + sizeof(uint32_t); // kind + protocol_id

// Peek the 64-bit connection token out of an *encrypted* datagram's clear header
// (App. A) without decrypting — the fast routing key for ConnectionTable::Find. The
// token is authenticated as AEAD AAD, so SecureChannel::Open still rejects a forged
// header after routing; this is purely "which connection does this datagram belong
// to?" Returns nullopt for a clear-handshake datagram (no token yet → cookie phase)
// or a runt too short to contain the token. Little-endian, matching PacketCodec.h.
[[nodiscard]] inline std::optional<uint64_t> PeekConnectionToken(std::span<const uint8_t> dg) noexcept
{
    if (dg.empty() || dg[0] != kDatagramKindEncrypted) return std::nullopt;
    if (dg.size() < kTokenByteOffset + sizeof(uint64_t)) return std::nullopt;
    uint64_t token = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i)
        token |= static_cast<uint64_t>(dg[kTokenByteOffset + i]) << (8 * i);
    return token;
}

// A stable reference to a connection slot: index + the generation it was opened at.
// Validate() rejects it once the slot is recycled (generation moved on).
struct ConnHandle
{
    uint32_t index{ 0 };
    uint32_t generation{ 0 };
    bool     valid{ false };
    [[nodiscard]] explicit operator bool() const noexcept { return valid; }
};

class ConnectionTable
{
public:
    // Open (or return the existing) slot for 'token'. Reuses a freed slot — bumping
    // its generation so old handles to it are stale — or grows the array. O(1)
    // amortised, one u64 hash, no string anywhere.
    ConnHandle Open(uint64_t token)
    {
        auto it = m_byToken.find(token);
        if (it != m_byToken.end()) return HandleFor(it->second);

        uint32_t idx;
        if (!m_free.empty()) {
            idx = m_free.back();
            m_free.pop_back();
        } else {
            idx = static_cast<uint32_t>(m_slots.size());
            m_slots.push_back({});
        }
        Slot& s = m_slots[idx];
        s.token  = token;
        s.active = true;
        // generation is bumped on Close, so the slot's current generation is correct.
        m_byToken.emplace(token, idx);
        return HandleFor(idx);
    }

    // Route a datagram by its token. Returns an invalid handle if unknown. Pure
    // lookup — no allocation on this hot path.
    [[nodiscard]] ConnHandle Find(uint64_t token) const
    {
        auto it = m_byToken.find(token);
        if (it == m_byToken.end()) return {};
        return HandleFor(it->second);
    }

    // Is 'h' still live? Index in range, slot active, and generation unchanged since
    // the handle was issued (stale-handle safe across slot recycling).
    [[nodiscard]] bool Validate(const ConnHandle& h) const
    {
        if (!h.valid || h.index >= m_slots.size()) return false;
        const Slot& s = m_slots[h.index];
        return s.active && s.generation == h.generation;
    }

    // Close a connection: free its slot and bump the generation so any outstanding
    // handle (or a stale datagram for a recycled slot) is rejected.
    void Close(uint64_t token)
    {
        auto it = m_byToken.find(token);
        if (it == m_byToken.end()) return;
        const uint32_t idx = it->second;
        Slot& s = m_slots[idx];
        s.active = false;
        ++s.generation;
        s.token = 0;
        m_byToken.erase(it);
        m_free.push_back(idx);
    }

    // The lane a connection is affinitised to (§9): all of a connection's
    // decode/reliability/decrypt work runs on one lane, so IOCP threads never race
    // its per-connection state. Stable for the life of the slot.
    [[nodiscard]] static uint32_t Lane(const ConnHandle& h, uint32_t laneCount) noexcept
    {
        return laneCount == 0 ? 0 : (h.index % laneCount);
    }

    [[nodiscard]] size_t ActiveCount() const noexcept { return m_byToken.size(); }
    [[nodiscard]] size_t SlotCapacity() const noexcept { return m_slots.size(); }

    // Per-connection state accessor (reliability/decrypt); valid only while the
    // handle validates. Returns nullptr for a stale/unknown handle.
    struct Slot
    {
        uint64_t token{ 0 };
        uint32_t generation{ 0 };
        bool     active{ false };
        // Per-connection reliability/decrypt state attaches here (fixed-size: ring
        // buffers / bitsets in Connection.h / ReplayWindow.h — no per-message heap).
    };
    [[nodiscard]] Slot* Get(const ConnHandle& h) { return Validate(h) ? &m_slots[h.index] : nullptr; }
    [[nodiscard]] const Slot* Get(const ConnHandle& h) const { return Validate(h) ? &m_slots[h.index] : nullptr; }

private:
    [[nodiscard]] ConnHandle HandleFor(uint32_t idx) const
    {
        return { idx, m_slots[idx].generation, true };
    }

    std::vector<Slot>                       m_slots;
    std::vector<uint32_t>                    m_free;    // recycled slot indices
    std::unordered_map<uint64_t, uint32_t>  m_byToken; // connectionToken → slot index
};

} // namespace Neuron::Net
