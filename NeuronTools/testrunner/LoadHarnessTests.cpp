// Contested-sector scale harness (masterplan §17 M4 Done, §10.3, App. B; M4 area J).
// The Windows ERHeadless run drives hundreds of real UDP sessions and gates on
// wall-clock sim p99; this Linux mirror (§16.2) drives the *platform-independent
// replication pipeline* (areas A–I) at scale through ServerUniverse directly and
// gates on the parts that don't need a Windows clock: the per-client bandwidth
// budget, baseline RAM, interest culling (contested vs dispersed, R23), the R16
// visible cap, eviction/cold-start convergence, and determinism.
//
// Component ids are bound once per binary in ShapeCatalogTests.cpp.

#include "ServerUniverse.h"
#include "Snapshot.h"
#include "Telemetry.h"
#include "TestRunner.h"

#include <vector>

using namespace ertest;
using namespace Neuron::Sim;
using Neuron::Universe::SECTOR_SIZE;
using Neuron::Universe::UniversePos;

namespace
{
    constexpr size_t SAFE_MTU = 1100; // safe per-snapshot byte budget (§8.2)

    struct ScaleResult
    {
        size_t   clients{ 0 };
        int      ticksToConverge{ -1 };   // -1 = did not converge in the window
        size_t   peakClientDownstream{ 0 };
        uint64_t steadyStateDownstream{ 0 }; // total bytes the tick after convergence
        size_t   baselineBytes{ 0 };
        size_t   maxCapBind{ 0 };
    };

    // Drive 'clients' through the full per-tick pipeline (select → rank → cap →
    // encode → budget → send → ack) until every client's decode state matches its
    // interest set, or maxTicks elapse. Records the App. B-relevant metrics.
    ScaleResult RunScenario(ServerUniverse& su, const std::vector<uint32_t>& clients,
                            size_t cap, int maxTicks)
    {
        // This harness stresses the replication pipeline with many mutually-hostile
        // bases packed into one sector; combat (M6) would make them fight and never go
        // idle, so disable it — the contested *replication* case is what's under test.
        su.SetCombatEnabled(false);
        su.SetVisibleCap(cap);
        su.Step(0.1f); // stamp + interest before the run (counters wired first, §17)

        std::vector<DeltaDecodeState> decoders(clients.size());
        std::vector<size_t> want(clients.size());
        for (size_t i = 0; i < clients.size(); ++i) {
            std::vector<uint32_t> vis; su.Interest().VisibleTo(clients[i], vis);
            want[i] = vis.size();
        }

        ScaleResult r; r.clients = clients.size();
        ServerTelemetry tel;
        for (int t = 0; t < maxTicks; ++t) {
            bool allConverged = true;
            uint64_t tickDownstream = 0; // bytes from record-bearing snapshots only
            for (size_t i = 0; i < clients.size(); ++i) {
                size_t capped = 0;
                const DeltaSnapshot snap = su.BuildClientSnapshot(clients[i], SAFE_MTU, &capped);
                const auto bytes = EncodeDeltaSnapshot(snap);
                ER_CHECK(bytes.size() <= SAFE_MTU); // the budget invariant, every client every tick
                tel.RecordCapBind(capped);
                if (!snap.records.empty()) { // a real server skips empty snapshots
                    if (bytes.size() > r.peakClientDownstream) r.peakClientDownstream = bytes.size();
                    tickDownstream += bytes.size();
                    tel.RecordClientDownstream(bytes.size());
                    decoders[i].Apply(bytes);
                    su.RecordClientSnapshotSent(clients[i], snap);
                    su.AckClient(clients[i], su.Tick());
                }
                if (decoders[i].Size() < want[i]) allConverged = false;
            }
            if (r.ticksToConverge >= 0) {           // the idle tick after convergence
                r.steadyStateDownstream = tickDownstream;
                break;
            }
            if (allConverged) r.ticksToConverge = t + 1;
            su.Step(0.1f); // world idle (static bases) → nothing new bumps
        }
        r.baselineBytes = su.TotalClientBaselineBytes();
        r.maxCapBind = tel.MaxCapBind();
        return r;
    }

    // 'n' bases all packed into a single sector (mutual interest — the worst case
    // the dispersed run hides, R23).
    std::vector<uint32_t> Contested(ServerUniverse& su, int n)
    {
        std::vector<uint32_t> c;
        for (int i = 0; i < n; ++i)
            c.push_back(su.SpawnBase({ 200 + 5 * i, 0, 0 }, { 0, 0, 0 })); // all in sector {0,0,0}
        return c;
    }

    // 'n' bases spread far apart (≥ several sectors) so interest culls — each base
    // sees essentially only itself.
    std::vector<uint32_t> Dispersed(ServerUniverse& su, int n)
    {
        std::vector<uint32_t> c;
        for (int i = 0; i < n; ++i)
            c.push_back(su.SpawnBase({ static_cast<int64_t>(i) * SECTOR_SIZE * 6, 0, 0 }, { 0, 0, 0 }));
        return c;
    }
}

// The primary gate: a contested single sector holds the per-client byte budget every
// tick (never exceeds the safe MTU), and the whole interest set still converges —
// nothing dropped, spillover delivered over ticks.
ER_TEST(LoadHarness, ContestedSectorHoldsBandwidthAndConverges)
{
    ServerUniverse su(false);
    const auto clients = Contested(su, 120);
    const ScaleResult r = RunScenario(su, clients, /*cap=*/1024, /*maxTicks=*/60);

    ER_CHECK_EQ(r.clients, size_t{ 120 });
    ER_CHECK(r.ticksToConverge > 0);                 // converged within the window
    ER_CHECK(r.peakClientDownstream <= SAFE_MTU);    // never over the MTU budget
    ER_CHECK_EQ(r.steadyStateDownstream, uint64_t{ 0 }); // idle costs nothing (stationary)
}

// The dispersed control: same player count, spread out — interest culling makes it
// far cheaper to converge, proving the pileup is the binding case (R23).
ER_TEST(LoadHarness, DispersedControlIsFarCheaperThanContested)
{
    ServerUniverse suC(false);
    const ScaleResult contested = RunScenario(suC, Contested(suC, 120), 1024, 60);

    ServerUniverse suD(false);
    const ScaleResult dispersed = RunScenario(suD, Dispersed(suD, 120), 1024, 60);

    ER_CHECK(dispersed.ticksToConverge > 0);
    // Dispersed converges at least as fast and holds a far smaller baseline than the
    // contested pileup (each client tracks ~1 entity vs ~120).
    ER_CHECK(dispersed.ticksToConverge <= contested.ticksToConverge);
    ER_CHECK(dispersed.baselineBytes * 4 < contested.baselineBytes);
}

// Per-client baseline RAM stays bounded and proportional to what each client tracks
// (App. B gauge), even with everyone piled into one sector.
ER_TEST(LoadHarness, BaselineRamBoundedAtScale)
{
    ServerUniverse su(false);
    const auto clients = Contested(su, 100);
    const ScaleResult r = RunScenario(su, clients, 1024, 60);
    ER_CHECK(r.baselineBytes > 0);
    // 100 clients each tracking ~100 entities. The gate here is "measured + bounded"
    // — RAM grows linearly with (clients × tracked entities), not unbounded — at
    // ~65 B per tracked entity (a full SnapshotEntity delta-base + its acked version).
    // The tight App. B per-client byte budget is evaluated on the Windows agent.
    ER_CHECK(r.baselineBytes < size_t{ 100 } * size_t{ 100 } * size_t{ 160 });
}

// The R16 visible cap binds under the pileup (the evidence that aggregation/LOD is
// mandatory at M7), yet the scene still fully converges over ticks via staleness.
ER_TEST(LoadHarness, VisibleCapBindsUnderPileupButConverges)
{
    ServerUniverse su(false);
    const auto clients = Contested(su, 120);
    const ScaleResult r = RunScenario(su, clients, /*cap=*/32, /*maxTicks=*/120);
    ER_CHECK(r.maxCapBind > 0);        // the cap bound (R16 evidence)
    ER_CHECK(r.ticksToConverge > 0);   // and the scene still converged
}

// Determinism: the contested pipeline does not perturb sim state — two runs of the
// same scenario produce identical SimHash (the replication layer is a pure read).
ER_TEST(LoadHarness, ContestedPipelineIsDeterministic)
{
    ServerUniverse a(false); const auto ca = Contested(a, 80); RunScenario(a, ca, 1024, 60);
    ServerUniverse b(false); const auto cb = Contested(b, 80); RunScenario(b, cb, 1024, 60);
    ER_CHECK_EQ(a.SimHash(), b.SimHash());
}
