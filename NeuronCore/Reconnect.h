#pragma once
// Reconnect.h — client reconnect backoff + jitter policy (M5 area G, §26, R22).
//
// After a server warm-restart (k8s rolling restart, §26) every connected client
// drops at once. If they all retry on the same schedule they form a thundering
// herd that hammers the freshly-restarted shard (R22). The fix is **exponential
// backoff with full jitter**: each client waits a random time in [0, ceiling],
// where the ceiling grows exponentially per failed attempt and is capped. Full
// jitter (uniform in [0, ceiling], not ceiling ± a wobble) gives the widest spread
// and is the standard anti-herd choice.
//
// This is the pure, deterministic policy (mirrored on the Linux testrunner, §16.2);
// the NeuronClient session wires it into the actual reconnect loop (re-run the §8.5
// handshake, re-present the session token, resume the snapshot loop from ∅) — that
// wiring is the Win32/client side. Keeping the schedule pure here means the
// anti-herd property is unit-tested, not just asserted.

#include <cstdint>

namespace Neuron::Net
{

// The backoff schedule. Defaults: 500 ms base, ×2 per attempt, capped at 30 s.
struct ReconnectPolicy
{
    uint32_t baseDelayMs{ 500 };
    uint32_t maxDelayMs{ 30000 };
    double   multiplier{ 2.0 };

    // The (un-jittered) exponential ceiling for a given 0-based attempt:
    //   ceiling = min(maxDelayMs, baseDelayMs * multiplier^attempt)
    // Saturates at maxDelayMs and never overflows (the doubling is clamped each step).
    [[nodiscard]] uint32_t CeilingMs(uint32_t attempt) const noexcept
    {
        double v = static_cast<double>(baseDelayMs);
        for (uint32_t i = 0; i < attempt; ++i) {
            v *= multiplier;
            if (v >= static_cast<double>(maxDelayMs)) return maxDelayMs;
        }
        return v >= static_cast<double>(maxDelayMs) ? maxDelayMs
                                                    : static_cast<uint32_t>(v);
    }

    // Full-jitter delay: uniform in [0, CeilingMs(attempt)], given jitter01 ∈ [0,1).
    // A pure function of (attempt, jitter01) so the schedule is testable; the caller
    // supplies the random draw (JitterRng below, or the platform RNG).
    [[nodiscard]] uint32_t DelayMs(uint32_t attempt, double jitter01) const noexcept
    {
        const double j = jitter01 < 0.0 ? 0.0 : (jitter01 >= 1.0 ? 0.999999 : jitter01);
        return static_cast<uint32_t>(j * static_cast<double>(CeilingMs(attempt)));
    }
};

// Deterministic per-client jitter source (splitmix64) so a fleet of bots spreads
// its reconnects reproducibly in tests — seed each client by its id and the
// reconnect times fan out across the ceiling instead of synchronising. On the real
// client any decent RNG works; the determinism here is purely for the test gate.
class JitterRng
{
public:
    explicit JitterRng(uint64_t seed) noexcept : m_state(seed + 0x9E3779B97F4A7C15ull) {}

    // Next uniform double in [0, 1).
    [[nodiscard]] double Next01() noexcept
    {
        uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z = z ^ (z >> 31);
        // 53-bit mantissa → [0,1).
        return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
    }

private:
    uint64_t m_state;
};

} // namespace Neuron::Net
