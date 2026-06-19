#pragma once
// 16-bit sequence-number arithmetic with wraparound — §8.3.
//
// Per-channel reliability uses 16-bit sequences that wrap at 65536. Comparing
// them requires modular ("serial number", RFC 1982-style) arithmetic so that a
// sequence just after a wrap is correctly seen as "newer" than one just before.

#include <cstdint>

namespace Neuron::Net
{

// True if s1 is strictly more recent than s2, accounting for 16-bit wraparound.
// Window is half the sequence space (32768).
[[nodiscard]] inline bool SeqGreater(uint16_t s1, uint16_t s2) noexcept
{
    return ((s1 > s2) && (s1 - s2 <= 32768)) ||
           ((s1 < s2) && (s2 - s1 >  32768));
}

[[nodiscard]] inline bool SeqLess(uint16_t s1, uint16_t s2) noexcept
{
    return SeqGreater(s2, s1);
}

// Signed distance s1 - s2 with wraparound (positive if s1 is newer).
[[nodiscard]] inline int SeqDiff(uint16_t s1, uint16_t s2) noexcept
{
    return static_cast<int16_t>(static_cast<uint16_t>(s1 - s2));
}

} // namespace Neuron::Net
