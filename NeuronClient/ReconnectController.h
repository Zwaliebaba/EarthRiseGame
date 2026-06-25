#pragma once
// ReconnectController.h — drives the client's reconnect loop after a dropped
// connection (M5 area G, §26, R22).
//
// After a server warm-restart (k8s rolling restart, §26) every connected client drops
// at once. The pure schedule that spreads their retries — exponential backoff with full
// jitter — lives in NeuronCore/Reconnect.h. This controller is the small state machine
// that *uses* that schedule: on connection loss it arms the next attempt, and once per
// session Tick it reports whether the backoff timer has elapsed so the session should
// re-run the §8.5 handshake (re-login with the stored credentials, resume from ∅).
//
// It is pure and deterministic — driven by an injected millisecond clock and the
// seeded JitterRng — so the anti-herd reconnect *behaviour* (not just the delay
// formula) is unit-tested on the Linux runner (ReconnectControllerTests, §16.2), while
// SessionImpl supplies the real steady_clock and the actual socket/handshake.

#include "Reconnect.h"

#include <cstdint>

namespace Neuron::Client
{

// What the session should do this Tick, per the controller.
enum class ReconnectAction : uint8_t
{
    None,     // connected, or still waiting for the backoff timer — do nothing
    Attempt,  // the timer elapsed — fire a reconnect now (re-run handshake/auth)
};

class ReconnectController
{
public:
    ReconnectController() = default;

    // 'policy' is the backoff schedule; 'jitterSeed' seeds the per-client jitter so a
    // fleet of bots (each seeded by its id/name) fans its retries out across the
    // ceiling instead of synchronising (R22). On the real client any seed works.
    explicit ReconnectController(Neuron::Net::ReconnectPolicy policy,
                                 uint64_t jitterSeed = 0) noexcept
        : m_policy(policy), m_rng(jitterSeed) {}

    // Connection is live again — reset the backoff. Call on every (re)connect so the
    // *next* drop starts from the base delay, not wherever the last storm left off.
    void OnConnected() noexcept
    {
        m_attempt       = 0;
        m_waiting       = false;
        m_nextAttemptMs = 0;
    }

    // Connection lost (liveness timeout / stalled-or-rejected handshake). Arms the next
    // attempt with a jittered backoff. Idempotent while already waiting, so repeated
    // loss notifications within one outage don't restart (and thereby lengthen) the
    // timer — only a fired attempt advances the schedule.
    void OnConnectionLost(uint64_t nowMs) noexcept
    {
        if (m_waiting) return;
        ScheduleNext(nowMs);
    }

    // Poll once per Tick. Returns Attempt exactly when the backoff timer has elapsed,
    // and immediately pre-arms the *next* (longer) backoff in case this attempt also
    // fails — so a server that stays down produces 0,1,2,… ceiling waits, never a busy
    // loop. OnConnected() clears that pre-armed wait on success.
    ReconnectAction Poll(uint64_t nowMs) noexcept
    {
        if (!m_waiting)               return ReconnectAction::None;
        if (nowMs < m_nextAttemptMs)  return ReconnectAction::None;
        ++m_attempt;
        ScheduleNext(nowMs);
        return ReconnectAction::Attempt;
    }

    [[nodiscard]] bool     Waiting()       const noexcept { return m_waiting; }
    [[nodiscard]] uint32_t Attempt()       const noexcept { return m_attempt; }
    [[nodiscard]] uint64_t NextAttemptMs() const noexcept { return m_nextAttemptMs; }

private:
    void ScheduleNext(uint64_t nowMs) noexcept
    {
        const uint32_t delay = m_policy.DelayMs(m_attempt, m_rng.Next01());
        m_nextAttemptMs = nowMs + delay;
        m_waiting       = true;
    }

    Neuron::Net::ReconnectPolicy m_policy{};
    Neuron::Net::JitterRng       m_rng{ 0 };
    uint32_t                     m_attempt{ 0 };
    bool                         m_waiting{ false };
    uint64_t                     m_nextAttemptMs{ 0 };
};

} // namespace Neuron::Client
