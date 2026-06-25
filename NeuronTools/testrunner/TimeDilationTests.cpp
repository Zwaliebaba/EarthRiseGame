// Time-dilation policy tests (masterplan §7.2 / §9; M4 area H). When a tick
// overruns its real-time budget the shard slows in-game time toward a floor
// (EVE-style TiDi) instead of dropping ticks (R23); recovery eases back to full
// speed. The pure DilationController is mirrored here on the Linux runner (§16.2);
// the Win32 FixedStepAccumulator scales its elapsed intake by Factor().

#include "TimeDilation.h"
#include "TestRunner.h"

using namespace ertest;
using Neuron::Sim::DilationConfig;
using Neuron::Sim::DilationController;

namespace
{
    constexpr double BUDGET = 1.0 / 30.0; // the 30 Hz real-time step (SIM_DELTA_SECONDS)

    // Drive 'ctrl' with a constant per-tick cost for 'n' updates.
    double Settle(DilationController& ctrl, const DilationConfig& cfg, double cost, int n)
    {
        double f = ctrl.Factor();
        for (int i = 0; i < n; ++i) f = ctrl.Update(cost, BUDGET, cfg);
        return f;
    }
}

ER_TEST(TimeDilation, FullSpeedWhenUnderBudget)
{
    DilationController ctrl;
    DilationConfig cfg;
    const double f = Settle(ctrl, cfg, BUDGET * 0.5, 50); // tick costs half its budget
    ER_CHECK(f >= 0.999); // never dilates when the sim keeps up
    ER_CHECK(!ctrl.IsDilated());
}

ER_TEST(TimeDilation, DilatesTowardOnsetOverLoadWhenOverrunning)
{
    DilationController ctrl;
    DilationConfig cfg;
    // Tick costs 3× its budget → the dilated budget must match cost, so the factor
    // settles near onset/load = 1/3 (well above the floor).
    const double f = Settle(ctrl, cfg, BUDGET * 3.0, 200);
    ER_CHECK(f > 0.30 && f < 0.37);
    ER_CHECK(ctrl.IsDilated());
}

ER_TEST(TimeDilation, ClampsAtTheFloorUnderExtremeOverload)
{
    DilationController ctrl;
    DilationConfig cfg; // floor 0.10
    const double f = Settle(ctrl, cfg, BUDGET * 50.0, 300); // 50× over → target below floor
    ER_CHECK(f >= cfg.floor - 1e-9);
    ER_CHECK(f <= cfg.floor + 1e-6); // pinned at the floor, never lower
}

ER_TEST(TimeDilation, RecoversToFullSpeedWhenLoadDrops)
{
    DilationController ctrl;
    DilationConfig cfg;
    Settle(ctrl, cfg, BUDGET * 4.0, 200); // drive it down under overload
    ER_CHECK(ctrl.IsDilated());
    const double f = Settle(ctrl, cfg, BUDGET * 0.2, 500); // load drops → ease back up
    ER_CHECK(f >= 0.999);
    ER_CHECK(!ctrl.IsDilated());
}

ER_TEST(TimeDilation, FactorStaysWithinBounds)
{
    DilationController ctrl;
    DilationConfig cfg;
    // A noisy load that swings above and below budget never leaves [floor, 1].
    double f = 1.0;
    for (int i = 0; i < 500; ++i) {
        const double cost = (i % 2 == 0) ? BUDGET * 8.0 : BUDGET * 0.1;
        f = ctrl.Update(cost, BUDGET, cfg);
        ER_CHECK(f >= cfg.floor - 1e-9 && f <= 1.0 + 1e-9);
    }
}

ER_TEST(TimeDilation, OnsetIsFasterThanRecovery)
{
    // Asymmetric easing: shed load quickly, recover slowly (avoids oscillation).
    DilationController down;
    DilationConfig cfg;
    const double afterOneOverrun = down.Update(BUDGET * 4.0, BUDGET, cfg);

    DilationController up;
    // Put 'up' near the same dilated factor, then give it one under-budget tick.
    Settle(up, cfg, BUDGET * 4.0, 200);
    const double before = up.Factor();
    const double afterOneRecover = up.Update(BUDGET * 0.1, BUDGET, cfg);

    const double onsetStep   = 1.0 - afterOneOverrun;      // moved down from 1.0
    const double recoverStep  = afterOneRecover - before;   // moved up from 'before'
    ER_CHECK(onsetStep > recoverStep); // one overrun dilates more than one calm tick recovers
}
