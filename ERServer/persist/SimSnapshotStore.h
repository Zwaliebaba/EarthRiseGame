#pragma once
// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// SimSnapshotStore.h — warm-restart blob persistence (M5 area F, §15, §9, §26).
//
// Persists/loads the periodic binary snapshot to/from SimSnapshots (Blob
// VARBINARY(MAX) + SimTickNumber + OutboxWatermark from migration 004). The blob
// FORMAT and (de)serialization already exist and are verified in
// NeuronCore/WarmRestart.h (PersistState, EncodeState/DecodeState, StateHash) — this
// store does NOT re-implement any of that; it only moves the byte blob + the watermark
// in and out of SQL.
//
// Restart contract (§15): load the latest snapshot, then the EconomyOutbox rows with
// OutboxId > OutboxWatermark (EconomyStore::ReadSince) for the parent to replay. The
// blob also carries the watermark internally (PersistState::outboxSeq); the
// OutboxWatermark column surfaces it so the restart query needs no blob parse.

#include "OdbcConnectionPool.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Neuron::Persist
{

// A loaded snapshot: the raw blob (decode with Neuron::Persist::DecodeState) plus the
// row's metadata. The parent decodes the blob, restores the sim, then replays the
// outbox from outboxWatermark.
struct LoadedSnapshot
{
    int64_t              snapshotId{ 0 };
    int64_t              simTick{ 0 };
    int64_t              outboxWatermark{ 0 };
    std::vector<uint8_t> blob;

    [[nodiscard]] bool Empty() const noexcept { return blob.empty(); }
};

class SimSnapshotStore
{
public:
    explicit SimSnapshotStore(OdbcConnectionPool* pool) : m_pool(pool) {}

    SimSnapshotStore(const SimSnapshotStore&) = delete;
    SimSnapshotStore& operator=(const SimSnapshotStore&) = delete;

    // Write a snapshot row. 'blob' is the bytes from Neuron::Persist::EncodeState
    // (the caller serializes the ServerUniverse → PersistState → EncodeState; that
    // glue is the parent's integration step). 'outboxWatermark' is the max OutboxId
    // reflected in the blob at capture time (EconomyStore::MaxOutboxId). Returns the
    // new SnapshotId, or nullopt on error.
    [[nodiscard]] std::optional<int64_t>
    Save(int64_t simTick, int64_t outboxWatermark, std::span<const uint8_t> blob);

    // Load the latest snapshot (highest SimTickNumber). nullopt on error; a present-
    // but-empty LoadedSnapshot (Empty()) means "no snapshot yet" (cold start).
    [[nodiscard]] std::optional<LoadedSnapshot> LoadLatest();

    // Delete snapshots older than the newest 'keep' rows (housekeeping so the table
    // does not grow unbounded). Returns rows deleted, or nullopt on error.
    [[nodiscard]] std::optional<int64_t> PruneKeepingLatest(int keep);

private:
    OdbcConnectionPool* m_pool{ nullptr };
};

} // namespace Neuron::Persist
