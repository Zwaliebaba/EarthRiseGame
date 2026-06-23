// PvE-AI + loot/killmail + disable-not-destroy scenario tests (ServerUniverse; M6
// areas F & G) — NPCs that fight with real fits and target priority, ship death that
// drops recoverable loot and logs a killmail (economy events for the M5 outbox), and
// the base capital that retreats at low hull and is NEVER destroyed (§13.1). Component
// ids are bound in ShapeCatalogTests.

#include "Combat.h"
#include "Command.h"
#include "ServerUniverse.h"
#include "WarmRestart.h"
#include "TestRunner.h"

#include <algorithm>

using namespace Neuron::Sim;
using namespace ertest;
using Neuron::Universe::UniversePos;

namespace
{
    uint16_t Ship() { return ServerUniverse::ShipShapeId(); }
    void Attack(ServerUniverse& su, uint32_t owner, uint32_t attacker, uint32_t target)
    {
        FleetCommand c; c.intent = IntentType::Attack; c.units = { attacker }; c.targetNetId = target;
        su.ApplyFleetCommand(owner, c);
    }
}

// --- area F: NPC AI fights with the real model ------------------------------

ER_TEST(CombatScenario, NpcTargetPriorityPicksLogiFirst)
{
    // An NPC facing a player fighter + a player logi should PRIMARY the logi (§4).
    ServerUniverse su(false);
    const uint16_t site = su.SpawnNpcSite({ 0, 0, 0 }, 1, 50.0f, "fighter-kin");
    const uint32_t npc  = su.NpcSiteMembers(site).front();
    const uint32_t fighter = su.SpawnFleetShipFit(1, Ship(), { 1500, 0, 0 }, "fighter-kin");
    const uint32_t logi    = su.SpawnFleetShipFit(1, Ship(), { 1500, 500, 0 }, "logi-shield");
    (void)fighter;

    su.Step(1.0f / 30.0f);
    ER_CHECK(su.NpcAiOf(npc)->state == AiState::Aggro);
    ER_CHECK_EQ(su.NpcAiOf(npc)->targetNetId, logi); // primaried the logi, not the fighter
}

ER_TEST(CombatScenario, FittedNpcDiesThroughThreeLayers)
{
    // A player fleet kills a fitted NPC by depleting shield→armor→hull (not a flat bar):
    // along the way the layers fall in order, and the kill logs exactly one killmail.
    ServerUniverse su(false);
    const uint16_t site = su.SpawnNpcSite({ 1500, 0, 0 }, 1, 50.0f, "fighter-kin");
    const uint32_t npc  = su.NpcSiteMembers(site).front();
    std::vector<uint32_t> fleet;
    for (int i = 0; i < 4; ++i) fleet.push_back(su.SpawnFleetShipFit(1, Ship(), { 0, static_cast<int64_t>(i) * 100, 0 }, "fighter-kin"));

    bool sawShieldGone = false, sawArmorGone = false;
    for (int i = 0; i < 1200 && !su.IsNpcSiteCleared(site); ++i) {
        FleetCommand c; c.intent = IntentType::Attack; c.units = fleet; c.targetNetId = npc;
        su.ApplyFleetCommand(1, c);
        su.Step(1.0f / 30.0f);
        if (const DefenseLayers* d = su.DefenseOf(npc)) {
            if (d->shield.cur == 0) sawShieldGone = true;
            if (sawShieldGone && d->armor.cur == 0) sawArmorGone = true;
        }
    }
    ER_CHECK(su.IsNpcSiteCleared(site));          // the NPC died
    ER_CHECK(sawShieldGone && sawArmorGone);       // through three layers, in order
    ER_CHECK_EQ(su.DrainKillmails().size(), size_t{ 1 }); // exactly one killmail logged
}

ER_TEST(CombatScenario, NpcLogiKeepsWingAliveUntilPrimaried)
{
    // A fighter NPC backed by an allied NPC logi survives a player's fire longer than
    // the same fighter alone (the NPC logi reps its wing — the sustain loop, area E/F).
    auto fighterHpAfter = [](bool withLogi) {
        ServerUniverse su(false);
        const uint16_t fSite = su.SpawnNpcSite({ 1200, 0, 0 }, 1, 50.0f, "fighter-kin");
        const uint32_t fighter = su.NpcSiteMembers(fSite).front();
        if (withLogi) su.SpawnNpcSite({ 1200, 300, 0 }, 1, 50.0f, "logi-shield"); // allied NPC logi
        const uint32_t attacker = su.SpawnFleetShipFit(1, Ship(), { 0, 0, 0 }, "fighter-kin");
        for (int i = 0; i < 90; ++i) { Attack(su, 1, attacker, fighter); su.Step(1.0f / 30.0f); }
        const DefenseLayers* d = su.DefenseOf(fighter);
        return d ? d->TotalCur() : 0;
    };
    ER_CHECK(fighterHpAfter(true) > fighterHpAfter(false)); // logi-backed wing survives longer
}

// --- area G: loot-on-kill + killmail ----------------------------------------

ER_TEST(CombatScenario, ShipDeathDropsLootAndEmitsEconomyEvents)
{
    ServerUniverse su(false);
    const uint32_t mine = su.SpawnFleetShipFit(1, Ship(), { 0, 0, 0 }, "fighter-kin");
    const uint32_t foe  = su.SpawnFleetShipFit(2, Ship(), { 800, 0, 0 }, "fighter-kin");
    if (auto* d = su.DefenseOf(foe)) { d->shield.cur = 1; d->armor.cur = 0; d->hull.cur = 1; }
    if (auto* c = su.CargoOf(foe))   c->amount[0] = 100.0f; // cargo to drop

    for (int i = 0; i < 90 && su.LootContainerIds().empty(); ++i) { Attack(su, 1, mine, foe); su.Step(1.0f / 30.0f); }

    // A loot container dropped, exactly one killmail, and the loot + kill rode the
    // economy-event stream (→ the M5 write-through outbox, zero-loss §15).
    ER_CHECK_EQ(su.LootContainerIds().size(), size_t{ 1 });
    ER_CHECK_EQ(su.DrainKillmails().size(), size_t{ 1 });
    const auto events = su.DrainEconEvents();
    int drops = 0, kills = 0;
    for (const auto& e : events) {
        if (e.type == ServerUniverse::EconEventType::LootDrop) ++drops;
        if (e.type == ServerUniverse::EconEventType::Killmail) ++kills;
    }
    ER_CHECK_EQ(drops, 1);
    ER_CHECK_EQ(kills, 1);
}

ER_TEST(CombatScenario, ClaimingLootTransfersCargoAndEmitsOneEvent)
{
    ServerUniverse su(false);
    const uint32_t mine = su.SpawnFleetShipFit(1, Ship(), { 0, 0, 0 }, "fighter-kin");
    const uint32_t foe  = su.SpawnFleetShipFit(2, Ship(), { 800, 0, 0 }, "fighter-kin");
    if (auto* d = su.DefenseOf(foe)) { d->shield.cur = 1; d->armor.cur = 0; d->hull.cur = 1; }
    if (auto* c = su.CargoOf(foe))   c->amount[0] = 100.0f;
    for (int i = 0; i < 90 && su.LootContainerIds().empty(); ++i) { Attack(su, 1, mine, foe); su.Step(1.0f / 30.0f); }
    ER_CHECK_EQ(su.LootContainerIds().size(), size_t{ 1 });
    (void)su.DrainEconEvents(); // clear the drop event

    const uint32_t container = su.LootContainerIds().front();
    const float before = su.CargoOf(mine)->amount[0];
    ER_CHECK(su.ClaimLoot(mine, container));
    ER_CHECK(su.CargoOf(mine)->amount[0] > before);   // items moved into the looter's cargo
    ER_CHECK(su.LootContainerIds().empty());          // container consumed
    const auto events = su.DrainEconEvents();
    int claims = 0; for (const auto& e : events) if (e.type == ServerUniverse::EconEventType::LootClaim) ++claims;
    ER_CHECK_EQ(claims, 1);                            // exactly one claim event (zero-loss)
}

// --- area G: base disable-not-destroy (§13.1) -------------------------------

ER_TEST(CombatScenario, BaseRetreatsAtLowHullAndIsNeverDestroyed)
{
    ServerUniverse su(false);
    su.SetBaseRetreatSeconds(5.0f); // short cooldown for the test
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    su.StorageOf(base)->amount[0] = 1000.0f; // cargo to lose on the emergency jump

    // Drive the hull into the danger band (simulating sustained incoming damage).
    DefenseLayers* d = su.DefenseOf(base);
    d->hull.cur = d->hull.max / 20; // 5% hull → below the retreat threshold

    su.Step(1.0f); // the retreat trigger fires this tick
    ER_CHECK(su.BaseCombatOf(base)->state == BaseState::Retreating);
    ER_CHECK_EQ(su.StorageOf(base)->amount[0], 0.0f);        // cargo lost on the jump
    const auto alerts = su.DrainLowHullAlerts();
    ER_CHECK(std::find(alerts.begin(), alerts.end(), base) != alerts.end()); // area H alert raised

    // The cooldown elapses → Disabled; the base is still present (never destroyed).
    for (int i = 0; i < 8; ++i) su.Step(1.0f);
    ER_CHECK(su.BaseCombatOf(base)->state == BaseState::Disabled);
    ER_CHECK(su.DefenseOf(base) != nullptr);                 // the capital is NOT removed

    // The disable-not-destroy state survives a warm restart (M5 F).
    const Neuron::Persist::PersistState s = su.CaptureState();
    ServerUniverse restored(false);
    restored.RestoreState(s);
    ER_CHECK(restored.BaseCombatOf(base)->state == BaseState::Disabled);
}
