#pragma once
// AEAD replay protection — §8.3.
//
// A sliding window over the 64-bit packet_number that rejects stale or duplicate
// packet numbers on decrypt. This is independent of the per-channel reliability
// ack bitfield: it operates on the monotonic AEAD packet counter and is the
// defence against replayed encrypted datagrams (R13).
//
// Algorithm (mirrors IPsec / DTLS anti-replay):
//   - Track the highest packet number seen (m_highest).
//   - A bitmask covers the kWindowBits numbers ending at m_highest.
//   - A number > m_highest slides the window forward.
//   - A number within the window is accepted iff its bit is unset (then set).
//   - A number older than the window's tail is rejected outright.

#include <cstdint>

namespace Neuron::Net
{

class ReplayWindow
{
public:
    static constexpr int kWindowBits = 1024;
    static constexpr int kWords      = kWindowBits / 64;

    // Check-and-update. Returns true if the packet number is fresh (accept),
    // false if it is a replay or too old (reject).
    [[nodiscard]] bool CheckAndUpdate(uint64_t packetNumber) noexcept
    {
        if (!m_seenAny) {
            m_seenAny = true;
            m_highest = packetNumber;
            SetBit(0);
            return true;
        }

        if (packetNumber > m_highest) {
            // Slide window forward by the gap.
            const uint64_t shift = packetNumber - m_highest;
            SlideForward(shift);
            m_highest = packetNumber;
            SetBit(0); // bit 0 always represents m_highest
            return true;
        }

        // packetNumber <= m_highest: offset back from the head.
        const uint64_t offset = m_highest - packetNumber;
        if (offset >= static_cast<uint64_t>(kWindowBits))
            return false; // older than the window — reject

        if (TestBit(offset))
            return false; // duplicate — reject

        SetBit(offset);
        return true;
    }

    [[nodiscard]] uint64_t Highest()  const noexcept { return m_highest; }
    [[nodiscard]] bool     SeenAny()  const noexcept { return m_seenAny; }

    void Reset() noexcept
    {
        for (auto& w : m_bits) w = 0;
        m_highest = 0;
        m_seenAny = false;
    }

private:
    // Bit 'offset' counts back from m_highest (offset 0 == m_highest).
    [[nodiscard]] bool TestBit(uint64_t offset) const noexcept
    {
        return (m_bits[offset / 64] >> (offset % 64)) & 1ull;
    }
    void SetBit(uint64_t offset) noexcept
    {
        m_bits[offset / 64] |= (1ull << (offset % 64));
    }

    // Shift the whole bitmask "down" (toward older offsets) by 'shift' bits,
    // because m_highest is advancing. Bits that fall past kWindowBits are lost.
    void SlideForward(uint64_t shift) noexcept
    {
        if (shift >= static_cast<uint64_t>(kWindowBits)) {
            for (auto& w : m_bits) w = 0;
            return;
        }
        const int wordShift = static_cast<int>(shift / 64);
        const int bitShift  = static_cast<int>(shift % 64);

        if (bitShift == 0) {
            for (int i = kWords - 1; i >= 0; --i)
                m_bits[i] = (i - wordShift >= 0) ? m_bits[i - wordShift] : 0;
        } else {
            for (int i = kWords - 1; i >= 0; --i) {
                uint64_t v = 0;
                const int lo = i - wordShift;
                const int hi = i - wordShift - 1;
                if (lo >= 0) v |= m_bits[lo] << bitShift;
                if (hi >= 0) v |= m_bits[hi] >> (64 - bitShift);
                m_bits[i] = v;
            }
        }
    }

    uint64_t m_bits[kWords]{};
    uint64_t m_highest{ 0 };
    bool     m_seenAny{ false };
};

} // namespace Neuron::Net
