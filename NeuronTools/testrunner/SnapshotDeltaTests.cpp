// Quantized sector-local delta snapshot codec tests (masterplan §8.4, App. A;
// M4 area C). The delta record (netId + changed-field mask + quantized
// sector-local position + masked fields) replaces the fixed ~46 B absolute-int64
// record: a moving entity costs ~16 B, a stationary one nothing, and the wire
// never carries an absolute int64 (R2). Mirrors the Windows NeuronCoreTest cases
// on the Linux runner (§16.2).
//
// Component ids are bound once per binary in ShapeCatalogTests.cpp.

#include "ServerUniverse.h"
#include "Snapshot.h"
#include "TestRunner.h"

#include <cmath>
#include <vector>

using namespace ertest;
using namespace Neuron::Sim;
using Neuron::Universe::UniversePos;

namespace
{
    // A full-state entity for first-sight / baseline tests.
    SnapshotEntity MakeEntity(uint32_t netId, UniversePos pos, DirectX::XMFLOAT3 local,
                              int32_t hp, uint16_t shape, EntityKind kind, uint32_t owner)
    {
        SnapshotEntity e;
        e.netId = netId; e.pos = pos; e.localOffset = local; e.hp = hp;
        e.shapeId = shape; e.kind = kind; e.ownerPlayer = owner;
        return e;
    }

    // Reconstructed absolute metres on one axis (int position + sub-metre offset).
    double AbsMetres(int64_t pos, float local) { return static_cast<double>(pos) + local; }
}

// --- round-trip + changed-field mask ----------------------------------------

ER_TEST(SnapshotDelta, FirstSightRoundTripWithinQuantBound)
{
    const SnapshotEntity src = MakeEntity(7, { 1000, 2000, 3000 }, { 0.25f, 0.5f, 0.75f },
                                          640, 5, EntityKind::Ship, 42);
    DeltaSnapshot snap;
    snap.tick = 11;
    snap.records.push_back(MakeDeltaRecord(src, nullptr)); // first sight = full record

    DeltaDecodeState dec;
    ER_CHECK(dec.Apply(EncodeDeltaSnapshot(snap)));
    const SnapshotEntity* got = dec.Find(7);
    ER_CHECK(got != nullptr);

    // Position reconstructs within one quantization step on every axis.
    const double step = static_cast<double>(Neuron::Universe::kSectorSize) /
                        static_cast<double>(uint64_t(1) << kPosQuantBitsPerAxis);
    ER_CHECK(std::fabs(AbsMetres(got->pos.x, got->localOffset.x) - 1000.25) <= step);
    ER_CHECK(std::fabs(AbsMetres(got->pos.y, got->localOffset.y) - 2000.5) <= step);
    ER_CHECK(std::fabs(AbsMetres(got->pos.z, got->localOffset.z) - 3000.75) <= step);
    // Non-position fields are exact.
    ER_CHECK_EQ(got->hp, int32_t{ 640 });
    ER_CHECK_EQ(got->shapeId, uint16_t{ 5 });
    ER_CHECK_EQ(got->ownerPlayer, uint32_t{ 42 });
    ER_CHECK(got->kind == EntityKind::Ship);
}

ER_TEST(SnapshotDelta, MaskOmitsUnchangedFields)
{
    const SnapshotEntity base = MakeEntity(7, { 1000, 0, 0 }, { 0, 0, 0 }, 640, 5, EntityKind::Ship, 42);
    SnapshotEntity cur = base;
    cur.hp = 600; // only HP changed

    const DeltaRecord r = MakeDeltaRecord(cur, &base);
    ER_CHECK_EQ(r.mask, uint8_t{ DeltaHp }); // exactly the HP bit, nothing else
    // A one-field delta is much smaller than the full first-sight record.
    ER_CHECK(DeltaRecordBits(r) < DeltaRecordBits(MakeDeltaRecord(cur, nullptr)));

    // It decodes onto the prior state, touching only HP.
    DeltaDecodeState dec;
    DeltaSnapshot s0; s0.tick = 1; s0.records.push_back(MakeDeltaRecord(base, nullptr));
    dec.Apply(EncodeDeltaSnapshot(s0));
    DeltaSnapshot s1; s1.tick = 2; s1.records.push_back(r);
    dec.Apply(EncodeDeltaSnapshot(s1));
    ER_CHECK_EQ(dec.Find(7)->hp, int32_t{ 600 });
    ER_CHECK_EQ(dec.Find(7)->shapeId, uint16_t{ 5 }); // unchanged field preserved
}

ER_TEST(SnapshotDelta, StationaryEntityCostsNothing)
{
    const SnapshotEntity base = MakeEntity(7, { 1234, 5678, 9012 }, { 0.1f, 0.2f, 0.3f },
                                           640, 5, EntityKind::Ship, 42);
    const SnapshotEntity cur = base; // unchanged this tick
    ER_CHECK_EQ(MakeDeltaRecord(cur, &base).mask, uint8_t{ 0 }); // no record at all
}

ER_TEST(SnapshotDelta, SectorCrossingReAnchorsPosition)
{
    const SnapshotEntity base = MakeEntity(7, { 100, 0, 0 }, { 0, 0, 0 }, 1, 0, EntityKind::Base, 1);
    SnapshotEntity cur = base;
    cur.pos.x = Neuron::Universe::kSectorSize + 100; // crossed into the next sector on x
    const DeltaRecord r = MakeDeltaRecord(cur, &base);
    ER_CHECK((r.mask & DeltaSector) != 0);
    ER_CHECK((r.mask & DeltaPos) != 0); // crossing forces position to be re-sent

    DeltaDecodeState dec;
    DeltaSnapshot s0; s0.tick = 1; s0.records.push_back(MakeDeltaRecord(base, nullptr));
    dec.Apply(EncodeDeltaSnapshot(s0));
    DeltaSnapshot s1; s1.tick = 2; s1.records.push_back(r);
    dec.Apply(EncodeDeltaSnapshot(s1));
    const double step = static_cast<double>(Neuron::Universe::kSectorSize) /
                        static_cast<double>(uint64_t(1) << kPosQuantBitsPerAxis);
    ER_CHECK(std::fabs(AbsMetres(dec.Find(7)->pos.x, dec.Find(7)->localOffset.x) -
                       static_cast<double>(Neuron::Universe::kSectorSize + 100)) <= step);
}

// --- MTU budget + spillover -------------------------------------------------

ER_TEST(SnapshotDelta, BudgetedSnapshotNeverExceedsAndSpillsTheRest)
{
    std::vector<DeltaRecord> recs;
    for (uint32_t i = 1; i <= 20; ++i)
        recs.push_back(MakeDeltaRecord(
            MakeEntity(i, { 1000 * i, 0, 0 }, { 0, 0, 0 }, 100, 5, EntityKind::Ship, 1), nullptr));

    std::vector<uint32_t> overflow;
    const size_t budget = 60; // only a couple of full records fit
    const DeltaSnapshot snap = BuildBudgetedSnapshot(1, recs, budget, overflow);

    ER_CHECK(EncodeDeltaSnapshot(snap).size() <= budget); // never larger than the budget
    ER_CHECK(!overflow.empty());                          // the rest spilled
    ER_CHECK_EQ(snap.records.size() + overflow.size(), size_t{ 20 }); // none dropped
}

// --- last-writer-wins idempotency (reorder / duplicate) ---------------------

ER_TEST(SnapshotDelta, ReorderedAndDuplicateSnapshotsConvergeByTick)
{
    const SnapshotEntity a = MakeEntity(1, { 0, 0, 0 }, { 0, 0, 0 }, 100, 5, EntityKind::Ship, 7);
    DeltaDecodeState dec;

    DeltaSnapshot s5; s5.tick = 5; s5.records.push_back(MakeDeltaRecord(a, nullptr));
    dec.Apply(EncodeDeltaSnapshot(s5));
    ER_CHECK_EQ(dec.Find(1)->hp, int32_t{ 100 });

    SnapshotEntity a4 = a; a4.hp = 50;             // an older tick arriving late
    DeltaSnapshot s4; s4.tick = 4; s4.records.push_back(MakeDeltaRecord(a4, &a));
    dec.Apply(EncodeDeltaSnapshot(s4));
    ER_CHECK_EQ(dec.Find(1)->hp, int32_t{ 100 });  // stale tick ignored

    SnapshotEntity a6 = a; a6.hp = 80;
    DeltaSnapshot s6; s6.tick = 6; s6.records.push_back(MakeDeltaRecord(a6, &a));
    dec.Apply(EncodeDeltaSnapshot(s6));
    ER_CHECK_EQ(dec.Find(1)->hp, int32_t{ 80 });   // newer tick applied

    SnapshotEntity a6b = a; a6b.hp = 10;
    DeltaSnapshot s6b; s6b.tick = 6; s6b.records.push_back(MakeDeltaRecord(a6b, &a));
    dec.Apply(EncodeDeltaSnapshot(s6b));
    ER_CHECK_EQ(dec.Find(1)->hp, int32_t{ 80 });   // duplicate tick ignored
}

// --- ServerUniverse integration ---------------------------------------------

ER_TEST(SnapshotDelta, EncodesLiveServerEntityRoundTrip)
{
    ServerUniverse su(false);
    const uint32_t base = su.SpawnBase({ 4096, 8192, 12288 }, { 0, 0, 0 });
    su.Step(0.1f);

    SnapshotEntity cur;
    ER_CHECK(su.SnapshotEntityOf(base, cur));
    DeltaSnapshot snap; snap.tick = su.Tick();
    snap.records.push_back(MakeDeltaRecord(cur, nullptr));

    DeltaDecodeState dec;
    ER_CHECK(dec.Apply(EncodeDeltaSnapshot(snap)));
    const SnapshotEntity* got = dec.Find(base);
    ER_CHECK(got != nullptr);
    const double step = static_cast<double>(Neuron::Universe::kSectorSize) /
                        static_cast<double>(uint64_t(1) << kPosQuantBitsPerAxis);
    ER_CHECK(std::fabs(AbsMetres(got->pos.x, got->localOffset.x) -
                       AbsMetres(cur.pos.x, cur.localOffset.x)) <= step);
    ER_CHECK(got->shapeId == cur.shapeId);
    ER_CHECK(got->kind == cur.kind);
    ER_CHECK_EQ(got->ownerPlayer, cur.ownerPlayer);
}
