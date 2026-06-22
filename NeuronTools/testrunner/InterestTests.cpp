// Cell publish/subscribe interest tests (masterplan §8.4, §6.3; M4 area A). The
// InterestGrid replaces the M3 O(players × entities) per-tick fog rebuild with a
// SectorId-keyed grid of cells: entities enter/leave cells on sector crossings and
// players subscribe to the neighbourhood of cells their sensors reach, so a
// mutation in a cell is enqueued once to that cell's subscribers. Mirrors the
// Windows NeuronCoreTest cases on the Linux runner (§16.2).
//
// Component ids are bound once per binary in ShapeCatalogTests.cpp.

#include "Interest.h"
#include "ServerUniverse.h"
#include "TestRunner.h"

#include <vector>

using namespace ertest;
using Neuron::Sim::CollectNeighbourhood;
using Neuron::Sim::InterestGrid;
using Neuron::Sim::SectorRadiusForRange;
using Neuron::Sim::ServerUniverse;
using Neuron::Universe::kSectorSize;
using Neuron::Universe::SectorId;
using Neuron::Universe::UniversePos;
using Neuron::Universe::UniverseToSector;

// --- residency / sector crossings ------------------------------------------

ER_TEST(Interest, FirstResidencyIsEnterOnly)
{
    InterestGrid g;
    const auto ev = g.UpdateResidency(7, { 0, 0, 0 });
    ER_CHECK(ev.changed);
    ER_CHECK(!ev.hadPrevious); // no matching leave on first residency
    ER_CHECK_EQ(g.Residents({ 0, 0, 0 }).size(), size_t{ 1 });
    ER_CHECK_EQ(g.Residents({ 0, 0, 0 })[0], uint32_t{ 7 });
}

ER_TEST(Interest, CrossingEmitsOneLeaveAndOneEnter)
{
    InterestGrid g;
    g.UpdateResidency(7, { 0, 0, 0 });

    // Same sector again → no crossing.
    ER_CHECK(!g.UpdateResidency(7, { 0, 0, 0 }).changed);

    // Cross into a neighbour: exactly one leave (old cell empties) + one enter.
    const auto ev = g.UpdateResidency(7, { 1, 0, 0 });
    ER_CHECK(ev.changed && ev.hadPrevious);
    ER_CHECK((ev.from == SectorId{ 0, 0, 0 }));
    ER_CHECK((ev.to == SectorId{ 1, 0, 0 }));
    ER_CHECK(g.Residents({ 0, 0, 0 }).empty());            // left old
    ER_CHECK_EQ(g.Residents({ 1, 0, 0 }).size(), size_t{ 1 }); // entered new
}

ER_TEST(Interest, RemoveDropsResidency)
{
    InterestGrid g;
    g.UpdateResidency(7, { 2, 3, 4 });
    g.Remove(7);
    ER_CHECK(g.Residents({ 2, 3, 4 }).empty());
    SectorId out;
    ER_CHECK(!g.ResidentCell(7, out));
}

// --- subscriptions match sensor range --------------------------------------

ER_TEST(Interest, NeighbourhoodSubscriptionMatchesRange)
{
    InterestGrid g;
    std::vector<SectorId> cells;
    CollectNeighbourhood({ 0, 0, 0 }, 2, cells); // Chebyshev radius 2
    g.SetSubscription(1, cells);

    // Every cell within the radius is subscribed; the corner is the extreme case.
    ER_CHECK(g.IsSubscribed(1, { 2, 0, 0 }));
    ER_CHECK(g.IsSubscribed(1, { -2, -2, -2 }));
    ER_CHECK(g.IsSubscribed(1, { 0, 0, 0 }));
    // One cell past the radius on any axis is not.
    ER_CHECK(!g.IsSubscribed(1, { 3, 0, 0 }));
    ER_CHECK(!g.IsSubscribed(1, { 0, 0, 3 }));

    // (2r+1)^3 cells subscribed for radius 2.
    ER_CHECK_EQ(g.Subscriptions(1).size(), size_t{ 5 * 5 * 5 });
}

ER_TEST(Interest, SectorRadiusForRangeCeils)
{
    ER_CHECK_EQ(SectorRadiusForRange(0.0f), 0);
    ER_CHECK_EQ(SectorRadiusForRange(static_cast<float>(kSectorSize) * 0.5f), 1);
    ER_CHECK_EQ(SectorRadiusForRange(static_cast<float>(kSectorSize)), 1);
    ER_CHECK_EQ(SectorRadiusForRange(static_cast<float>(kSectorSize) * 2.1f), 3);
}

ER_TEST(Interest, SetSubscriptionDiffsEnterAndLeave)
{
    InterestGrid g;
    std::vector<SectorId> a = { { 0, 0, 0 }, { 1, 0, 0 } };
    g.SetSubscription(1, a);

    // Slide one cell over: { 1,0,0 } stays, { 0,0,0 } leaves, { 2,0,0 } enters.
    std::vector<SectorId> b = { { 1, 0, 0 }, { 2, 0, 0 } };
    const auto delta = g.SetSubscription(1, b);
    ER_CHECK_EQ(delta.entered.size(), size_t{ 1 });
    ER_CHECK((delta.entered[0] == SectorId{ 2, 0, 0 }));
    ER_CHECK_EQ(delta.left.size(), size_t{ 1 });
    ER_CHECK((delta.left[0] == SectorId{ 0, 0, 0 }));
    ER_CHECK(!g.IsSubscribed(1, { 0, 0, 0 }));
    ER_CHECK(g.IsSubscribed(1, { 1, 0, 0 }));
    ER_CHECK(g.IsSubscribed(1, { 2, 0, 0 }));
}

// --- a mutation reaches only its cell's subscribers -------------------------

ER_TEST(Interest, MutationEnqueuedToCellSubscribersOnly)
{
    InterestGrid g;
    const SectorId cellC{ 5, 0, 0 };
    g.UpdateResidency(100, cellC); // entity 100 lives in cell C

    g.Subscribe(1, cellC);          // player 1 watches C
    g.Subscribe(2, { 9, 0, 0 });    // player 2 watches elsewhere

    // C's subscriber list is exactly { 1 } — never the non-subscriber.
    ER_CHECK_EQ(g.Subscribers(cellC).size(), size_t{ 1 });
    ER_CHECK_EQ(g.Subscribers(cellC)[0], uint32_t{ 1 });

    std::vector<uint32_t> vis;
    g.VisibleTo(1, vis);
    ER_CHECK_EQ(vis.size(), size_t{ 1 });
    ER_CHECK_EQ(vis[0], uint32_t{ 100 });
    g.VisibleTo(2, vis);
    ER_CHECK(vis.empty()); // the non-subscriber never sees the mutation
}

ER_TEST(Interest, RemovePlayerClearsSubscriptions)
{
    InterestGrid g;
    g.Subscribe(1, { 0, 0, 0 });
    g.Subscribe(1, { 1, 0, 0 });
    g.RemovePlayer(1);
    ER_CHECK(g.Subscriptions(1).empty());
    ER_CHECK(g.Subscribers({ 0, 0, 0 }).empty());
    ER_CHECK(g.Subscribers({ 1, 0, 0 }).empty());
}

// --- ServerUniverse integration --------------------------------------------

ER_TEST(Interest, ServerUniverseSubscribesOwnSectorAfterStep)
{
    ServerUniverse su(false);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    su.Step(0.1f); // Step refreshes the interest grid (UpdateInterest)

    // The player covers its own sector and not a sector far beyond sensor range.
    ER_CHECK(su.Interest().IsSubscribed(base, UniverseToSector({ 0, 0, 0 })));
    ER_CHECK(!su.Interest().IsSubscribed(base, SectorId{ 100, 0, 0 }));
}

ER_TEST(Interest, WarpPrefetchSubscribesDestinationBeforeArrival)
{
    ServerUniverse su(false);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const UniversePos dest{ int64_t(40) * kSectorSize, 0, 0 }; // far across sectors
    const SectorId destSec = UniverseToSector(dest);

    ER_CHECK(su.BeginWarpTo(base, dest));            // R21 prefetch fires here
    ER_CHECK(su.Interest().IsSubscribed(base, destSec)); // subscribed before arrival

    // The prefetch is pinned: it survives the per-tick subscription refresh while
    // the base is still in flight (its own neighbourhood doesn't cover destSec yet).
    su.Step(0.1f);
    ER_CHECK(su.Interest().IsSubscribed(base, destSec));
}
