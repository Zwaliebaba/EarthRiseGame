#pragma once
// EconomyOutbox.h — pure mapping from sim combat-economy events to write-through
// outbox records (M6 area G → M5 area D, §15).
//
// The sim (ServerUniverse) emits loot/kill/cargo-loss events each tick; ERServer turns
// each into a Persist::EconomyMutation enqueued to the persistence thread (SQL never
// touches the tick, §9). That bridge previously did not exist — the sim drained nowhere,
// so loot/kills were not persisted and the "zero economy loss" guarantee (§15) did not
// cover combat. Keeping the derivation pure *here* means the property that actually makes
// it zero-loss — a **stable, unique idempotency key per event**, so a crash/replay can't
// double-credit or silently drop one (Outbox idempotency, migration 004) — is unit-tested
// on the Linux runner (EconomyOutboxTests, §16.2), not just asserted in the Windows-only
// ERServer loop. ERServer copies these portable fields into an EconomyMutation (whose
// ODBC header can't compile off-Windows), so the persistence-layer coupling stays thin.

#include <cstdint>

namespace Neuron::Sim
{

// Ordinal of ServerUniverse::EconEventType — the sim passes static_cast<uint8_t>(type).
// Mirrored here so the mapping needs no dependency on the (heavy) ServerUniverse header.
enum class EconEventCode : uint8_t { LootDrop = 0, LootClaim = 1, Killmail = 2, CargoLost = 3 };

// Outbox event-kind codes. MUST stay in lockstep with ERServer Persist::EconomyEventKind
// (a compile-time check in ERServer pins them together).
enum class OutboxKind : uint8_t
{
    BuildComplete = 1,
    LootClaim     = 3,
    Killmail      = 4,
    CargoLost     = 5,
    LootDrop      = 6,
};

// A portable, persistence-agnostic outbox record. ERServer copies these fields into a
// Persist::EconomyMutation. 'reason'/'refType' are string literals (static storage), so
// the const char* members are safe to hold and copy.
struct OutboxRecord
{
    uint64_t    idemKey{ 0 };   // exactly-once key (CurrencyLedger dedupe, migration 004)
    uint8_t     kind{ 0 };      // OutboxKind value
    int64_t     amount{ 0 };    // signed credit delta (placeholder until M7 balance)
    int64_t     refId{ 0 };     // CurrencyLedger.RefId (+ refType → dedupe pair)
    const char* reason{ "" };   // CurrencyLedger.Reason
    const char* refType{ "" };  // CurrencyLedger.RefType
};

// splitmix64 finalizer over (domain, netId, tick) → a well-distributed 64-bit key.
// Folding the source tick in (not just the net id) keeps keys unique even if the ECS
// recycles a net id later (generation handles, §7.2) — otherwise a reused id could
// collide with a past event's key and be dropped as a dup. 'domain' separates the event
// families so a kill and a cargo-loss for the *same* victim never share a key.
[[nodiscard]] inline uint64_t OutboxIdemKey(uint64_t domain, uint32_t netId, uint32_t tick) noexcept
{
    uint64_t z = (domain * 0x9E3779B97F4A7C15ull)
               ^ (static_cast<uint64_t>(netId) << 21)
               ^  static_cast<uint64_t>(tick);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Map one sim economy event (its primitive fields) to an outbox record. 'aNetId' is the
// event's identifying entity (loot container / victim), 'bNetId' the counterparty
// (claimer / killer), 'value' the gross amount, 'tick' the producing sim tick.
[[nodiscard]] inline OutboxRecord MapEconEvent(uint8_t  typeCode,
                                               uint32_t aNetId,
                                               uint32_t bNetId,
                                               int32_t  value,
                                               uint32_t tick) noexcept
{
    (void)bNetId; // identifies the counterparty for later account resolution; unused here
    OutboxRecord r;
    switch (static_cast<EconEventCode>(typeCode))
    {
    case EconEventCode::LootDrop:
        r.kind   = static_cast<uint8_t>(OutboxKind::LootDrop);
        r.reason = "loot_drop";  r.refType = "Loot"; r.refId = static_cast<int64_t>(aNetId);
        r.amount = 0; // a drop is an inventory event, not a wallet credit (claim credits)
        r.idemKey = OutboxIdemKey(1, aNetId, tick);
        break;
    case EconEventCode::LootClaim:
        r.kind   = static_cast<uint8_t>(OutboxKind::LootClaim);
        r.reason = "loot_claimed"; r.refType = "Loot"; r.refId = static_cast<int64_t>(aNetId);
        r.amount = static_cast<int64_t>(value);
        r.idemKey = OutboxIdemKey(2, aNetId, tick);
        break;
    case EconEventCode::Killmail:
        r.kind   = static_cast<uint8_t>(OutboxKind::Killmail);
        r.reason = "bounty"; r.refType = "Kill"; r.refId = static_cast<int64_t>(aNetId);
        r.amount = static_cast<int64_t>(value); // bounty to the killer (M7 tunes the rate)
        r.idemKey = OutboxIdemKey(3, aNetId, tick);
        break;
    case EconEventCode::CargoLost:
        r.kind   = static_cast<uint8_t>(OutboxKind::CargoLost);
        r.reason = "cargo_lost"; r.refType = "Cargo"; r.refId = static_cast<int64_t>(aNetId);
        r.amount = -static_cast<int64_t>(value); // destroyed cargo is a debit
        r.idemKey = OutboxIdemKey(4, aNetId, tick);
        break;
    }
    return r;
}

} // namespace Neuron::Sim
