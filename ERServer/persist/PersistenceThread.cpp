// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// PersistenceThread.cpp — dedicated persistence thread (M5 areas A/D/E/F, §9/§15).

#include "pch.h"
#include "PersistenceThread.h"

#include <chrono>
#include <ctime>
#include <unordered_map>

namespace Neuron::Persist
{
namespace
{
// Wall-clock seconds (UTC). Used for the RPO watermark + cadence timers + the unix
// timestamps the stores stamp. std::chrono keeps this off the Win32 SYSTEMTIME path.
int64_t NowUnix()
{
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

double MonoMs()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

PersistenceThread::PersistenceThread(PersistConfig cfg) : m_cfg(std::move(cfg)) {}

PersistenceThread::~PersistenceThread()
{
    Stop();
}

bool PersistenceThread::InitializeForTests()
{
    m_dbAvailable = m_pool.Initialize(m_cfg);
    m_economy     = std::make_unique<EconomyStore>(&m_pool);
    m_writeBehind = std::make_unique<WriteBehindStore>(&m_pool);
    m_snapshots   = std::make_unique<SimSnapshotStore>(&m_pool);
    return m_dbAvailable;
}

bool PersistenceThread::Start(SnapshotCaptureFn capture)
{
    if (m_running.load())
        return true;

    m_capture = std::move(capture);
    const bool ok = InitializeForTests(); // builds pool + stores; sets m_dbAvailable

    // Even if the DB is unavailable we may still spin the thread so the queues drain
    // (and keep escalating) once the DB returns — but for M5's degraded mode we simply
    // report failure and let the server run no-persist. Start the thread only if the DB
    // is reachable.
    if (!ok)
        return false;

    m_lastWriteBehindFlushUnix = NowUnix();
    m_lastSnapshotUnix = NowUnix();
    m_stop.store(false);
    m_running.store(true);
    m_thread = std::thread([this] { ThreadMain(); });
    return true;
}

void PersistenceThread::Stop()
{
    if (!m_running.load()) {
        // Still tear down stores/pool if InitializeForTests was used without Start.
        if (m_thread.joinable())
            m_thread.join();
        return;
    }
    m_stop.store(true);
    if (m_thread.joinable())
        m_thread.join();
    m_running.store(false);

    // Best-effort final economy drain so a graceful shutdown loses nothing (zero-loss).
    DrainEconomyOnce();
    m_pool.Shutdown();
}

size_t PersistenceThread::EnqueueEconomy(const EconomyMutation& m)
{
    const size_t depth = m_econQueue.Push(m);
    std::lock_guard lock(m_telMutex);
    ++m_counters.economyEnqueued;
    m_counters.economyQueueDepth = depth;
    return depth;
}

void PersistenceThread::EnqueueWriteBehind(const WriteBehindRow& row)
{
    const bool accepted = m_wbQueue.Push(WriteBehindRow{ row });
    if (!accepted) {
        std::lock_guard lock(m_telMutex);
        m_counters.writeBehindDropped = m_wbQueue.Dropped();
    }
}

std::optional<LoadedSnapshot> PersistenceThread::LoadLatestSnapshotForRestore()
{
    if (!m_snapshots) {
        if (!InitializeForTests())
            return std::nullopt;
    }
    return m_snapshots->LoadLatest();
}

std::optional<std::vector<OutboxRow>> PersistenceThread::ReadOutboxSince(int64_t watermarkOutboxId)
{
    if (!m_economy) {
        if (!InitializeForTests())
            return std::nullopt;
    }
    return m_economy->ReadSince(watermarkOutboxId);
}

PersistCounters PersistenceThread::Counters() const
{
    std::lock_guard lock(m_telMutex);
    PersistCounters c = m_counters;
    c.economyQueueDepth = m_econQueue.Depth(); // live gauge
    return c;
}

// -----------------------------------------------------------------------------
// Worker loop
// -----------------------------------------------------------------------------

void PersistenceThread::ThreadMain()
{
    // The loop is deliberately simple: drain economy as fast as it can (zero-loss), then
    // service the time-based write-behind + snapshot cadences, then sleep briefly. SQL
    // round-trips happen only here, so a slow DB stretches this loop — never the tick.
    while (!m_stop.load()) {
        DrainEconomyOnce();

        const int64_t now = NowUnix();
        FlushWriteBehindIfDue(now);
        SnapshotIfDue(now);

        // 5 ms idle yield — short enough that economy drain latency stays low, long
        // enough that an empty queue does not spin a core. (Not on the tick thread.)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void PersistenceThread::DrainEconomyOnce()
{
    if (!m_economy)
        return;

    std::vector<EconomyMutation> batch;
    m_econQueue.DrainInto(batch);
    if (batch.empty())
        return;

    const double t0 = MonoMs();
    uint64_t committed = 0, failed = 0;
    // In ORDER (the queue preserves push order — Outbox::Drain semantics). Each commit
    // is independently transactional + idempotent (idemKey), so a failure does not
    // corrupt the rest; a failed item is re-queued so zero-loss holds across a transient
    // DB blip (the event is authoritative only once committed).
    for (auto& m : batch) {
        auto outboxId = m_economy->AppendWriteThrough(m, NowUnix());
        if (outboxId) {
            ++committed;
        } else {
            ++failed;
            // Re-queue at the back to retry on the next pass (transient DB error). The
            // idemKey makes a later success exactly-once even if the original partially
            // applied — zero-loss without double-credit.
            m_econQueue.Push(std::move(m));
        }
    }
    const double dt = MonoMs() - t0;

    std::lock_guard lock(m_telMutex);
    m_counters.economyCommitted += committed;
    m_counters.economyFailed    += failed;
    m_counters.lastDrainLatencyMs = dt;
}

void PersistenceThread::FlushWriteBehindIfDue(int64_t nowUnix)
{
    if (!m_writeBehind)
        return;
    const uint64_t elapsedMs = static_cast<uint64_t>((nowUnix - m_lastWriteBehindFlushUnix) * 1000);
    if (elapsedMs < m_cfg.writeBehindRpoMs)
        return;

    std::vector<WriteBehindRow> drained;
    m_wbQueue.DrainInto(drained);
    m_lastWriteBehindFlushUnix = nowUnix;
    if (drained.empty()) {
        // Nothing to write but the cadence elapsed — the durable state is current, so
        // advance the RPO watermark to now (no loss window open).
        std::lock_guard lock(m_telMutex);
        m_counters.rpoWatermarkUnix = nowUnix;
        return;
    }

    // Coalesce to latest-wins per (entity,dbId) so a base that moved 60 times this
    // window writes ONE row (a dropped interim point is fine — bounded RPO, §15).
    std::unordered_map<uint64_t, WriteBehindRow> latest;
    latest.reserve(drained.size());
    for (const auto& r : drained) {
        const uint64_t key = (static_cast<uint64_t>(r.entity) << 62) ^ static_cast<uint64_t>(r.dbId);
        latest[key] = r; // last push for this entity wins
    }
    std::vector<WriteBehindRow> batch;
    batch.reserve(latest.size());
    for (auto& [k, v] : latest)
        batch.push_back(v);

    auto written = m_writeBehind->FlushBatch(batch);

    std::lock_guard lock(m_telMutex);
    ++m_counters.writeBehindBatches;
    if (written) {
        m_counters.writeBehindRows += static_cast<uint64_t>(*written);
        // The batch committed → high-frequency state is durable up to now (RPO met).
        m_counters.rpoWatermarkUnix = nowUnix;
    }
    m_counters.writeBehindDropped = m_wbQueue.Dropped();
}

void PersistenceThread::SnapshotIfDue(int64_t nowUnix)
{
    if (!m_snapshots || !m_capture)
        return;
    const uint64_t elapsedMs = static_cast<uint64_t>((nowUnix - m_lastSnapshotUnix) * 1000);
    if (elapsedMs < m_cfg.snapshotMs)
        return;
    m_lastSnapshotUnix = nowUnix;

    SnapshotRequest req = m_capture(nowUnix);
    if (!req.wantSnapshot || req.blob.empty())
        return;

    auto id = m_snapshots->Save(req.simTick, req.outboxWatermark, req.blob);
    if (id) {
        std::lock_guard lock(m_telMutex);
        ++m_counters.snapshotsWritten;
        m_counters.lastSnapshotTick = req.simTick;
    }
}

} // namespace Neuron::Persist
