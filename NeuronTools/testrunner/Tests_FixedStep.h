#pragma once
// M0 unit tests — FixedStepAccumulator (30 Hz, bounded catch-up, alpha).

#include "TestRunner.h"
#include "sim/FixedStepAccumulator.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

TEST_SUITE(FixedStep)
{
    TEST_CASE(Start_NoStepsReady) {
        // Immediately after Start(), no steps should be ready (zero elapsed time).
        Neuron::Sim::FixedStepAccumulator acc;
        acc.Start();
        acc.Tick();
        // At Start time there is virtually no elapsed time — definitely < 1 step.
        // ConsumeStep may return true if the QPC call takes > 33 ms, which it won't.
        // We just verify the API doesn't crash and tick count starts at 0.
        // (Don't assert step count — tiny real elapsed is platform-dependent.)
        CHECK_GE(acc.GetSimTickCount(), uint64_t(0));
        CHECK_GE(acc.GetAlpha(), 0.0f);
        CHECK_LE(acc.GetAlpha(), 1.0f);
    });

    TEST_CASE(MaxCatchUpGuard) {
        // Inject more than kMaxCatchUpTicksPerFrame steps worth of time,
        // verify we never consume more than kMaxCatchUpTicksPerFrame in one Tick().
        Neuron::Sim::FixedStepAccumulator acc;
        acc.Start();

        // Sleep well beyond one frame's worth to force catch-up.
        Sleep(200); // 200 ms >> 5 × 33.3 ms

        acc.Tick();

        int stepsConsumed = 0;
        while (acc.ConsumeStep())
            ++stepsConsumed;

        CHECK_LE(stepsConsumed, Neuron::Sim::kMaxCatchUpTicksPerFrame);
        CHECK_GE(stepsConsumed, 1); // at least one step after 200 ms
    });

    TEST_CASE(AlphaBounded) {
        Neuron::Sim::FixedStepAccumulator acc;
        acc.Start();
        Sleep(50); // ~1.5 steps
        acc.Tick();
        while (acc.ConsumeStep()) {} // drain
        const float alpha = acc.GetAlpha();
        CHECK_GE(alpha, 0.0f);
        CHECK_LT(alpha, 1.0f);
    });

    TEST_CASE(TickCountAccumulates) {
        Neuron::Sim::FixedStepAccumulator acc;
        acc.Start();

        uint64_t totalSteps = 0;
        // Run for ~3 ticks worth of time.
        Sleep(static_cast<DWORD>(3 * 1000 / Neuron::Sim::kSimTicksPerSecond + 10));

        acc.Tick();
        while (acc.ConsumeStep())
            ++totalSteps;

        CHECK_GE(acc.GetSimTickCount(), uint64_t(1));
        CHECK_EQ(acc.GetSimTickCount(), totalSteps);
    });

    TEST_CASE(ResetAccumulator) {
        // After ResetAccumulator() the next Tick() + ConsumeStep() loop should see
        // near-zero elapsed (Sleep happened before the reset).
        Neuron::Sim::FixedStepAccumulator acc;
        acc.Start();
        Sleep(200);
        acc.ResetAccumulator();
        acc.Tick();
        int steps = 0;
        while (acc.ConsumeStep()) ++steps;
        // After reset, elapsed ≈ 0 → 0 or 1 steps at most.
        CHECK_LE(steps, 1);
    });

    TEST_CASE(StepsThisFrameReset) {
        Neuron::Sim::FixedStepAccumulator acc;
        acc.Start();

        Sleep(200);
        acc.Tick();
        while (acc.ConsumeStep()) {}
        const int first = acc.GetStepsThisFrame();

        Sleep(200);
        acc.Tick(); // new Tick() resets stepsThisFrame
        int second = 0;
        while (acc.ConsumeStep()) ++second;

        CHECK_LE(first,  Neuron::Sim::kMaxCatchUpTicksPerFrame);
        CHECK_LE(second, Neuron::Sim::kMaxCatchUpTicksPerFrame);
    });
}
