// ServerUniverse <-> PersistState capture/restore tests (M5 area F; §15, §9, §26).
//
// The portable glue between the authoritative ECS and the warm-restart blob: a
// restart loads the latest binary snapshot and replays the economy log onto it for a
// clean, verifiable state (warm-restart correctness is an uptime SLA, §26). Here we
// exercise the full path the SimSnapshotStore drives on Windows —
//   Capture -> EncodeState -> DecodeState -> RestoreState
// — and assert it is faithful two ways:
//   1. StateHash(Capture(a)) == StateHash(Capture(restored))  (the PersistState mirror
//      round-trips through the blob and the ECS rebuild), and
//   2. ServerUniverse::SimHash matches for the restored economy/ownership state — the
//      §17 M5 "SimHash matches for the economy/ownership subset" check.
// Pure / Linux-buildable (the ODBC blob read/write is the Win32 side, SimSnapshotStore).
//
// Component ids are bound once per binary in ShapeCatalogTests.cpp (the testrunner TU).

#include "ServerUniverse.h"
#include "WarmRestart.h"
#include "TestRunner.h"

#include <vector>

using namespace ertest;
using namespace Neuron::Sim;
using Neuron::Universe::UniversePos;
using Neuron::Persist::PersistState;
using Neuron::Persist::StateHash;
using Neuron::Persist::EncodeState;
using Neuron::Persist::DecodeState;

namespace
{
    // A pristine universe (no demo seed / scenery) populated with a controlled mix of
    // bases, ships, a build, and an NPC site so the capture covers every PersistState
    // category. Ship fuel is left at the nav default (shipFuelMax) so the FULL SimHash
    // — which hashes fuel — round-trips (RestoreState seeds ship fuel to max, since
    // ship fuel is bounded-RPO write-behind state, not in the blob, §15).
    struct Seeded { uint32_t base0, base1, harv; };

    Seeded SeedScene(ServerUniverse& su)
    {
        Seeded s{};
        // Two owned bases + a harvester + an NPC site.
        s.base0 = su.SpawnBase({ 16184, 0, -50 }, { 0, 0, 0 });
        s.base1 = su.SpawnBase({ -32000, 2000, 900 }, { 0, 0, 0 });
        s.harv  = su.SpawnFleetShip(s.base0, ServerUniverse::ShipShapeId(), { 16404, 0, -50 });
        su.SpawnNpcSite({ 0, 0, 16000 }, 3, 1500.0f); // transient state, blob-only

        // Stamp replication + interest once (mirrors a live tick) BEFORE mutating, so
        // the BuildSystem doesn't consume the storage/progress we pin below.
        su.Step(0.1f);

        // Now pin the authoritative state away from spawn defaults so the capture has
        // something non-trivial to round-trip.
        if (auto* h = su.HealthOf(s.base0)) h->hp = 640;          // dent the hull
        if (auto* st = su.StorageOf(s.base0)) {
            st->amount[0] = 1234.5f; st->amount[1] = 50.0f; st->amount[2] = 9.25f;
        }
        if (auto* f = su.FuelOf(s.base0)) f->current = 175.0f;    // burn some fuel
        su.EnqueueBuild(s.base0);                                  // active build queue
        // paid=true so the captured build matches RestoreState (which restores active
        // builds as paid); otherwise the round-trip SimHash would differ by the paid
        // flag. A persisted active build is realistically already paid (§13.4).
        if (auto* q = su.BuildQueueOf(s.base0)) { q->paid = true; q->progress = 0.33f; }

        if (auto* c = su.CargoOf(s.harv)) { c->amount[0] = 60.0f; c->amount[2] = 5.0f; }
        if (auto* h = su.HealthOf(s.harv)) h->hp = 470;
        return s;
    }
}

ER_TEST(WarmRestartCapture, CaptureCoversEveryCategory)
{
    ServerUniverse su(false);
    SeedScene(su);
    const PersistState s = su.CaptureState();

    ER_CHECK_EQ(s.bases.size(), size_t{ 2 });
    ER_CHECK_EQ(s.ships.size(), size_t{ 1 });   // the harvester (NPCs are not ships)
    ER_CHECK_EQ(s.builds.size(), size_t{ 1 });  // base 0's active build
    ER_CHECK_EQ(s.npcs.size(), size_t{ 3 });    // the guardian ring

    // Spot-check a mutated base survived capture exactly (int64 pos, layered HP, storage).
    ER_CHECK_EQ(s.bases[0].x, int64_t{ 16184 });
    ER_CHECK_EQ(s.bases[0].hullHp, int32_t{ 640 });
    ER_CHECK(s.bases[0].storage[0] == 1234.5f);
    ER_CHECK(s.bases[0].fuel == 175.0f);
}

ER_TEST(WarmRestartCapture, CaptureIsDeterministic)
{
    ServerUniverse su(false);
    SeedScene(su);
    // Two captures of the same unchanged state hash identically.
    ER_CHECK_EQ(StateHash(su.CaptureState()), StateHash(su.CaptureState()));
}

ER_TEST(WarmRestartCapture, CaptureEncodeDecodeRestoreRoundTrips)
{
    // Source shard.
    ServerUniverse a(false);
    SeedScene(a);
    const PersistState captured = a.CaptureState();

    // The exact Win32 path: blob it, ship it, decode it on a fresh shard.
    const std::vector<uint8_t> blob = EncodeState(captured);
    ER_CHECK(!blob.empty());
    PersistState decoded;
    ER_CHECK(DecodeState(blob, decoded));
    ER_CHECK_EQ(StateHash(decoded), StateHash(captured)); // blob round-trips

    // Restore into a stateless fresh shard and re-capture.
    ServerUniverse b(false);
    b.RestoreState(decoded);
    const PersistState recaptured = b.CaptureState();

    // (1) the PersistState mirror is reproduced exactly after the ECS rebuild.
    ER_CHECK_EQ(StateHash(recaptured), StateHash(captured));

    // (2) the restored sim's authoritative state matches for the economy/ownership
    //     subset (the §17 M5 SimHash check). Ship fuel was left at the default so the
    //     full SimHash (which includes fuel) matches as well.
    ER_CHECK_EQ(b.SimHash(), a.SimHash());
}

ER_TEST(WarmRestartCapture, RestoreRebindsOwnershipAndBuild)
{
    ServerUniverse a(false);
    const Seeded seed = SeedScene(a);
    PersistState decoded;
    ER_CHECK(DecodeState(EncodeState(a.CaptureState()), decoded));

    ServerUniverse b(false);
    b.RestoreState(decoded);

    // Ownership survives (the harvester still belongs to base 0's player id).
    ER_CHECK(b.StorageOf(seed.base0) != nullptr); // base restored under its original net id
    ER_CHECK(b.CargoOf(seed.harv) != nullptr);    // ship restored under its original net id
    if (auto* c = b.CargoOf(seed.harv)) ER_CHECK(c->amount[0] == 60.0f);

    // The active build came back (owner-scoped) and still advances.
    if (auto* q = b.BuildQueueOf(seed.base0)) {
        ER_CHECK(q->active);
        ER_CHECK(q->progress == 0.33f);
    }
}

ER_TEST(WarmRestartCapture, RestoreThenSpawnDoesNotCollideNetIds)
{
    ServerUniverse a(false);
    SeedScene(a);
    PersistState decoded;
    ER_CHECK(DecodeState(EncodeState(a.CaptureState()), decoded));

    ServerUniverse b(false);
    b.RestoreState(decoded);

    // A post-restore spawn must get a fresh net id (m_nextNetId advanced past the
    // restored max), not clobber a restored entity.
    const uint32_t fresh = b.SpawnBase({ 5000, 5000, 0 }, { 0, 0, 0 });
    ER_CHECK(b.StorageOf(fresh) != nullptr);
    for (const auto& base : decoded.bases) ER_CHECK(base.netId != fresh);
    for (const auto& sh : decoded.ships)   ER_CHECK(sh.netId != fresh);
    for (const auto& n : decoded.npcs)     ER_CHECK(n.netId != fresh);
}

ER_TEST(WarmRestartCapture, EmptyUniverseRestoresClean)
{
    ServerUniverse a(false);
    const PersistState empty = a.CaptureState();
    ER_CHECK_EQ(empty.bases.size(), size_t{ 0 });

    ServerUniverse b(false);
    SeedScene(b);                 // give b some state...
    b.RestoreState(empty);        // ...then restore an empty snapshot over it.
    const PersistState after = b.CaptureState();
    ER_CHECK_EQ(StateHash(after), StateHash(empty));
    ER_CHECK_EQ(after.bases.size(), size_t{ 0 });
    ER_CHECK_EQ(after.ships.size(), size_t{ 0 });
}
