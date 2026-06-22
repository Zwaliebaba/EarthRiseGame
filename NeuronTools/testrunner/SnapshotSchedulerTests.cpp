// Priority / quota snapshot scheduler tests (masterplan §8.4; M4 area E). The
// scheduler ranks the entities a client still lacks (area B) by a named priority
// function, applies the R16 visible cap, encodes minimal deltas against the
// per-client acked base (area C), and keeps the MTU-budgeted prefix — the same
// machinery for steady-state spillover and cold-start from the empty (∅) baseline.
// Mirrors the Windows NeuronCoreTest cases on the Linux runner (§16.2).
//
// Component ids are bound once per binary in ShapeCatalogTests.cpp.

#include "ServerUniverse.h"
#include "Snapshot.h"
#include "SnapshotScheduler.h"
#include "TestRunner.h"

#include <algorithm>
#include <vector>

using namespace ertest;
using namespace Neuron::Sim;
using Neuron::Universe::UniversePos;

// --- priority function ordering ---------------------------------------------

ER_TEST(Scheduler, PriorityRanksRelevantAndNearAboveDistantNeutral)
{
    RelevanceWeights w;
    // Own base, right on top of the client.
    SchedCandidate ownBase{ 1, 0.0, Iff::Own, true, false, 0 };
    // The client's current command target, one sector out.
    SchedCandidate target{ 2, 16000.0, Iff::Enemy, false, true, 0 };
    // A distant neutral asteroid, far away.
    SchedCandidate neutralFar{ 3, 320000.0, Iff::Neutral, false, false, 0 };

    const double pBase = SnapshotPriority(ownBase, w);
    const double pTarget = SnapshotPriority(target, w);
    const double pNeutral = SnapshotPriority(neutralFar, w);

    ER_CHECK(pBase > pTarget);     // own base on top outranks a target a sector away
    ER_CHECK(pTarget > pNeutral);  // a near-ish target outranks a distant neutral
}

ER_TEST(Scheduler, StalenessRaisesAnAgedEntity)
{
    RelevanceWeights w;
    SchedCandidate fresh{ 1, 16000.0, Iff::Neutral, false, false, 0 };
    SchedCandidate aged = fresh; aged.netId = 2; aged.staleness = 100;
    ER_CHECK(SnapshotPriority(aged, w) > SnapshotPriority(fresh, w)); // anti-starvation
}

ER_TEST(Scheduler, ScheduleAppliesVisibleCapAndIsDeterministic)
{
    RelevanceWeights w;
    std::vector<SchedCandidate> cands;
    for (uint32_t i = 1; i <= 10; ++i)
        cands.push_back({ i, 1000.0 * i, Iff::Neutral, false, false, 0 }); // nearer = higher

    const ScheduleResult r = ScheduleClient(cands, w, 3);
    ER_CHECK_EQ(r.ordered.size(), size_t{ 3 });
    ER_CHECK_EQ(r.capped, size_t{ 7 });              // 10 − cap(3) shed (R16 evidence)
    ER_CHECK_EQ(r.ordered[0], uint32_t{ 1 });        // nearest first
    ER_CHECK_EQ(r.ordered[1], uint32_t{ 2 });
    ER_CHECK_EQ(r.ordered[2], uint32_t{ 3 });

    // Re-running the same scene yields the same order (deterministic tie-break).
    const ScheduleResult r2 = ScheduleClient(cands, w, 3);
    ER_CHECK(r.ordered == r2.ordered);
}

// --- per-client delta-base cache (ack-advanced) -----------------------------

ER_TEST(Scheduler, KnownStateAdvancesOnlyOnAck)
{
    ClientKnownState known;
    SnapshotEntity e; e.netId = 7; e.hp = 100;
    ER_CHECK(known.Base(7) == nullptr); // cold → first sight

    known.RecordSent(5, { e });
    ER_CHECK(known.Base(7) == nullptr); // sent, not acked → base unchanged (re-delta)

    known.Ack(5);
    ER_CHECK(known.Base(7) != nullptr);
    ER_CHECK_EQ(known.Base(7)->hp, int32_t{ 100 });
    ER_CHECK(known.ApproxBytes() > 0);
}

// --- ServerUniverse end-to-end: cold-start convergence ----------------------

namespace
{
    // Spawn a base and 'n' static scenery props in its sector; returns the base id.
    uint32_t SeedScene(ServerUniverse& su, int n)
    {
        const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
        for (int i = 0; i < n; ++i)
            su.SpawnProp(0, { 100 + 10 * i, 0, 0 }); // all in sector 0, near the base
        return base;
    }

    // Count entities the base can currently see (its interest set).
    size_t VisibleCount(ServerUniverse& su, uint32_t base)
    {
        std::vector<uint32_t> vis;
        su.Interest().VisibleTo(base, vis);
        return vis.size();
    }
}

ER_TEST(Scheduler, ColdStartConvergesUnderTinyBudget)
{
    ServerUniverse su(false);
    const uint32_t base = SeedScene(su, 10);
    su.Step(0.1f); // stamp + interest
    const size_t want = VisibleCount(su, base);
    ER_CHECK(want >= 11); // base + 10 props

    DeltaDecodeState client;
    bool firstTick = true;
    for (int t = 0; t < 60 && client.Size() < want; ++t) {
        size_t capped = 0;
        const DeltaSnapshot snap = su.BuildClientSnapshot(base, /*byteBudget=*/64, &capped);
        if (firstTick) {
            // The very first record a cold client gets is its own base (top priority).
            ER_CHECK(!snap.records.empty());
            ER_CHECK_EQ(snap.records[0].netId, base);
            firstTick = false;
        }
        ER_CHECK(EncodeDeltaSnapshot(snap).size() <= 64); // never exceeds the budget
        client.Apply(EncodeDeltaSnapshot(snap));
        su.RecordClientSnapshotSent(base, snap);
        su.AckClient(base, su.Tick());
        su.Step(0.1f); // world idle (static props) → nothing new bumps
    }
    ER_CHECK_EQ(client.Size(), want); // the whole interest set converged
}

ER_TEST(Scheduler, VisibleCapBindsButStillConverges)
{
    ServerUniverse su(false);
    const uint32_t base = SeedScene(su, 10);
    su.SetVisibleCap(3); // a hard cap well under the scene size
    su.Step(0.1f);
    const size_t want = VisibleCount(su, base);

    DeltaDecodeState client;
    bool sawCapBind = false;
    for (int t = 0; t < 80 && client.Size() < want; ++t) {
        size_t capped = 0;
        const DeltaSnapshot snap = su.BuildClientSnapshot(base, /*byteBudget=*/4096, &capped);
        ER_CHECK(snap.records.size() <= 3); // the cap is never exceeded in one tick
        if (capped > 0) sawCapBind = true;
        client.Apply(EncodeDeltaSnapshot(snap));
        su.RecordClientSnapshotSent(base, snap);
        su.AckClient(base, su.Tick());
        su.Step(0.1f);
    }
    ER_CHECK(sawCapBind);              // the cap bound (M4 R16 evidence)
    ER_CHECK_EQ(client.Size(), want); // yet the scene still fully converges over ticks
}

ER_TEST(Scheduler, BudgetedSnapshotNeverExceedsAndNoneDropped)
{
    ServerUniverse su(false);
    const uint32_t base = SeedScene(su, 20);
    su.Step(0.1f);
    const size_t want = VisibleCount(su, base);

    DeltaDecodeState client;
    for (int t = 0; t < 120 && client.Size() < want; ++t) {
        const DeltaSnapshot snap = su.BuildClientSnapshot(base, /*byteBudget=*/48);
        ER_CHECK(EncodeDeltaSnapshot(snap).size() <= 48);
        client.Apply(EncodeDeltaSnapshot(snap));
        su.RecordClientSnapshotSent(base, snap);
        su.AckClient(base, su.Tick());
        su.Step(0.1f);
    }
    ER_CHECK_EQ(client.Size(), want); // spillover delivered everything; nothing dropped
}
