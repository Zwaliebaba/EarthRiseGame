// Fitting + layered-defense ECS tests (Components.h + ServerUniverse; M6 area B) —
// installing a catalog fit gives an entity its hull's layered HP + resists + the
// fitting grid; the base carries the disable-not-destroy state; projectiles and loot
// are snapshot-shaped replicated entities. Component ids are bound in ShapeCatalogTests.

#include "Combat.h"
#include "Command.h"
#include "ServerUniverse.h"
#include "Snapshot.h"
#include "TestRunner.h"

using namespace Neuron::Sim;
using namespace ertest;
using Neuron::Universe::UniversePos;

namespace
{
    // Command 'attacker' (owned by 'owner') to attack 'target'.
    void Attack(ServerUniverse& su, uint32_t owner, uint32_t attacker, uint32_t target)
    {
        FleetCommand c; c.intent = IntentType::Attack; c.units = { attacker }; c.targetNetId = target;
        su.ApplyFleetCommand(owner, c);
    }
}

// --- a fit installs the hull's layered HP + resists + the fitting grid -------

ER_TEST(Fitting, ShipSpawnsWithCatalogLayeredHpResistsAndModules)
{
    ServerUniverse su(false);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t ship = su.SpawnFleetShipFit(base, ServerUniverse::ShipShapeId(), { 0, 0, 0 }, "fighter-em");

    const HullClass* medium = su.Combat().FindHull("hull.medium");
    ER_CHECK(medium != nullptr);

    const DefenseLayers* d = su.DefenseOf(ship);
    ER_CHECK(d != nullptr);
    ER_CHECK_EQ(d->shield.max, medium->shieldHp);                 // base shield from the hull
    ER_CHECK_EQ(d->armor.max,  medium->armorHp + 200);            // +1 armor plate (strength 200)
    ER_CHECK_EQ(d->hull.max,   medium->hullHp);
    ER_CHECK(d->shieldRegenPerSec >= medium->shieldRegenPerSec);  // shield booster adds regen

    // Resist profile installed from the hull (the counter triangle is data-driven).
    const ResistProfile* rp = su.ResistOf(ship);
    ER_CHECK(rp != nullptr);
    ER_CHECK_EQ(rp->At(DefenseLayer::Shield, DamageType::Kinetic), medium->resists.At(DefenseLayer::Shield, DamageType::Kinetic));

    // The fitting grid carries exactly the template's modules within budget.
    const Fitting* f = su.FittingOf(ship);
    ER_CHECK(f != nullptr);
    ER_CHECK_EQ(f->modules.size(), su.Combat().FindFit("fighter-em")->modules.size());
    ER_CHECK(f->pgUsed <= f->pgMax);
    ER_CHECK(f->cpuUsed <= f->cpuMax);

    // The synced Health mirror equals the layered totals (wire/HUD/SimHash read it).
    const Health* h = su.HealthOf(ship);
    ER_CHECK(h != nullptr);
    ER_CHECK_EQ(h->hp, d->TotalCur());
    ER_CHECK_EQ(h->maxHp, d->TotalMax());
}

ER_TEST(Fitting, BaseSpawnsWithCapitalFitAndActiveState)
{
    ServerUniverse su(false);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const DefenseLayers* d = su.DefenseOf(base);
    const BaseCombat*    bc = su.BaseCombatOf(base);
    ER_CHECK(d != nullptr && bc != nullptr);
    ER_CHECK(bc->state == BaseState::Active);
    ER_CHECK(d->hull.max >= su.Combat().FindHull("hull.capital")->hullHp); // capital-class HP
    ER_CHECK(su.FittingOf(base) != nullptr);                               // fire-support fit
}

// --- projectiles + loot are snapshot-shaped replicated entities -------------

ER_TEST(Fitting, ProjectileIsSnapshotShaped)
{
    ServerUniverse su(false);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t mine = su.SpawnFleetShipFit(base, ServerUniverse::ShipShapeId(), { 0, 0, 0 }, "fighter-kin");
    const uint32_t foe  = su.SpawnFleetShipFit(999, ServerUniverse::ShipShapeId(), { 1000, 0, 0 }, "fighter-kin");

    Attack(su, base, mine, foe);
    for (int i = 0; i < 10 && su.ProjectileCount() == 0; ++i) su.Step(1.0f / 30.0f);
    ER_CHECK(su.ProjectileCount() > 0); // railgun (projectile weapon) spawned a shot

    bool sawProjectile = false;
    for (const auto& e : su.BuildSnapshot().entities)
        if (e.kind == EntityKind::Projectile) sawProjectile = true;
    ER_CHECK(sawProjectile); // replicated through the snapshot pipeline (area D)
}

ER_TEST(Fitting, LootContainerIsSnapshotShapedOnKill)
{
    ServerUniverse su(false);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t mine = su.SpawnFleetShipFit(base, ServerUniverse::ShipShapeId(), { 0, 0, 0 }, "fighter-kin");
    const uint32_t foe  = su.SpawnFleetShipFit(999, ServerUniverse::ShipShapeId(), { 600, 0, 0 }, "fighter-kin");
    // Make the foe a one-shot kill so loot drops quickly.
    if (auto* d = su.DefenseOf(foe)) { d->shield.cur = 1; d->armor.cur = 0; d->hull.cur = 1; }
    if (auto* c = su.CargoOf(foe)) c->amount[0] = 100.0f; // something to loot

    Attack(su, base, mine, foe);
    for (int i = 0; i < 60 && su.LootContainerIds().empty(); ++i) su.Step(1.0f / 30.0f);
    ER_CHECK(!su.LootContainerIds().empty()); // a destroyed ship dropped recoverable loot

    bool sawLoot = false;
    for (const auto& e : su.BuildSnapshot().entities)
        if (e.kind == EntityKind::LootContainer) sawLoot = true;
    ER_CHECK(sawLoot);
}
