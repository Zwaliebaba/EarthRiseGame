#pragma once
// TimeDilation.h — bounded time-dilation policy (M4 area H, §7.2 / §9).
//
// The fixed step is the sim clock. When a tick consistently overruns its real-time
// budget, the shard *stretches the real-time spacing* of the fixed step — slowing
// in-game time toward a floor — instead of dropping ticks: EVE-style graceful
// degradation, the load-shedding floor (R23). The step count and order are never
// touched, only how much wall-clock each step is allowed to take, so the authoritative
// SimHash is identical dilated vs un-dilated for the same input log (§16.1) — only
// timing differs. The dilation factor is published in the clock-sync echo (§8.5) so
// clients interpolate against server time, not wall-clock.
//
// This is the pure policy (mirrored on the Linux testrunner, §16.2). The Win32
// FixedStepAccumulator feeds it the measured per-tick cost and scales its elapsed-time
// intake by Factor(); the platform clock stays in the accumulator.

#include <algorithm>

namespace Neuron::Sim
{

// Dilation tunables (§7.2 floor + onset). Authored, not literal — area J sweeps them.
struct DilationConfig
{
    double floor     = 0.10; // slowest in-game speed (10%); the degradation floor
    double onsetLoad = 1.00; // dilate once measured tick cost / budget exceeds this
    double easeDown  = 0.25; // onset responsiveness toward the needed factor (fast)
    double easeUp    = 0.05; // recovery responsiveness back toward full speed (slow)
};

// Tracks a dilation factor in [floor, 1]. Factor 1 = real time; factor d < 1 slows
// in-game time to d× so each fixed step is allowed budget/d real seconds.
class DilationController
{
public:
    [[nodiscard]] double Factor() const noexcept { return m_factor; }
    [[nodiscard]] bool   IsDilated() const noexcept { return m_factor < 1.0; }
    void Reset() noexcept { m_factor = 1.0; }

    // Feed the real time 'measuredSeconds' the last sim tick took against the
    // real-time step 'budgetSeconds' (SIM_DELTA_SECONDS). Returns the updated factor.
    // Over budget → ease down toward onset/load (clamped to the floor); under budget
    // → ease back up toward full speed. Asymmetric easing: shed fast, recover slow.
    double Update(double measuredSeconds, double budgetSeconds, const DilationConfig& cfg) noexcept
    {
        const double budget = budgetSeconds > 0.0 ? budgetSeconds : 1e-9;
        const double load   = measuredSeconds / budget; // > 1 = overrunning the budget

        double target = 1.0;
        if (load > cfg.onsetLoad) {
            target = std::clamp(cfg.onsetLoad / load, cfg.floor, 1.0); // dilated budget == cost
        }
        const double ease = (target < m_factor) ? cfg.easeDown : cfg.easeUp;
        m_factor += (target - m_factor) * ease;
        m_factor = std::clamp(m_factor, cfg.floor, 1.0);
        if (m_factor > 1.0 - 1e-6) m_factor = 1.0; // snap to full speed (easing is asymptotic)
        return m_factor;
    }

private:
    double m_factor{ 1.0 };
};

} // namespace Neuron::Sim
