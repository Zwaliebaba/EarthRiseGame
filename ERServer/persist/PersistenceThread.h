#pragma once
// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// PersistenceThread.h — the dedicated persistence thread (M5 areas A/D/E, §9/§15).
//
// A single OS thread, OFF the 30 Hz tick hot path (§9), that owns BOTH durability
// paths and the connection pool:
//   * WRITE-THROUGH economy (area D): drains the economy queue in order, committing
//     each mutation transactionally via EconomyStore (zero-loss, idempotent — the
//     Outbox.h contract). The sim enqueues an economy mutation and continues; the
//     event is authoritative once the thread commits it.
//   * WRITE-BEHIND state (area E): drains the bounded position/HP queue, coalesces to
//     latest-wins per entity, and flushes a batch at the RPO cadence via
//     WriteBehindStore (bounded loss, never an economy event).
//   * SNAPSHOT (area F): on the snapshot cadence, asks a caller-supplied capture
//     callback for the latest encoded blob + watermark and writes it via
//     SimSnapshotStore. (The ServerUniverse→PersistState→EncodeState capture is the
//     parent's glue; the thread just persists what the callback returns.)
//
// SQL LATENCY NEVER STALLS THE SIM (§9): the sim only ever calls the non-blocking
// Enqueue* methods (an O(1) push to an in-memory queue — see PersistQueue.h). All SQL
// round-trips happen on THIS thread. If SQL is slow, the economy queue grows (Depth()
// is exposed so the operator/drill can see it) but the tick never waits; the bounded
// write-behind queue sheds oldest points (bounded RPO) rather than back-pressure.

#include "EconomyStore.h"
#include "OdbcConnectionPool.h"
#include "PersistConfig.h"
#include "PersistQueue.h"
#include "SimSnapshotStore.h"
#include "WriteBehindStore.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace Neuron::Persist
{

// What the thread asks the sim for when it is time to take a warm-restart snapshot.
// The parent fills it by serializing the frozen ServerUniverse → PersistState →
// Neuron::Persist::EncodeState, and reports the outbox watermark at capture time
// (EconomyStore::MaxOutboxId). 'wantSnapshot=false' tells the thread to skip (e.g. no
// state yet). Pure data so the callback stays on the caller's side.
struct SnapshotRequest
{
    bool                 wantSnapshot{ false };
    int64_t              simTick{ 0 };
    int64_t              outboxWatermark{ 0 };
    std::vector<uint8_t> blob; // Neuron::Persist::EncodeState output
};

// §21 persistence telemetry (area H). Monotonic counters + live gauges the drill reads.
struct PersistCounters
{
    uint64_t economyEnqueued{ 0 };     // pushes from the sim
    uint64_t economyCommitted{ 0 };    // write-through commits (zero-loss proof)
    uint64_t economyFailed{ 0 };       // commit attempts that errored (escalation signal)
    uint64_t economyQueueDepth{ 0 };   // live outbox queue depth (§21 "outbox depth")
    double   lastDrainLatencyMs{ 0.0 };// last economy-drain batch latency (§21)
    uint64_t writeBehindRows{ 0 };     // rows flushed
    uint64_t writeBehindBatches{ 0 };
    uint64_t writeBehindDropped{ 0 };  // bounded-queue drops (RPO lag signal)
    int64_t  rpoWatermarkUnix{ 0 };    // last time write-behind state was made durable (§21 RPO)
    uint64_t snapshotsWritten{ 0 };
    int64_t  lastSnapshotTick{ 0 };
};

class PersistenceThread
{
public:
    // Capture callback: the thread calls it on the snapshot cadence to obtain the blob
    // + watermark. Runs ON the persistence thread; the parent's implementation must
    // therefore snapshot the sim safely (e.g. read the frozen post-tick state, or copy
    // under whatever guarantee ServerUniverse provides). Returns wantSnapshot=false to
    // skip this round.
    using SnapshotCaptureFn = std::function<SnapshotRequest(int64_t /*nowUnix*/)>;

    // 'nowUnixFn' supplies wall-clock seconds (the parent passes a real clock; tests
    // pass a fake) so the RPO/snapshot cadences and the RPO watermark are testable.
    explicit PersistenceThread(PersistConfig cfg);
    ~PersistenceThread();

    PersistenceThread(const PersistenceThread&) = delete;
    PersistenceThread& operator=(const PersistenceThread&) = delete;

    // Initialize the pool + stores and START the worker thread. Returns false if the DB
    // is unreachable (no connection); the server then runs in degraded/no-persist mode.
    // 'capture' may be null (no snapshots taken — e.g. before the sim glue lands).
    [[nodiscard]] bool Start(SnapshotCaptureFn capture = nullptr);

    // Signal the worker to flush what it can and exit; joins the thread.
    void Stop();

    [[nodiscard]] bool Running() const noexcept { return m_running.load(); }
    [[nodiscard]] bool DbAvailable() const noexcept { return m_dbAvailable; }

    // ---- producer API (called from the 30 Hz tick thread — NON-BLOCKING) -------
    // Enqueue an economy mutation for write-through (zero-loss). O(1) push; the thread
    // commits it. Returns the live economy queue depth (for the sim's own back-pressure
    // awareness; it should NOT block on it).
    size_t EnqueueEconomy(const EconomyMutation& m);

    // Enqueue a high-frequency state row for write-behind (bounded RPO). O(1) push;
    // may drop the oldest queued row under backlog (bounded loss is allowed, §15).
    void EnqueueWriteBehind(const WriteBehindRow& row);

    // ---- restart path (area F) -------------------------------------------------
    // Synchronous helpers the PARENT calls at startup (before Start, on the main
    // thread) to restore: load the latest snapshot, then the post-watermark outbox
    // rows for replay. These bypass the queues (no concurrency at boot).
    [[nodiscard]] std::optional<LoadedSnapshot> LoadLatestSnapshotForRestore();
    [[nodiscard]] std::optional<std::vector<OutboxRow>> ReadOutboxSince(int64_t watermarkOutboxId);

    // Direct store access for the parent's account/auth wiring + tests. Valid after a
    // successful Start (or InitializeForTests). Never null once initialized.
    [[nodiscard]] OdbcConnectionPool* Pool() noexcept { return &m_pool; }
    [[nodiscard]] EconomyStore*      Economy() noexcept { return m_economy.get(); }
    [[nodiscard]] WriteBehindStore*  WriteBehind() noexcept { return m_writeBehind.get(); }
    [[nodiscard]] SimSnapshotStore*  Snapshots() noexcept { return m_snapshots.get(); }

    // ---- telemetry (area H) ----------------------------------------------------
    [[nodiscard]] PersistCounters Counters() const;

    // Construct the pool + stores WITHOUT starting the thread (unit tests that drive the
    // stores directly). Returns false if the DB is unreachable. Public for ERServerTest.
    [[nodiscard]] bool InitializeForTests();

private:
    void ThreadMain();
    void DrainEconomyOnce();                 // write-through, ordered, idempotent
    void FlushWriteBehindIfDue(int64_t nowUnix); // batched at RPO cadence
    void SnapshotIfDue(int64_t nowUnix);     // warm-restart blob on snapshot cadence

    PersistConfig       m_cfg;
    OdbcConnectionPool  m_pool;
    std::unique_ptr<EconomyStore>     m_economy;
    std::unique_ptr<WriteBehindStore> m_writeBehind;
    std::unique_ptr<SimSnapshotStore> m_snapshots;

    MpscZeroLossQueue<EconomyMutation> m_econQueue;
    MpscBoundedQueue<WriteBehindRow>   m_wbQueue{ 4096 };

    SnapshotCaptureFn   m_capture;
    std::thread         m_thread;
    std::atomic<bool>   m_running{ false };
    std::atomic<bool>   m_stop{ false };
    bool                m_dbAvailable{ false };

    int64_t             m_lastWriteBehindFlushUnix{ 0 };
    int64_t             m_lastSnapshotUnix{ 0 };

    // Telemetry — written only by the worker thread except the queue-depth gauge, read
    // via Counters() under m_telMutex.
    mutable std::mutex  m_telMutex;
    PersistCounters     m_counters;
};

} // namespace Neuron::Persist
