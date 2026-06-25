// Client reconnect *controller* tests (M5 area G; §26, R22). ReconnectTests.cpp covers
// the pure backoff/jitter schedule; this covers the state machine the session drives
// with it — when a dropped link arms a retry, when Poll fires an attempt, that the
// backoff grows across repeated failures, that a successful connect resets it, and that
// a fleet's first attempts fan out instead of synchronising (anti-herd). Deterministic
// (injected ms clock + seeded jitter) so it runs on the Linux runner (§16.2).

#include "ReconnectController.h"
#include "TestRunner.h"

#include <vector>

using namespace ertest;
using Neuron::Client::ReconnectAction;
using Neuron::Client::ReconnectController;
using Neuron::Net::ReconnectPolicy;

ER_TEST(ReconnectCtl, IdleControllerDoesNothing)
{
    ReconnectController c{ ReconnectPolicy{}, 1 };
    ER_CHECK(!c.Waiting());
    // No loss reported → Poll never asks for an attempt, however far time advances.
    ER_CHECK(c.Poll(0) == ReconnectAction::None);
    ER_CHECK(c.Poll(1'000'000) == ReconnectAction::None);
}

ER_TEST(ReconnectCtl, LostArmsAndPollFiresAfterTimer)
{
    ReconnectController c{ ReconnectPolicy{ 500, 30000, 2.0 }, 7 };
    c.OnConnectionLost(1000);
    ER_CHECK(c.Waiting());
    const uint64_t due = c.NextAttemptMs();
    ER_CHECK(due >= 1000 && due <= 1000 + 500); // first ceiling is the 500 ms base

    // Before the timer: nothing. At/after the timer: exactly one Attempt.
    ER_CHECK(c.Poll(due - 1) == ReconnectAction::None);
    ER_CHECK(c.Poll(due) == ReconnectAction::Attempt);
}

ER_TEST(ReconnectCtl, BackoffGrowsAcrossRepeatedFailures)
{
    // A server that stays down: each fired attempt pre-arms a longer ceiling, so the
    // gaps between successive attempts trend upward toward the cap (never a busy loop).
    ReconnectController c{ ReconnectPolicy{ 500, 30000, 2.0 }, 42 };
    uint64_t now = 0;
    c.OnConnectionLost(now);

    std::vector<uint64_t> ceilings; // the per-attempt ceiling we waited within
    uint64_t lastFire = now;
    for (int i = 0; i < 6; ++i) {
        now = c.NextAttemptMs();            // jump to when the attempt is due
        ER_CHECK(c.Poll(now) == ReconnectAction::Attempt);
        ceilings.push_back(now - lastFire); // the delay we actually waited this round
        lastFire = now;
    }
    // Delays are bounded by the exponential ceilings 500,1000,2000,4000,8000,16000.
    const uint32_t expectCeil[6] = { 500, 1000, 2000, 4000, 8000, 16000 };
    for (int i = 0; i < 6; ++i) ER_CHECK(ceilings[i] <= expectCeil[i]);
    // And the schedule genuinely escalates: the last window dwarfs the first.
    ER_CHECK(ceilings[5] >= ceilings[0]);
    ER_CHECK(c.Attempt() == 6u);
}

ER_TEST(ReconnectCtl, ConnectedResetsBackoff)
{
    ReconnectController c{ ReconnectPolicy{ 500, 30000, 2.0 }, 9 };
    c.OnConnectionLost(0);
    // Burn a few attempts so the ceiling has climbed.
    for (int i = 0; i < 4; ++i) c.Poll(c.NextAttemptMs());
    ER_CHECK(c.Attempt() == 4u);

    c.OnConnected();                 // link came back
    ER_CHECK(!c.Waiting());
    ER_CHECK(c.Attempt() == 0u);

    // A *new* drop starts again from the base ceiling, not where the old storm ended.
    c.OnConnectionLost(100000);
    ER_CHECK(c.NextAttemptMs() - 100000 <= 500);
}

ER_TEST(ReconnectCtl, RepeatedLossWhileWaitingDoesNotRestartTimer)
{
    // Several "still down" notifications within one outage must not push the attempt
    // further out each time (that would starve reconnection). Only a fired attempt moves it.
    ReconnectController c{ ReconnectPolicy{ 500, 30000, 2.0 }, 3 };
    c.OnConnectionLost(1000);
    const uint64_t due = c.NextAttemptMs();
    c.OnConnectionLost(1100);
    c.OnConnectionLost(1200);
    ER_CHECK_EQ(c.NextAttemptMs(), due);
}

ER_TEST(ReconnectCtl, FleetFirstAttemptsFanOut)
{
    // 50 clients drop on the same warm-restart at t=0; seeded by id, their first
    // attempt times must spread across the base ceiling rather than all firing together.
    ReconnectPolicy p{ 500, 30000, 2.0 };
    std::vector<uint64_t> due;
    for (uint64_t id = 0; id < 50; ++id) {
        ReconnectController c{ p, id };
        c.OnConnectionLost(0);
        due.push_back(c.NextAttemptMs());
    }
    uint64_t lo = due[0], hi = due[0];
    int distinct = 0;
    for (size_t i = 0; i < due.size(); ++i) {
        lo = due[i] < lo ? due[i] : lo;
        hi = due[i] > hi ? due[i] : hi;
        bool seen = false;
        for (size_t j = 0; j < i; ++j) if (due[j] == due[i]) { seen = true; break; }
        if (!seen) ++distinct;
    }
    ER_CHECK(distinct >= 40);     // overwhelmingly distinct, not a synchronised herd
    ER_CHECK((hi - lo) > 250);    // spread covers a good fraction of the 500 ms window
}
