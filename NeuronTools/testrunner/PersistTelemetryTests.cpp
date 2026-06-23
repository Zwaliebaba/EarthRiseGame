// Persistence + auth telemetry tests (M5 area H; §21). The durability boundary and
// login health must be measurable so the restart/zero-loss drill (area I) is
// automated. Pure aggregation; mirrored on the Linux runner (§16.2). Sampling sites
// + export are the Win32 ERServer/ERHeadless side.

#include "PersistTelemetry.h"
#include "TestRunner.h"

using namespace ertest;
using Neuron::Sim::PersistTelemetry;

ER_TEST(PersistTelemetry, OutboxGaugesAndDrainPercentile)
{
    PersistTelemetry t;
    t.RecordOutboxDepth(12);
    ER_CHECK_EQ(t.OutboxDepth(), uint64_t{ 12 });
    for (double ms : { 1.0, 2.0, 3.0, 4.0, 100.0 }) t.RecordOutboxDrainMs(ms);
    ER_CHECK(t.OutboxDrainP99() >= 100.0); // the slow drain shows up at p99
}

ER_TEST(PersistTelemetry, RpoWatermarkAdvancesMonotonically)
{
    PersistTelemetry t;
    t.AdvanceRpoWatermark(1000);
    t.AdvanceRpoWatermark(3000);
    t.AdvanceRpoWatermark(2500);          // a stale sample must not move it backwards
    ER_CHECK_EQ(t.RpoWatermarkMs(), uint64_t{ 3000 });

    t.RecordWriteBehindBatch(40);
    t.RecordWriteBehindBatch(60);
    ER_CHECK_EQ(t.WriteBehindBatchRows(), uint64_t{ 60 });
    ER_CHECK_EQ(t.WriteBehindBatches(), uint64_t{ 2 });
}

ER_TEST(PersistTelemetry, AuthCountersIncrement)
{
    PersistTelemetry t;
    t.RecordLoginAttempt(); t.RecordLoginAttempt(); t.RecordLoginAttempt();
    t.RecordLoginFailure();
    t.RecordLockout();
    t.RecordRateLimitHit(); t.RecordRateLimitHit();
    ER_CHECK_EQ(t.LoginAttempts(), uint64_t{ 3 });
    ER_CHECK_EQ(t.LoginFailures(), uint64_t{ 1 });
    ER_CHECK_EQ(t.Lockouts(), uint64_t{ 1 });
    ER_CHECK_EQ(t.RateLimitHits(), uint64_t{ 2 });
}
