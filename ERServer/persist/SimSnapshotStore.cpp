// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// SimSnapshotStore.cpp — warm-restart blob persistence (M5 area F, §15).

#include "pch.h"
#include "SimSnapshotStore.h"

#include <cstring>

namespace Neuron::Persist
{
namespace
{
int64_t ReadI64(const SqlValue& v)
{
    if (v.isNull || v.bytes.size() < sizeof(int64_t))
        return 0;
    int64_t out = 0;
    std::memcpy(&out, v.bytes.data(), sizeof(int64_t));
    return out;
}
} // namespace

std::optional<int64_t>
SimSnapshotStore::Save(int64_t simTick, int64_t outboxWatermark, std::span<const uint8_t> blob)
{
    auto lease = m_pool->Acquire();
    if (!lease)
        return std::nullopt;

    // Single INSERT — the blob is bound as VARBINARY(MAX) (the wrapper streams the full
    // span). OutboxWatermark (migration 004) records the replay boundary so restart
    // reads exactly EconomyOutbox WHERE OutboxId > OutboxWatermark.
    const SqlParam p[] = {
        SqlParam::Make(simTick),
        SqlParam::Make(blob),
        SqlParam::Make(outboxWatermark),
    };
    return lease->ExecInsertReturningIdentity(
        "INSERT INTO SimSnapshots (SimTickNumber, Blob, OutboxWatermark) VALUES (?, ?, ?)", p);
}

std::optional<LoadedSnapshot> SimSnapshotStore::LoadLatest()
{
    auto lease = m_pool->Acquire();
    if (!lease)
        return std::nullopt;

    // TOP 1 by SimTickNumber DESC — matches IX_SimSnapshots_Tick. If the table is empty
    // we return a default (Empty()) LoadedSnapshot so the caller cold-starts cleanly.
    auto res = lease->ExecQuery(
        "SELECT TOP 1 SnapshotId, SimTickNumber, ISNULL(OutboxWatermark, 0), Blob "
        "FROM SimSnapshots ORDER BY SimTickNumber DESC", {}, 4);
    if (!res)
        return std::nullopt;
    if (res->Empty())
        return LoadedSnapshot{}; // no snapshot yet → cold start

    const SqlRow& row = res->rows.front();
    LoadedSnapshot snap;
    snap.snapshotId      = ReadI64(row.size() > 0 ? row[0] : SqlValue{});
    snap.simTick         = ReadI64(row.size() > 1 ? row[1] : SqlValue{});
    snap.outboxWatermark = ReadI64(row.size() > 2 ? row[2] : SqlValue{});
    if (row.size() > 3 && !row[3].isNull)
        snap.blob = row[3].bytes; // already exact-sized by the chunked SQLGetData reader
    return snap;
}

std::optional<int64_t> SimSnapshotStore::PruneKeepingLatest(int keep)
{
    auto lease = m_pool->Acquire();
    if (!lease)
        return std::nullopt;
    // Keep the newest 'keep' rows; delete the rest. A correlated NOT IN over the TOP-N
    // newest ids — Azure-SQL-compatible (no Agent job; runs on demand from the
    // persistence thread on the snapshot cadence).
    const SqlParam p[] = { SqlParam::Make(static_cast<int64_t>(keep < 1 ? 1 : keep)) };
    return lease->ExecNonQuery(
        "DELETE FROM SimSnapshots WHERE SnapshotId NOT IN "
        "(SELECT TOP (?) SnapshotId FROM SimSnapshots ORDER BY SimTickNumber DESC)", p);
}

} // namespace Neuron::Persist
