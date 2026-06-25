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

#include <algorithm>
#include <vector>

using namespace ertest;
using Neuron::Sim::ClientBaseline;
using Neuron::Sim::CollectNeighbourhood;
using Neuron::Sim::InterestGrid;
using Neuron::Sim::ReplicationStamps;
using Neuron::Sim::SectorRadiusForRange;
using Neuron::Sim::ServerUniverse;
using Neuron::Universe::SECTOR_SIZE;
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
    ER_CHECK_EQ(SectorRadiusForRange(static_cast<float>(SECTOR_SIZE) * 0.5f), 1);
    ER_CHECK_EQ(SectorRadiusForRange(static_cast<float>(SECTOR_SIZE)), 1);
    ER_CHECK_EQ(SectorRadiusForRange(static_cast<float>(SECTOR_SIZE) * 2.1f), 3);
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
    const UniversePos dest{ int64_t(40) * SECTOR_SIZE, 0, 0 }; // far across sectors
    const SectorId destSec = UniverseToSector(dest);

    ER_CHECK(su.BeginWarpTo(base, dest));            // R21 prefetch fires here
    ER_CHECK(su.Interest().IsSubscribed(base, destSec)); // subscribed before arrival

    // The prefetch is pinned: it survives the per-tick subscription refresh while
    // the base is still in flight (its own neighbourhood doesn't cover destSec yet).
    su.Step(0.1f);
    ER_CHECK(su.Interest().IsSubscribed(base, destSec));
}

// --- per-entity version stamping (M4 area B, §8.4) --------------------------

ER_TEST(Replication, VersionBumpsOnlyWhenAReplicatedFieldChanges)
{
    ServerUniverse su(false);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });

    su.Step(0.1f); // first stamp
    const uint32_t v1 = su.ReplVersion(base);
    ER_CHECK(v1 >= 1);

    su.Step(0.1f); // idle → no replicated change
    su.Step(0.1f);
    ER_CHECK_EQ(su.ReplVersion(base), v1); // version held across idle ticks

    // The base relocates by a Move order now (the legacy SetBaseVelocity path is retired).
    Neuron::Sim::FleetCommand mv;
    mv.intent      = Neuron::Sim::IntentType::Move;
    mv.units       = { base };
    mv.targetPoint = { 1'000'000, 0, 0 }; // far point → the base keeps steering toward it
    su.ApplyFleetCommand(base, mv);       // player ≈ base net id, so it owns itself
    su.Step(0.1f);
    ER_CHECK(su.ReplVersion(base) > v1); // position changed → version advanced
}

ER_TEST(Replication, StampsAreServerOnlyAndIdleEntitiesAreFree)
{
    // A new entity stamps to version 1 (changed vs nothing); HP loss bumps it.
    ReplicationStamps stamps;
    Neuron::Sim::ReplFields f;
    f.hp = 100;
    ER_CHECK_EQ(stamps.Stamp(42, f), uint32_t{ 1 });
    ER_CHECK_EQ(stamps.Stamp(42, f), uint32_t{ 1 }); // unchanged → no bump
    f.hp = 90;
    ER_CHECK_EQ(stamps.Stamp(42, f), uint32_t{ 2 }); // changed field → bump
    stamps.Remove(42);
    ER_CHECK_EQ(stamps.Version(42), uint32_t{ 0 });
}

// --- per-client baseline diff + ack (M4 area B, §8.4 / §8.3) ----------------

ER_TEST(Replication, BaselineDiffSelectsChangedAndAckShrinksToEmpty)
{
    ClientBaseline base;
    ER_CHECK(base.Needs(10, 3)); // unacked entity is needed

    // Send a snapshot (tick 5) carrying entity 10 @ v3, but do not ack it yet.
    base.RecordSent(5, { { 10, 3 } });
    ER_CHECK(base.Needs(10, 3)); // still needed — diff is from acked, not last-sent

    base.Ack(5);                  // ack advances the baseline to v3
    ER_CHECK(!base.Needs(10, 3)); // v3 no longer needed (re-acked → ∅)
    ER_CHECK(base.Needs(10, 4));  // a later change (v4) is needed again
    ER_CHECK_EQ(base.Acked(10), uint32_t{ 3 });
}

ER_TEST(Replication, DroppedSnapshotReDeltasFromAckedBaseline)
{
    ServerUniverse su(false);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });

    su.Step(0.1f); // stamp + interest; the base is visible to itself
    const auto changed1 = su.ChangedFor(base);
    ER_CHECK(std::find(changed1.begin(), changed1.end(), base) != changed1.end());

    // Simulate the snapshot being SENT but the ack LOST.
    su.RecordSent(base, su.Tick(), changed1);
    su.Step(0.1f); // idle
    const auto changed2 = su.ChangedFor(base);
    ER_CHECK(std::find(changed2.begin(), changed2.end(), base) != changed2.end()); // re-delta

    // Now the client acks → baseline advances → the unchanged base drops out.
    su.RecordSent(base, su.Tick(), changed2);
    su.AckBaseline(base, su.Tick());
    su.Step(0.1f); // idle
    const auto changed3 = su.ChangedFor(base);
    ER_CHECK(std::find(changed3.begin(), changed3.end(), base) == changed3.end());
}

ER_TEST(Replication, BaselineRamAccountsForAckedEntities)
{
    ClientBaseline base;
    ER_CHECK_EQ(base.ApproxBytes(), size_t{ 0 });
    base.RecordSent(1, { { 10, 1 }, { 11, 1 }, { 12, 1 } });
    base.Ack(1);
    ER_CHECK_EQ(base.AckedCount(), size_t{ 3 });
    ER_CHECK(base.ApproxBytes() > 0); // App. B baseline-RAM gauge is non-empty
}

// --- tombstone eviction (M4 area D, §8.4 / App. A) --------------------------

ER_TEST(Tombstone, ReEmitsUntilAckedThenClears)
{
    ClientBaseline base;
    base.RecordSent(1, { { 10, 1 } });
    base.Ack(1); // client holds entity 10
    ER_CHECK_EQ(base.AckedCount(), size_t{ 1 });

    // Entity 10 leaves interest → tombstone; it drops from the acked baseline.
    base.Tombstone(10);
    ER_CHECK(base.IsTombstoned(10));
    ER_CHECK_EQ(base.AckedCount(), size_t{ 0 });

    // The leave record rides snapshot tick 5 — but it is DROPPED (no ack). It must
    // still be pending and re-emitted next tick (no ghost).
    std::vector<uint32_t> tombs;
    base.CollectTombstones(tombs);
    ER_CHECK_EQ(tombs.size(), size_t{ 1 });
    ER_CHECK_EQ(tombs[0], uint32_t{ 10 });
    base.RecordTombstonesSent(5, tombs);
    ER_CHECK(base.IsTombstoned(10)); // still pending — snapshot 5 not acked

    // Re-emitted on tick 6; the client acks 6 → the tombstone clears for good.
    tombs.clear();
    base.CollectTombstones(tombs);
    base.RecordTombstonesSent(6, tombs); // earliest carrying tick (5) is kept
    base.Ack(6);
    ER_CHECK(!base.IsTombstoned(10));
    ER_CHECK_EQ(base.TombstoneCount(), size_t{ 0 });
}

ER_TEST(Tombstone, StaleAckBeforeCarryingTickDoesNotClear)
{
    ClientBaseline base;
    base.Tombstone(10);
    base.RecordTombstonesSent(5, { 10 }); // first carried on tick 5
    base.Ack(4);                          // an older ack (tick 4 predates the leave)
    ER_CHECK(base.IsTombstoned(10));      // not cleared — tick 4 never carried it
    base.Ack(5);
    ER_CHECK(!base.IsTombstoned(10));     // ack of the carrying tick clears it
}

ER_TEST(Tombstone, ReEnteredEntityIsUntombstoned)
{
    ClientBaseline base;
    base.Tombstone(10);
    ER_CHECK(base.IsTombstoned(10));
    base.Untombstone(10); // came back into interest before the removal acked
    ER_CHECK(!base.IsTombstoned(10));
}

ER_TEST(Tombstone, ServerUniverseEvictsEntityThatLeftInterest)
{
    ServerUniverse su(false);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    // A scenery prop in the base's own sector, visible to it.
    const uint32_t prop = su.SpawnProp(0, { 100, 0, 0 });
    su.Step(0.1f);

    // The base sees the prop; record + ack so its baseline holds it.
    auto changed = su.ChangedFor(base);
    ER_CHECK(std::find(changed.begin(), changed.end(), prop) != changed.end());
    su.RecordSent(base, su.Tick(), changed);
    su.AckBaseline(base, su.Tick());
    ER_CHECK(su.TombstonesFor(base).empty()); // nothing has left yet

    // Destroy the prop: it leaves the grid, so it leaves the base's interest set.
    ER_CHECK(su.DespawnBase(prop)); // removes any netId from world+interest+repl
    su.Step(0.1f);

    // The base is now told the prop despawned (tombstone), and it keeps being told
    // until the ack — a dropped leave never leaves a ghost.
    const auto tombs1 = su.TombstonesFor(base);
    ER_CHECK(std::find(tombs1.begin(), tombs1.end(), prop) != tombs1.end());
    su.RecordTombstonesSent(base, su.Tick(), tombs1); // snapshot SENT but ack LOST
    su.Step(0.1f);
    const auto tombs2 = su.TombstonesFor(base);
    ER_CHECK(std::find(tombs2.begin(), tombs2.end(), prop) != tombs2.end()); // re-emit

    // Ack clears it; the prop never re-appears as a tombstone.
    su.RecordTombstonesSent(base, su.Tick(), tombs2);
    su.AckBaseline(base, su.Tick());
    su.Step(0.1f);
    ER_CHECK(su.TombstonesFor(base).empty());
}
