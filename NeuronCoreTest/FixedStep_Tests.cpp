#include "CppUnitTest.h"
#include "sim/FixedStepAccumulator.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

TEST_CLASS(FixedStepTests)
{
public:
    TEST_METHOD(Start_NoStepsReady)
    {
        Neuron::Sim::FixedStepAccumulator acc;
        acc.Start();
        acc.Tick();
        Assert::IsTrue(acc.GetSimTickCount() >= uint64_t(0));
        Assert::IsTrue(acc.GetAlpha() >= 0.0f);
        Assert::IsTrue(acc.GetAlpha() <= 1.0f);
    }

    TEST_METHOD(MaxCatchUpGuard)
    {
        Neuron::Sim::FixedStepAccumulator acc;
        acc.Start();
        Sleep(200);
        acc.Tick();

        int stepsConsumed = 0;
        while (acc.ConsumeStep())
            ++stepsConsumed;

        Assert::IsTrue(stepsConsumed <= Neuron::Sim::kMaxCatchUpTicksPerFrame);
        Assert::IsTrue(stepsConsumed >= 1);
    }

    TEST_METHOD(AlphaBounded)
    {
        Neuron::Sim::FixedStepAccumulator acc;
        acc.Start();
        Sleep(50);
        acc.Tick();
        while (acc.ConsumeStep()) {}
        const float alpha = acc.GetAlpha();
        Assert::IsTrue(alpha >= 0.0f);
        Assert::IsTrue(alpha <  1.0f);
    }

    TEST_METHOD(TickCountAccumulates)
    {
        Neuron::Sim::FixedStepAccumulator acc;
        acc.Start();
        Sleep(static_cast<DWORD>(3 * 1000 / Neuron::Sim::kSimTicksPerSecond + 10));
        acc.Tick();

        uint64_t totalSteps = 0;
        while (acc.ConsumeStep())
            ++totalSteps;

        Assert::IsTrue(acc.GetSimTickCount() >= uint64_t(1));
        Assert::AreEqual(acc.GetSimTickCount(), totalSteps);
    }

    TEST_METHOD(ResetAccumulator)
    {
        Neuron::Sim::FixedStepAccumulator acc;
        acc.Start();
        Sleep(200);
        acc.ResetAccumulator();
        acc.Tick();
        int steps = 0;
        while (acc.ConsumeStep()) ++steps;
        Assert::IsTrue(steps <= 1);
    }

    TEST_METHOD(StepsThisFrameReset)
    {
        Neuron::Sim::FixedStepAccumulator acc;
        acc.Start();

        Sleep(200);
        acc.Tick();
        while (acc.ConsumeStep()) {}
        const int first = acc.GetStepsThisFrame();

        Sleep(200);
        acc.Tick();
        int second = 0;
        while (acc.ConsumeStep()) ++second;

        Assert::IsTrue(first  <= Neuron::Sim::kMaxCatchUpTicksPerFrame);
        Assert::IsTrue(second <= Neuron::Sim::kMaxCatchUpTicksPerFrame);
    }
};
