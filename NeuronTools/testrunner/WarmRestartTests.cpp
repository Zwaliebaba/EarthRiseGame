// Warm-restart snapshot tests (M5 area F; §15, §9, §26). A restart replays the
// economy log onto the last binary state snapshot for a clean, verifiable state;
// warm-restart correctness is an uptime SLA (§26). Here: the snapshot blob codec
// round-trips and its StateHash is stable (the SimHash analog), and snapshot + log
// equals a continuous run with zero economy loss and idempotent re-recovery. Pure;
// mirrored on the Linux runner (§16.2). The ServerUniverse<->PersistState glue and
// the ODBC blob read/write are Win32/SQL.

#include "Outbox.h"
#include "WarmRestart.h"
#include "TestRunner.h"

#include <vector>

using namespace ertest;
using namespace Neuron::Persist;

namespace
{
    PersistState SampleState()
    {
        PersistState s;
        s.tick = 9001;
        s.outboxSeq = 3;
        s.bases.push_back({ 10, 0xACC1, 16184, 0, -50, 800, 1200, 4000,
                            { 600.0f, 0.0f, 12.5f }, 95.0f, 1, 0 });
        s.bases.push_back({ 11, 0xACC2, -32000, 2000, 0, 700, 1100, 3800,
                            { 0.0f, 40.0f, 0.0f }, 12.0f, 0, 1 });
        s.ships.push_back({ 20, 0xACC1, 16404, 0, -50, 500, { 30.0f, 0, 0 }, 2 });
        s.ships.push_back({ 21, 0xACC1, 16404, 80, -50, 480, { 0, 0, 0 }, 1 });
        s.builds.push_back({ 0xACC1, 3001, 0.42f });
        s.npcs.push_back({ 90, 100000, 0, 0, 280, 7, 1 });
        return s;
    }
}

ER_TEST(WarmRestart, SnapshotBlobRoundTripsAndHashMatches)
{
    const PersistState s = SampleState();
    const auto blob = EncodeState(s);
    ER_CHECK(!blob.empty());

    PersistState restored;
    ER_CHECK(DecodeState(blob, restored));
    ER_CHECK_EQ(StateHash(restored), StateHash(s)); // structural equality (SimHash analog)

    // Spot-check a few fields survived precisely (incl. int64 positions, layered HP).
    ER_CHECK_EQ(restored.bases.size(), size_t{ 2 });
    ER_CHECK_EQ(restored.bases[0].x, int64_t{ 16184 });
    ER_CHECK_EQ(restored.bases[1].baseState, uint8_t{ 1 }); // disable-not-destroy state
    ER_CHECK_EQ(restored.ships.size(), size_t{ 2 });
    ER_CHECK_EQ(restored.npcs[0].hp, int32_t{ 280 });
}

ER_TEST(WarmRestart, TruncatedBlobFailsToDecode)
{
    const PersistState s = SampleState();
    auto blob = EncodeState(s);
    blob.resize(blob.size() / 2); // chop it
    PersistState restored;
    ER_CHECK(!DecodeState(blob, restored)); // truncation is detected, not silently wrong
}

ER_TEST(WarmRestart, EmptyStateRoundTrips)
{
    PersistState empty;
    const auto blob = EncodeState(empty);
    PersistState restored;
    ER_CHECK(DecodeState(blob, restored));
    ER_CHECK_EQ(StateHash(restored), StateHash(empty));
    ER_CHECK_EQ(restored.bases.size(), size_t{ 0 });
}

ER_TEST(WarmRestart, SnapshotPlusLogEqualsContinuousRunWithZeroLoss)
{
    // Continuous run: maintain the economy by draining the outbox as events arrive.
    Outbox ob;
    for (uint64_t i = 1; i <= 8; ++i) ob.Append(0x500 + i, 1, static_cast<int64_t>(i));
    EconomyState continuous;
    ob.Drain(continuous);
    const int64_t expected = (1 + 2 + 3 + 4 + 5 + 6 + 7 + 8);

    // Sim snapshot blob (taken at some tick) + the durable economy log.
    const PersistState sim = SampleState();
    const auto simBlob = EncodeState(sim);

    // Restart: decode the snapshot, then replay the economy log onto a fresh economy.
    PersistState restoredSim;
    ER_CHECK(DecodeState(simBlob, restoredSim));
    EconomyState restoredEcon;
    ob.ReplayAll(restoredEcon);

    ER_CHECK_EQ(StateHash(restoredSim), StateHash(sim));        // sim restored exactly
    ER_CHECK_EQ(restoredEcon.Balance(1), expected);            // economy zero-loss
    ER_CHECK_EQ(restoredEcon.Balance(1), continuous.Balance(1)); // == continuous run

    // A second restart from the restored state is stable (idempotent recovery).
    PersistState restoredSim2;
    ER_CHECK(DecodeState(EncodeState(restoredSim), restoredSim2));
    ER_CHECK_EQ(StateHash(restoredSim2), StateHash(sim));
    ER_CHECK_EQ(ob.ReplayAll(restoredEcon), size_t{ 0 });      // no double-credit
    ER_CHECK_EQ(restoredEcon.Balance(1), expected);
}
