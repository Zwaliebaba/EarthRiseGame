// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// EconomyStore.cpp — write-through outbox implementation (M5 area D, §15).

#include "pch.h"
#include "EconomyStore.h"

#include <cstring>
#include <string>

namespace Neuron::Persist
{

const char* EconomyStore::EventTypeName(EconomyEventKind k) noexcept
{
    switch (k) {
    case EconomyEventKind::WalletDelta:    return "wallet_delta";
    case EconomyEventKind::BuildComplete:  return "build_complete";
    case EconomyEventKind::InventoryDelta: return "inventory_delta";
    case EconomyEventKind::LootClaim:      return "loot_claimed";
    }
    return "wallet_delta";
}

namespace
{

// Minimal JSON payload for EconomyOutbox.Payload (NVARCHAR(MAX)). Mirrors the event so
// a drain/replay (or M6 consumers) can reconstruct it. Hand-built to avoid a JSON dep;
// values are integers + a short reason string (escaped conservatively).
std::string EscapeJson(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) out += ' ';
            else out += c;
        }
    }
    return out;
}

std::string MakePayload(const EconomyMutation& m)
{
    std::string p = "{";
    p += "\"accountId\":" + std::to_string(m.accountId);
    p += ",\"amount\":" + std::to_string(m.amount);
    p += ",\"reason\":\"" + EscapeJson(m.reason) + "\"";
    if (!m.refType.empty()) {
        p += ",\"refType\":\"" + EscapeJson(m.refType) + "\"";
        p += ",\"refId\":" + std::to_string(m.refId);
    }
    p += "}";
    return p;
}

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
EconomyStore::AppendWriteThrough(const EconomyMutation& m, int64_t /*nowUnix*/)
{
    auto lease = m_pool->Acquire();
    if (!lease)
        return std::nullopt;

    // The whole point (zero-loss): outbox row + wallet balance + ledger row in ONE
    // transaction. If any statement fails, the whole thing rolls back — the event is
    // not authoritative (Outbox.h EconomyState::Apply all-or-nothing).
    if (!lease->BeginTransaction())
        return std::nullopt;

    const std::string payload = MakePayload(m);
    const int64_t idem = static_cast<int64_t>(m.idemKey);

    // 1) Outbox row carrying the IdempotencyKey. The UNIQUE filtered index
    //    (UX_EconomyOutbox_IdemKey, migration 004) is what makes replay exactly-once:
    //    a duplicate key collides here and we treat it as "already applied".
    const SqlParam ob[] = {
        SqlParam::Make(std::string_view(EventTypeName(m.kind))),
        SqlParam::Make(std::string_view(payload)),
        SqlParam::Make(idem),
    };
    auto outboxId = lease->ExecInsertReturningIdentity(
        "INSERT INTO EconomyOutbox (EventType, Payload, IdempotencyKey) VALUES (?, ?, ?)",
        ob);

    if (!outboxId) {
        // A duplicate idemKey means this exact event was already committed — idempotent
        // no-op (no double-credit). Roll back and return the EXISTING OutboxId so the
        // caller treats it as success (mirrors Outbox::Apply returning false → skip).
        if (lease->LastError().IsUniqueViolation()) {
            // Roll back; the lease returns to autocommit and stays connected, so reuse
            // it (do NOT acquire a 2nd lease — that would deadlock a poolMax==1 pool).
            (void)lease->Rollback();
            const SqlParam q[] = { SqlParam::Make(idem) };
            if (auto r = lease->ExecQuery(
                    "SELECT OutboxId FROM EconomyOutbox WHERE IdempotencyKey = ?", q, 1)) {
                if (!r->Empty() && !r->rows.front().empty())
                    return ReadI64(r->rows.front().front());
            }
            return std::nullopt;
        }
        (void)lease->Rollback();
        return std::nullopt;
    }

    // 2) Wallet balance change. CK_Wallets_NonNeg guards against an over-debit; a
    //    violation rolls the whole event back (the debit was never authoritative).
    {
        const SqlParam w[] = { SqlParam::Make(m.amount), SqlParam::Make(m.accountId) };
        auto n = lease->ExecNonQuery(
            "UPDATE Wallets SET Balance = Balance + ?, UpdatedAt = SYSUTCDATETIME() "
            "WHERE AccountId = ?", w);
        if (!n || *n == 0) { // 0 rows → no wallet (or the CHECK rejected it)
            (void)lease->Rollback();
            return std::nullopt;
        }
    }

    // 3) Append-only CurrencyLedger row with BalanceAfter snapshot (anti-dupe audit).
    //    UX_CurrencyLedger_Ref (migration 004) dedupes a replayed (RefType,RefId) as
    //    defence in depth behind the outbox idemKey.
    {
        const SqlParam led[] = {
            SqlParam::Make(m.accountId),
            SqlParam::Make(m.amount),
            SqlParam::Make(m.accountId), // for the BalanceAfter subquery below
            SqlParam::Make(std::string_view(m.reason.empty() ? "economy" : m.reason)),
            m.refType.empty() ? SqlParam::MakeNull() : SqlParam::Make(std::string_view(m.refType)),
            m.refType.empty() ? SqlParam::MakeNull() : SqlParam::Make(m.refId),
        };
        auto n = lease->ExecNonQuery(
            "INSERT INTO CurrencyLedger (AccountId, DeltaCredits, BalanceAfter, Reason, RefType, RefId) "
            "VALUES (?, ?, (SELECT Balance FROM Wallets WHERE AccountId = ?), ?, ?, ?)",
            led);
        if (!n) {
            // A duplicate (RefType,RefId) is the ledger's own idempotency net — roll
            // back this event as already-applied (consistent with the outbox dedupe).
            (void)lease->Rollback();
            return std::nullopt;
        }
    }

    if (!lease->Commit()) {
        (void)lease->Rollback();
        return std::nullopt;
    }
    return outboxId;
}

bool EconomyStore::MarkProcessed(int64_t outboxId, int64_t /*nowUnix*/)
{
    auto lease = m_pool->Acquire();
    if (!lease)
        return false;
    const SqlParam p[] = { SqlParam::Make(outboxId) };
    auto n = lease->ExecNonQuery(
        "UPDATE EconomyOutbox SET ProcessedAt = SYSUTCDATETIME() "
        "WHERE OutboxId = ? AND ProcessedAt IS NULL", p);
    return n.has_value();
}

std::optional<std::vector<OutboxRow>> EconomyStore::ReadUnprocessed(int maxRows)
{
    auto lease = m_pool->Acquire();
    if (!lease)
        return std::nullopt;
    const SqlParam p[] = { SqlParam::Make(static_cast<int64_t>(maxRows)) };
    auto res = lease->ExecQuery(
        "SELECT TOP (?) OutboxId, ISNULL(IdempotencyKey, 0), EventType, Payload "
        "FROM EconomyOutbox WHERE ProcessedAt IS NULL ORDER BY OutboxId ASC", p, 4);
    if (!res)
        return std::nullopt;

    std::vector<OutboxRow> rows;
    rows.reserve(res->rows.size());
    for (const auto& r : res->rows) {
        OutboxRow o;
        o.outboxId  = ReadI64(r.size() > 0 ? r[0] : SqlValue{});
        o.idemKey   = static_cast<uint64_t>(ReadI64(r.size() > 1 ? r[1] : SqlValue{}));
        if (r.size() > 2 && !r[2].isNull) o.eventType.assign(r[2].bytes.begin(), r[2].bytes.end());
        if (r.size() > 3 && !r[3].isNull) o.payload.assign(r[3].bytes.begin(), r[3].bytes.end());
        rows.push_back(std::move(o));
    }
    return rows;
}

std::optional<std::vector<OutboxRow>> EconomyStore::ReadSince(int64_t watermarkOutboxId, int maxRows)
{
    auto lease = m_pool->Acquire();
    if (!lease)
        return std::nullopt;
    const SqlParam p[] = { SqlParam::Make(static_cast<int64_t>(maxRows)), SqlParam::Make(watermarkOutboxId) };
    auto res = lease->ExecQuery(
        "SELECT TOP (?) OutboxId, ISNULL(IdempotencyKey, 0), EventType, Payload "
        "FROM EconomyOutbox WHERE OutboxId > ? ORDER BY OutboxId ASC", p, 4);
    if (!res)
        return std::nullopt;

    std::vector<OutboxRow> rows;
    rows.reserve(res->rows.size());
    for (const auto& r : res->rows) {
        OutboxRow o;
        o.outboxId  = ReadI64(r.size() > 0 ? r[0] : SqlValue{});
        o.idemKey   = static_cast<uint64_t>(ReadI64(r.size() > 1 ? r[1] : SqlValue{}));
        if (r.size() > 2 && !r[2].isNull) o.eventType.assign(r[2].bytes.begin(), r[2].bytes.end());
        if (r.size() > 3 && !r[3].isNull) o.payload.assign(r[3].bytes.begin(), r[3].bytes.end());
        rows.push_back(std::move(o));
    }
    return rows;
}

std::optional<int64_t> EconomyStore::WalletBalance(int64_t accountId)
{
    auto lease = m_pool->Acquire();
    if (!lease)
        return std::nullopt;
    const SqlParam p[] = { SqlParam::Make(accountId) };
    auto res = lease->ExecQuery("SELECT Balance FROM Wallets WHERE AccountId = ?", p, 1);
    if (!res || res->Empty() || res->rows.front().empty())
        return std::nullopt;
    return ReadI64(res->rows.front().front());
}

std::optional<int64_t> EconomyStore::OutboxDepth()
{
    auto lease = m_pool->Acquire();
    if (!lease)
        return std::nullopt;
    auto res = lease->ExecQuery(
        "SELECT COUNT_BIG(*) FROM EconomyOutbox WHERE ProcessedAt IS NULL", {}, 1);
    if (!res || res->Empty() || res->rows.front().empty())
        return std::nullopt;
    return ReadI64(res->rows.front().front());
}

std::optional<int64_t> EconomyStore::MaxOutboxId()
{
    auto lease = m_pool->Acquire();
    if (!lease)
        return std::nullopt;
    auto res = lease->ExecQuery("SELECT ISNULL(MAX(OutboxId), 0) FROM EconomyOutbox", {}, 1);
    if (!res || res->Empty() || res->rows.front().empty())
        return std::nullopt;
    return ReadI64(res->rows.front().front());
}

} // namespace Neuron::Persist
