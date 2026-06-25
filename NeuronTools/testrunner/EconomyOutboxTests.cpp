// Combat-economy → outbox mapping tests (M6 area G → M5 area D, §15). The sim emits
// loot/kill/cargo-loss events; ERServer maps each to a write-through EconomyMutation.
// The property that makes that bridge zero-loss is a STABLE, UNIQUE idempotency key per
// event (so a crash/replay can't double-credit or silently drop one). These tests pin
// that derivation purely (no ODBC), then drive a REAL combat scenario through it to prove
// the actual sim event stream maps to all-distinct keys. Mirrors NeuronCore on the runner.

#include "EconomyOutbox.h"
#include "Combat.h"
#include "Command.h"
#include "ServerUniverse.h"
#include "TestRunner.h"

#include <set>
#include <string_view>
#include <vector>

using namespace Neuron::Sim;
using namespace ertest;

namespace
{
    OutboxRecord Map(EconEventCode t, uint32_t a, uint32_t b, int32_t v, uint32_t tick)
    {
        return MapEconEvent(static_cast<uint8_t>(t), a, b, v, tick);
    }
}

// --- pure mapping --------------------------------------------------------------

ER_TEST(EconomyOutbox, KeyIsDeterministicForTheSameEvent)
{
    // Same event → same key (so a legitimate retry of the *same* event dedupes, never
    // double-credits) — and a different producing tick → a different key (so a recycled
    // net id on a genuinely new event isn't mistaken for a dup and dropped).
    ER_CHECK_EQ(Map(EconEventCode::Killmail, 42, 7, 100, 9).idemKey,
                Map(EconEventCode::Killmail, 42, 7, 100, 9).idemKey);
    ER_CHECK(Map(EconEventCode::Killmail, 42, 7, 100, 9).idemKey
             != Map(EconEventCode::Killmail, 42, 7, 100, 10).idemKey);
}

ER_TEST(EconomyOutbox, DifferentEventFamiliesNeverShareAKey)
{
    // A kill, a cargo-loss and a loot-drop for the SAME victim on the SAME tick must get
    // distinct keys — otherwise the outbox would treat two of them as one and drop it.
    const uint32_t victim = 1234, other = 5;
    const uint32_t tick = 77;
    std::set<uint64_t> keys{
        Map(EconEventCode::LootDrop,  victim, other, 0,  tick).idemKey,
        Map(EconEventCode::LootClaim, victim, other, 50, tick).idemKey,
        Map(EconEventCode::Killmail,  victim, other, 80, tick).idemKey,
        Map(EconEventCode::CargoLost, victim, other, 30, tick).idemKey,
    };
    ER_CHECK_EQ(keys.size(), size_t{ 4 }); // all four distinct
}

ER_TEST(EconomyOutbox, KindsAndRefsMatchTheEventType)
{
    // The kind code maps to the matching Persist::EconomyEventKind value, the refType is
    // the dedupe pair the ledger keys on, and the amount sign is right (cargo loss debits).
    const auto drop  = Map(EconEventCode::LootDrop,  10, 0, 0,  1);
    const auto claim = Map(EconEventCode::LootClaim, 11, 1, 40, 1);
    const auto kill  = Map(EconEventCode::Killmail,  12, 2, 90, 1);
    const auto cargo = Map(EconEventCode::CargoLost, 13, 0, 30, 1);

    ER_CHECK_EQ(drop.kind,  static_cast<uint8_t>(OutboxKind::LootDrop));
    ER_CHECK_EQ(claim.kind, static_cast<uint8_t>(OutboxKind::LootClaim));
    ER_CHECK_EQ(kill.kind,  static_cast<uint8_t>(OutboxKind::Killmail));
    ER_CHECK_EQ(cargo.kind, static_cast<uint8_t>(OutboxKind::CargoLost));

    ER_CHECK(std::string_view(claim.refType) == "Loot");
    ER_CHECK(std::string_view(kill.refType)  == "Kill");
    ER_CHECK(std::string_view(cargo.refType) == "Cargo");

    ER_CHECK_EQ(claim.amount, int64_t{ 40 });   // claim credits the looter
    ER_CHECK_EQ(cargo.amount, int64_t{ -30 });  // destroyed cargo is a debit
    ER_CHECK_EQ(claim.refId,  int64_t{ 11 });   // refId identifies the source entity
}

ER_TEST(EconomyOutbox, DistinctEntitiesGetDistinctKeys)
{
    // Two kills on the same tick but different victims must not collide.
    std::set<uint64_t> keys;
    for (uint32_t v = 1; v <= 200; ++v) keys.insert(Map(EconEventCode::Killmail, v, 0, 10, 5).idemKey);
    ER_CHECK_EQ(keys.size(), size_t{ 200 });
}

// --- real sim event stream → all-distinct outbox keys --------------------------

ER_TEST(EconomyOutbox, RealKillStreamMapsToUniqueKeys)
{
    // Drive an actual ship death (loot drop + killmail + cargo loss), then claim the loot,
    // and confirm EVERY drained economy event maps to a distinct idemKey — i.e. the real
    // bridge ERServer runs produces no key collisions that would drop a row.
    ServerUniverse su(false);
    const uint16_t shipShape = ServerUniverse::ShipShapeId();
    const uint32_t mine = su.SpawnFleetShipFit(1, shipShape, { 0, 0, 0 }, "fighter-kin");
    const uint32_t foe  = su.SpawnFleetShipFit(2, shipShape, { 800, 0, 0 }, "fighter-kin");
    if (auto* d = su.DefenseOf(foe)) { d->shield.cur = 1; d->armor.cur = 0; d->hull.cur = 1; }
    if (auto* c = su.CargoOf(foe))   c->amount[0] = 100.0f;

    std::vector<OutboxRecord> recs;
    auto drain = [&](){
        for (const auto& e : su.DrainEconEvents())
            recs.push_back(MapEconEvent(static_cast<uint8_t>(e.type), e.aNetId, e.bNetId, e.value, e.tick));
    };

    for (int i = 0; i < 90 && su.LootContainerIds().empty(); ++i) {
        FleetCommand c; c.intent = IntentType::Attack; c.units = { mine }; c.targetNetId = foe;
        su.ApplyFleetCommand(1, c);
        su.Step(1.0f / 30.0f);
        drain();
    }
    ER_CHECK_EQ(su.LootContainerIds().size(), size_t{ 1 });
    if (!su.LootContainerIds().empty()) su.ClaimLoot(mine, su.LootContainerIds().front());
    drain();

    ER_CHECK(recs.size() >= 3); // at least a drop, a killmail and a claim
    std::set<uint64_t> keys;
    for (const auto& r : recs) {
        keys.insert(r.idemKey);
        ER_CHECK(r.kind != 0); // every event mapped to a real kind
    }
    ER_CHECK_EQ(keys.size(), recs.size()); // no collisions across the real stream
}
