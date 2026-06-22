#pragma once
// Fixed-step sim accumulator — §7.2 / §9 of the masterplan.
//
// The authoritative 30 Hz simulation uses a real time accumulator:
//   1. Measure wall-clock elapsed time each frame.
//   2. Add it to the accumulator.
//   3. Consume whole fixed steps (each kSimDeltaSeconds long).
//   4. Clamp catch-up to kMaxCatchUpTicksPerFrame to prevent spiral-of-death.
//   5. Carry the remainder as alpha ∈ [0, 1) for rendering interpolation.
//
// WinRT-free: uses only Win32 QueryPerformanceCounter.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "TimeDilation.h" // M4 area H — bounded time-dilation policy (§7.2)

#include <cassert>
#include <cstdint>

namespace Neuron::Sim
{

// Constants — match §7.2 / TimerCore.h
static constexpr int    kSimTicksPerSecond      = 30;
static constexpr double kSimDeltaSeconds        = 1.0 / kSimTicksPerSecond;
static constexpr int    kMaxCatchUpTicksPerFrame = 5;

// ---------------------------------------------------------------------------
// FixedStepAccumulator
//
// Usage:
//   FixedStepAccumulator acc;
//   acc.Start();                          // call once before the loop
//
//   // Each OS frame / each time ERServer's outer loop wakes:
//   acc.Tick();                           // measure elapsed, add to accumulator
//   while (acc.ConsumeStep())             // true while a full 30 Hz step is ready
//       SimulateOneTick();
//   float alpha = acc.GetAlpha();        // interpolation fraction for renderer
// ---------------------------------------------------------------------------
class FixedStepAccumulator
{
public:
    void Start() noexcept
    {
        QueryPerformanceFrequency(&m_freq);
        QueryPerformanceCounter(&m_last);
        m_accumulator    = 0.0;
        m_simTickCount   = 0;
        m_stepsThisFrame = 0;
        m_dilation.Reset();
    }

    // Measure elapsed wall time since the last Tick() call and add to accumulator.
    // Call once per outer-loop iteration, before the ConsumeStep() loop.
    void Tick() noexcept
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        // Elapsed in seconds. Clamp to guard against debugger pauses etc.
        double elapsed = static_cast<double>(now.QuadPart - m_last.QuadPart)
                       / static_cast<double>(m_freq.QuadPart);

        // Hard clamp: never add more than kMaxCatchUpTicksPerFrame steps-worth.
        const double maxElapsed = kSimDeltaSeconds * kMaxCatchUpTicksPerFrame;
        if (elapsed > maxElapsed) elapsed = maxElapsed;

        // Time dilation (M4 area H, §7.2): when the sim is overrunning its budget,
        // the controller's factor < 1 throttles how much real time enters the
        // accumulator, so fewer fixed steps drain per real second — in-game time
        // slows toward the floor instead of dropping ticks. Step count/order are
        // untouched (SimHash unchanged); only real-time spacing dilates.
        m_accumulator    += elapsed * m_dilation.Factor();
        m_last            = now;
        m_stepsThisFrame  = 0;
    }

    // Feed the real time the last sim tick took (the server measures each
    // SimulateOneTick) so the dilation controller can stretch/recover the clock.
    // Returns the current dilation factor (∈ [floor, 1]) for the §8.5 clock echo.
    double ReportTickCost(double measuredSeconds) noexcept
    {
        return m_dilation.Update(measuredSeconds, kSimDeltaSeconds, m_dilationCfg);
    }

    // Current dilation factor — published in the clock-sync echo (§8.5) so clients
    // interpolate against the dilated authoritative clock, not wall-clock.
    [[nodiscard]] double DilationFactor() const noexcept { return m_dilation.Factor(); }
    [[nodiscard]] bool   IsDilated() const noexcept { return m_dilation.IsDilated(); }
    [[nodiscard]] DilationConfig& DilationCfg() noexcept { return m_dilationCfg; }

    // Returns true and deducts one step's worth of time if a full step is ready
    // AND we have not yet consumed kMaxCatchUpTicksPerFrame steps this frame.
    [[nodiscard]] bool ConsumeStep() noexcept
    {
        if (m_stepsThisFrame >= kMaxCatchUpTicksPerFrame) return false;
        if (m_accumulator < kSimDeltaSeconds)             return false;

        m_accumulator   -= kSimDeltaSeconds;
        ++m_simTickCount;
        ++m_stepsThisFrame;
        return true;
    }

    // Interpolation fraction ∈ [0, 1) for rendering between the last two sim states.
    [[nodiscard]] float GetAlpha() const noexcept
    {
        return static_cast<float>(m_accumulator / kSimDeltaSeconds);
    }

    // Total simulation ticks consumed since Start().
    [[nodiscard]] uint64_t GetSimTickCount() const noexcept { return m_simTickCount; }

    // Steps consumed in the current frame (resets each Tick()).
    [[nodiscard]] int GetStepsThisFrame() const noexcept { return m_stepsThisFrame; }

    // Remaining accumulated time (useful for diagnostics).
    [[nodiscard]] double GetAccumulator() const noexcept { return m_accumulator; }

    // Call after a blocking operation (e.g. loading) to prevent catch-up flood.
    void ResetAccumulator() noexcept
    {
        QueryPerformanceCounter(&m_last);
        m_accumulator    = 0.0;
        m_stepsThisFrame = 0;
    }

private:
    LARGE_INTEGER      m_freq{};
    LARGE_INTEGER      m_last{};
    double             m_accumulator{ 0.0 };
    uint64_t           m_simTickCount{ 0 };
    int                m_stepsThisFrame{ 0 };
    DilationController m_dilation{};     // M4 area H (§7.2)
    DilationConfig     m_dilationCfg{};
};

} // namespace Neuron::Sim
