// Client reconnect backoff + jitter tests (M5 area G; §26, R22). After a server
// warm-restart every client drops at once; exponential backoff with full jitter
// spreads their retries so they don't thundering-herd the freshly-restarted shard.
// Pure schedule; mirrored on the Linux runner (§16.2). The NeuronClient session
// wiring (re-handshake + re-present token) is the Win32/client side.

#include "Reconnect.h"
#include "TestRunner.h"

#include <vector>

using namespace ertest;
using Neuron::Net::JitterRng;
using Neuron::Net::ReconnectPolicy;

ER_TEST(Reconnect, CeilingGrowsExponentiallyThenCaps)
{
    ReconnectPolicy p{ 500, 30000, 2.0 };
    ER_CHECK_EQ(p.CeilingMs(0), uint32_t{ 500 });
    ER_CHECK_EQ(p.CeilingMs(1), uint32_t{ 1000 });
    ER_CHECK_EQ(p.CeilingMs(2), uint32_t{ 2000 });
    ER_CHECK_EQ(p.CeilingMs(3), uint32_t{ 4000 });
    // Saturates at the cap and never exceeds it, however many attempts.
    ER_CHECK_EQ(p.CeilingMs(20), uint32_t{ 30000 });
    ER_CHECK(p.CeilingMs(100) == 30000);
}

ER_TEST(Reconnect, FullJitterStaysWithinCeiling)
{
    ReconnectPolicy p{ 500, 30000, 2.0 };
    JitterRng rng{ 12345 };
    for (uint32_t attempt = 0; attempt < 8; ++attempt) {
        const uint32_t ceil = p.CeilingMs(attempt);
        for (int i = 0; i < 1000; ++i) {
            const uint32_t d = p.DelayMs(attempt, rng.Next01());
            ER_CHECK(d <= ceil); // full jitter: uniform in [0, ceiling]
        }
    }
}

ER_TEST(Reconnect, AFleetSpreadsItsReconnects)
{
    // 50 bots reconnecting at the same attempt must NOT pick the same delay (R22):
    // seed each by its id and confirm the delays fan out across the ceiling.
    ReconnectPolicy p{ 500, 30000, 2.0 };
    const uint32_t attempt = 4;              // ceiling 8000 ms
    std::vector<uint32_t> delays;
    for (uint32_t id = 0; id < 50; ++id) {
        JitterRng rng{ id };
        delays.push_back(p.DelayMs(attempt, rng.Next01()));
    }
    uint32_t lo = delays[0], hi = delays[0];
    int distinct = 0;
    for (size_t i = 0; i < delays.size(); ++i) {
        lo = delays[i] < lo ? delays[i] : lo;
        hi = delays[i] > hi ? delays[i] : hi;
        bool seen = false;
        for (size_t j = 0; j < i; ++j) if (delays[j] == delays[i]) { seen = true; break; }
        if (!seen) ++distinct;
    }
    ER_CHECK(distinct >= 40);          // overwhelmingly distinct, not synchronised
    ER_CHECK((hi - lo) > 4000);        // spread covers most of the 8000 ms window
}

ER_TEST(Reconnect, JitterIsDeterministicPerSeed)
{
    // Reproducible for the gate: same seed → same sequence (so a flaky herd can't
    // hide behind nondeterminism).
    JitterRng a{ 7 }, b{ 7 };
    for (int i = 0; i < 16; ++i) ER_CHECK_EQ(a.Next01(), b.Next01());
}
