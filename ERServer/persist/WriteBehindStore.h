#pragma once
// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// WriteBehindStore.h — batched high-frequency state writes (M5 area E, §15).
//
// The durability boundary's BOUNDED-LOSS half (§15). Position / layered-HP / state for
// Bases and Ships is written BEHIND the sim in batches at an RPO cadence (a few seconds
// of movement may be lost on a hard crash; NEVER an economy event — that is area D's
// zero-loss path). This carries NO currency/ledger/outbox effect — the separation
// invariant (§15, asserted in the area-E test).
//
// Batching: the sim pushes WriteBehindRow items (keyed by entity id) to the persistence
// thread's bounded queue. The thread coalesces to latest-wins per entity and flushes
// the batch with an UPDATE per row (TVP/bcp for big checkpoints is the §15 optimization;
// a per-row UPDATE is the correct-and-simple first pass, noted for upgrade).

#include "OdbcConnectionPool.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Neuron::Persist
{

// Which table a write-behind row targets (Bases vs Ships share the layered-HP shape).
enum class WriteBehindEntity : uint8_t { Base = 0, Ship = 1 };

// One high-frequency state row. The sim fills it from its authoritative components
// (UniversePos → int64 metres §6; layered HP from §13.1). 'dbId' is the SQL PK
// (BaseId/ShipId) the row updates; the sim maps its netId→dbId at spawn (the parent's
// glue). 'stateByte' is BaseState/ShipState.
struct WriteBehindRow
{
    WriteBehindEntity entity{ WriteBehindEntity::Base };
    int64_t  dbId{ 0 };
    int64_t  x{ 0 }, y{ 0 }, z{ 0 };  // int64 metres
    int32_t  shieldHp{ 0 }, armorHp{ 0 }, hullHp{ 0 };
    uint8_t  stateByte{ 0 };
};

class WriteBehindStore
{
public:
    explicit WriteBehindStore(OdbcConnectionPool* pool) : m_pool(pool) {}

    WriteBehindStore(const WriteBehindStore&) = delete;
    WriteBehindStore& operator=(const WriteBehindStore&) = delete;

    // Flush a coalesced batch (latest-wins per dbId already applied by the caller).
    // Each row is one UPDATE inside a single transaction so the whole batch lands
    // atomically at the RPO boundary. Returns the number of rows written, or nullopt
    // on a DB error (the batch is rolled back; the watermark does not advance).
    [[nodiscard]] std::optional<int64_t> FlushBatch(std::span<const WriteBehindRow> rows);

private:
    OdbcConnectionPool* m_pool{ nullptr };
};

} // namespace Neuron::Persist
