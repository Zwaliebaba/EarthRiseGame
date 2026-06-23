#pragma once
// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// EconomyStore.h — write-through economy outbox (M5 area D, §15).
//
// The durability boundary's ZERO-LOSS half (§15, §17 M5 Done). Every economy mutation
// is committed transactionally — an EconomyOutbox row + the Wallets balance change + a
// CurrencyLedger row in ONE transaction — so the event is never lost on crash and is
// authoritative only once committed. This is the SQL realisation of the verified
// portable model Neuron::Persist::Outbox / EconomyState (NeuronCore/Outbox.h); the
// semantics here mirror it exactly:
//
//   * Append == Outbox::Append + EconomyState::Apply in one transaction. The
//     IdempotencyKey (migration 004, UNIQUE filtered index) makes a replayed drain
//     exactly-once: a re-staged event with the same key collides (23000) and is
//     skipped — the "CrashBeforeDrainReplaysWithZeroLossAndNoDoubleCredit" invariant,
//     enforced in SQL rather than memory.
//   * Drain/Replay == Outbox::Drain/ReplayAll: apply unprocessed rows in OutboxId
//     order, idempotently; ConfirmDrained marks rows ProcessedAt. On warm restart the
//     post-snapshot rows (OutboxId > watermark) ARE the log replayed onto the snapshot.
//
// Azure-SQL-compatible: a single multi-statement transaction, no cross-DB / Agent /
// FILESTREAM (§15).

#include "OdbcConnectionPool.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Neuron::Persist
{

// Mirrors Neuron::Persist::EconomyEventType (Outbox.h) so the sim's enum maps 1:1.
enum class EconomyEventKind : uint8_t
{
    WalletDelta    = 0,
    BuildComplete  = 1,
    InventoryDelta = 2,
    LootClaim      = 3,
};

// One economy mutation to commit write-through. 'idemKey' is the caller-assigned
// exactly-once key (the sim derives it deterministically from the event source —
// e.g. BuildId, KillId — so a replay produces the SAME key, matching Outbox.h's
// idemKey/ledger-RefId dedupe). 'amount' is the signed credit delta.
struct EconomyMutation
{
    uint64_t         idemKey{ 0 };
    int64_t          accountId{ 0 };
    int64_t          amount{ 0 };
    EconomyEventKind kind{ EconomyEventKind::WalletDelta };
    std::string      reason;   // CurrencyLedger.Reason (e.g. "build_complete","bounty")
    std::string      refType;  // CurrencyLedger.RefType (e.g. "Build","Kill") — dedupe pair
    int64_t          refId{ 0 }; // CurrencyLedger.RefId (with refType: UX_CurrencyLedger_Ref)
};

// An outbox row read back for the ordered drain (warm-restart replay, area F).
struct OutboxRow
{
    int64_t     outboxId{ 0 };
    uint64_t    idemKey{ 0 };
    std::string eventType;
    std::string payload; // JSON
};

class EconomyStore
{
public:
    explicit EconomyStore(OdbcConnectionPool* pool) : m_pool(pool) {}

    EconomyStore(const EconomyStore&) = delete;
    EconomyStore& operator=(const EconomyStore&) = delete;

    // Commit one economy mutation write-through (Outbox row + Wallet + Ledger in ONE
    // transaction). Returns the new OutboxId on success; std::nullopt on DB error.
    // If the idemKey already exists (UNIQUE violation) the call is a no-op success that
    // returns the EXISTING OutboxId — exactly-once, no double-credit (Outbox.h Apply
    // returning false for a known idemKey). This is the path the sim's economy events
    // route through (build completion, deposits, ownership), area D.
    [[nodiscard]] std::optional<int64_t> AppendWriteThrough(const EconomyMutation& m, int64_t nowUnix);

    // The ordered drain (area D/F): mark a row processed once its effect is durable.
    // In the all-in-one-transaction model the effect is already committed at append, so
    // the drain's job is to advance ProcessedAt (the watermark) for housekeeping +
    // telemetry; idempotency rests on the IdempotencyKey index, not this flag.
    [[nodiscard]] bool MarkProcessed(int64_t outboxId, int64_t nowUnix);

    // Read unprocessed outbox rows in OutboxId order (Outbox::PendingCount / Drain).
    [[nodiscard]] std::optional<std::vector<OutboxRow>> ReadUnprocessed(int maxRows = 256);

    // Read outbox rows with OutboxId > watermark in order — the warm-restart "log since
    // the snapshot" (area F). The parent replays these onto the loaded snapshot.
    [[nodiscard]] std::optional<std::vector<OutboxRow>> ReadSince(int64_t watermarkOutboxId, int maxRows = 4096);

    // Current wallet balance (audit / test).
    [[nodiscard]] std::optional<int64_t> WalletBalance(int64_t accountId);

    // Count of not-yet-processed outbox rows — the §21 "outbox depth" gauge (area H).
    [[nodiscard]] std::optional<int64_t> OutboxDepth();

    // Highest OutboxId (the append watermark / Outbox::MaxSeq) — used to stamp a
    // snapshot's OutboxWatermark at capture time (area F).
    [[nodiscard]] std::optional<int64_t> MaxOutboxId();

private:
    OdbcConnectionPool* m_pool{ nullptr };

    static const char* EventTypeName(EconomyEventKind k) noexcept;
};

} // namespace Neuron::Persist
