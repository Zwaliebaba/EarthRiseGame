#pragma once
// Outbox.h — write-through economy outbox: ordered, idempotent, zero-loss (M5 area D, §15).
//
// The durability boundary's zero-loss half (§15, §17 M5 Done "zero economy loss").
// Every economy mutation (currency, build completion, inventory, later kills/loot)
// appends an **ordered, durable** event in the *same transaction* as the balance
// change, so the event is never lost on crash. A drain applies events in order; the
// apply is **idempotent**, so a crash mid-drain (or a lost drain watermark) replays
// cleanly with no double-credit. On warm restart (area F) the post-snapshot outbox
// events are exactly the log replayed onto the snapshot.
//
// This is the pure ordering/idempotency model (mirrored on the Linux testrunner,
// §16.2). The ODBC `EconomyOutbox`/`Wallets`/`CurrencyLedger` writes (area A schema)
// are the Win32/SQL side; modelling the contract here means "zero-loss + idempotent
// replay" is unit-tested, not just asserted against a database.

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Neuron::Persist
{

enum class EconomyEventType : uint8_t
{
    WalletDelta = 0,   // currency credit/debit (amount signed)
    BuildComplete = 1, // a build-queue completion (carries credit/value)
    InventoryDelta = 2,// itemized storage change recorded as value
    LootClaim = 3,     // loot-on-kill recovery (M6 uses the same path)
};

struct EconomyEvent
{
    uint64_t        seq{ 0 };       // monotonic, assigned by the outbox on append
    uint64_t        idemKey{ 0 };   // idempotency key (unique; the ledger's dedupe index)
    uint32_t        accountId{ 0 };
    int64_t         amount{ 0 };    // signed currency delta
    EconomyEventType type{ EconomyEventType::WalletDelta };
};

struct LedgerEntry
{
    uint64_t seq{ 0 };
    uint64_t idemKey{ 0 };
    uint32_t accountId{ 0 };
    int64_t  delta{ 0 };
    int64_t  balanceAfter{ 0 };
};

// The canonical economy state an outbox drains into. In production this is the
// `Wallets` + append-only `CurrencyLedger` (with a UNIQUE index on idemKey). The
// dedupe set models that index: it is committed *with* the wallet/ledger rows in one
// transaction, so it survives a crash even when the drain watermark does not.
class EconomyState
{
public:
    // Apply one event transactionally + idempotently. Returns true iff newly applied.
    // - Idempotent: a seq already covered by the watermark, or an idemKey already in
    //   the ledger, is a no-op (defends replay against double-credit).
    // - Atomic: wallet, ledger, and dedupe set advance together; if 'injectFailure'
    //   is set the whole apply rolls back (nothing mutates) — the all-or-nothing model.
    bool Apply(const EconomyEvent& e, bool injectFailure = false)
    {
        if (e.seq <= m_lastAppliedSeq) return false;          // already drained (watermark)
        if (m_appliedKeys.count(e.idemKey)) return false;     // already in ledger (unique idemKey)
        if (injectFailure) return false;                      // rollback: nothing mutated

        int64_t& bal = m_wallet[e.accountId];
        bal += e.amount;
        m_ledger.push_back({ e.seq, e.idemKey, e.accountId, e.amount, bal });
        m_appliedKeys.insert(e.idemKey);
        if (e.seq > m_lastAppliedSeq) m_lastAppliedSeq = e.seq;
        return true;
    }

    [[nodiscard]] int64_t  Balance(uint32_t accountId) const
    {
        auto it = m_wallet.find(accountId);
        return it == m_wallet.end() ? 0 : it->second;
    }
    [[nodiscard]] size_t   LedgerSize() const noexcept { return m_ledger.size(); }
    [[nodiscard]] uint64_t LastAppliedSeq() const noexcept { return m_lastAppliedSeq; }
    [[nodiscard]] const std::vector<LedgerEntry>& Ledger() const noexcept { return m_ledger; }

    // Simulate a hard crash that loses the in-memory drain watermark but NOT the
    // durably-committed wallet/ledger/dedupe (they were in the transaction). Replay
    // from the outbox then relies on the idemKey dedupe, not the watermark.
    void DropWatermarkOnly() noexcept { m_lastAppliedSeq = 0; }

private:
    std::unordered_map<uint32_t, int64_t> m_wallet;
    std::vector<LedgerEntry>              m_ledger;
    std::unordered_set<uint64_t>         m_appliedKeys; // models the ledger's UNIQUE(idemKey)
    uint64_t                             m_lastAppliedSeq{ 0 };
};

// The durable, ordered, append-only outbox. Append stages an event (assigning the
// next seq) in the same logical transaction as the balance change; Drain applies the
// not-yet-confirmed events in order; ConfirmDrained advances the durable watermark
// after the apply commits. A crash before ConfirmDrained re-drains from the old
// watermark — idempotent apply makes that safe.
class Outbox
{
public:
    // Append a new economy event; returns its assigned monotonic seq.
    uint64_t Append(uint64_t idemKey, uint32_t accountId, int64_t amount,
                    EconomyEventType type = EconomyEventType::WalletDelta)
    {
        const uint64_t seq = ++m_nextSeq;
        m_log.push_back({ seq, idemKey, accountId, amount, type });
        return seq;
    }

    [[nodiscard]] size_t TotalEvents() const noexcept { return m_log.size(); }
    [[nodiscard]] uint64_t DrainWatermark() const noexcept { return m_drainWatermark; }

    // Events past the drain watermark (what a drain would attempt).
    [[nodiscard]] size_t PendingCount() const noexcept
    {
        size_t n = 0;
        for (const auto& e : m_log) if (e.seq > m_drainWatermark) ++n;
        return n;
    }

    // Drain pending events (seq > watermark) into 'state', in seq order. Returns the
    // count newly applied. Does NOT advance the watermark — the caller ConfirmDrained
    // after the apply transaction commits (so a crash in between simply re-drains).
    size_t Drain(EconomyState& state)
    {
        size_t applied = 0;
        for (const auto& e : m_log)
            if (e.seq > m_drainWatermark && state.Apply(e)) ++applied;
        return applied;
    }

    // Full replay from the start of the durable log (the warm-restart path / a crash
    // that lost the watermark). Idempotent apply dedupes already-applied events.
    size_t ReplayAll(EconomyState& state)
    {
        size_t applied = 0;
        for (const auto& e : m_log)
            if (state.Apply(e)) ++applied;
        return applied;
    }

    // Advance the durable watermark after a committed drain (the optimization that
    // lets steady-state drains skip already-applied events).
    void ConfirmDrained(uint64_t uptoSeq) noexcept
    {
        if (uptoSeq > m_drainWatermark) m_drainWatermark = uptoSeq;
    }

    [[nodiscard]] uint64_t MaxSeq() const noexcept { return m_nextSeq; }
    [[nodiscard]] const std::vector<EconomyEvent>& Log() const noexcept { return m_log; }

private:
    std::vector<EconomyEvent> m_log;            // durable, ordered, append-only
    uint64_t                  m_nextSeq{ 0 };
    uint64_t                  m_drainWatermark{ 0 };
};

} // namespace Neuron::Persist
