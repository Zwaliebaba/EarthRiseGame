// Telemetry / counter tests (masterplan §21; M4 area I). The load gate (area J)
// reads sim p50/p99, encode p99, per-client downstream, baseline RAM, cold-start
// convergence, and the §21 net counters — so the aggregation math must be correct
// before the run. Pure; mirrored on the Linux runner (§16.2). The sampling sites
// live in the Win32 server/harness.

#include "Telemetry.h"
#include "TestRunner.h"

using namespace ertest;
using Neuron::Sim::NetCounters;
using Neuron::Sim::PercentileWindow;
using Neuron::Sim::ServerTelemetry;

ER_TEST(Telemetry, PercentileNearestRankOverKnownSample)
{
    PercentileWindow w(1000);
    for (int i = 1; i <= 100; ++i) w.Add(static_cast<double>(i)); // 1..100

    ER_CHECK_EQ(w.Count(), size_t{ 100 });
    ER_CHECK_EQ(w.Percentile(0.50), 50.0); // ceil(0.50*100)=50 → 50th value
    ER_CHECK_EQ(w.Percentile(0.99), 99.0); // ceil(0.99*100)=99 → 99th value
    ER_CHECK_EQ(w.Percentile(1.0), 100.0); // max
    ER_CHECK_EQ(w.Percentile(0.0), 1.0);   // min (rank clamped to 1)
    ER_CHECK_EQ(w.Max(), 100.0);
    ER_CHECK(w.Mean() > 50.0 && w.Mean() < 51.0); // 50.5
}

ER_TEST(Telemetry, PercentileIgnoresInsertionOrder)
{
    PercentileWindow a, b;
    for (int i = 1; i <= 50; ++i) a.Add(i);
    for (int i = 50; i >= 1; --i) b.Add(i); // reverse order, same multiset
    ER_CHECK_EQ(a.Percentile(0.99), b.Percentile(0.99));
    ER_CHECK_EQ(a.Percentile(0.50), b.Percentile(0.50));
}

ER_TEST(Telemetry, WindowIsBoundedAndEvictsOldest)
{
    PercentileWindow w(8);
    for (int i = 0; i < 100; ++i) w.Add(static_cast<double>(i)); // only the last 8 survive
    ER_CHECK_EQ(w.Count(), size_t{ 8 });
    ER_CHECK_EQ(w.Max(), 99.0);            // newest retained
    ER_CHECK_EQ(w.Percentile(0.0), 92.0);  // oldest surviving sample is 92
}

ER_TEST(Telemetry, EmptyWindowIsZero)
{
    PercentileWindow w;
    ER_CHECK(w.Empty());
    ER_CHECK_EQ(w.Percentile(0.99), 0.0);
    ER_CHECK_EQ(w.Mean(), 0.0);
}

ER_TEST(Telemetry, NetCountersSumBytesAndDatagrams)
{
    NetCounters c;
    c.AddDown(100);
    c.AddDown(250);
    c.AddUp(40);
    ER_CHECK_EQ(c.downstreamBytes, uint64_t{ 350 });
    ER_CHECK_EQ(c.datagramsOut, uint64_t{ 2 });
    ER_CHECK_EQ(c.upstreamBytes, uint64_t{ 40 });
    ER_CHECK_EQ(c.datagramsIn, uint64_t{ 1 });
}

ER_TEST(Telemetry, ServerTelemetryAggregatesTheGates)
{
    ServerTelemetry t;
    // Sim ticks: 99 cheap, one spike — p99 catches the spike, p50 does not.
    for (int i = 0; i < 99; ++i) t.RecordTickMs(10.0);
    t.RecordTickMs(40.0);
    ER_CHECK_EQ(t.SimP50(), 10.0);
    ER_CHECK_EQ(t.SimP99(), 10.0); // ceil(.99*100)=99 → still a 10 ms sample
    t.RecordTickMs(50.0);          // 101 samples; now the tail is heavier
    ER_CHECK(t.SimP99() >= 40.0);

    t.RecordClientDownstream(1200);
    t.RecordClientDownstream(800);
    ER_CHECK(t.ClientDownstreamMax() >= 1200.0);

    t.RecordBaselineBytes(4096);
    ER_CHECK_EQ(t.BaselineBytes(), uint64_t{ 4096 });

    t.RecordColdStartTicks(12);
    t.RecordColdStartTicks(30);
    ER_CHECK(t.ColdStartMaxTicks() >= 30.0);

    t.RecordCapBind(0);
    t.RecordCapBind(7);
    ER_CHECK_EQ(t.MaxCapBind(), size_t{ 7 });
    ER_CHECK_EQ(t.CapBindTicks(), uint64_t{ 1 }); // only the non-zero tick counts

    t.Net().AddDown(500);
    ER_CHECK_EQ(t.Net().downstreamBytes, uint64_t{ 500 });
}
