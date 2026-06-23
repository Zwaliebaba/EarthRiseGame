// Weapons & projectile tests (Combat.h + ServerUniverse; M6 area D) — the anti-
// tunneling sub-stepping (the headline assertion), the engagement-range gate, weapon
// cadence, and that a projectile hit routes typed damage through the area-C model.
// Component ids are bound in ShapeCatalogTests.

#include "Combat.h"
#include "CombatData.h"
#include "Command.h"
#include "ServerUniverse.h"
#include "TestRunner.h"

using namespace Neuron::Sim;
using namespace ertest;
using Neuron::Universe::UniversePos;

namespace
{
    void Attack(ServerUniverse& su, uint32_t owner, uint32_t attacker, uint32_t target)
    {
        FleetCommand c; c.intent = IntentType::Attack; c.units = { attacker }; c.targetNetId = target;
        su.ApplyFleetCommand(owner, c);
    }
}

// --- the headline: sub-stepping prevents tunneling (area D) ------------------

ER_TEST(Projectile, SubSteppingPreventsTunneling)
{
    const UniversePos target{ 1000, 0, 0 };
    const DirectX::XMFLOAT3 vel{ 50000.0f, 0.0f, 0.0f }; // ~1667 m per 1/30 s tick
    const double hitRadius = 50.0;

    // Without sub-stepping (1 step/tick) the shot leaps from before to past the small
    // target in one tick — a classic tunnel/miss.
    {
        UniversePos p{ 0, 0, 0 }; float ttl = 1.0f;
        const SubStepResult r = StepProjectile(p, vel, target, target, ttl, 1.0f / 30.0f, hitRadius, 1);
        ER_CHECK(!r.hit);
    }
    // With sub-stepping an intermediate sub-step lands within the hit radius → hit.
    {
        UniversePos p{ 0, 0, 0 }; float ttl = 1.0f;
        const SubStepResult r = StepProjectile(p, vel, target, target, ttl, 1.0f / 30.0f, hitRadius, 8);
        ER_CHECK(r.hit);
    }
}

ER_TEST(Projectile, EngagementRangeGate)
{
    const ModuleDef w = *DefaultCombatCatalog().FindModule("module.railgun.t1");
    ER_CHECK(InEngagementRange(w, 1000.0, 1.0f));                                  // inside reach
    ER_CHECK(InEngagementRange(w, static_cast<double>(w.optimal), 1.0f));          // at optimal
    ER_CHECK(!InEngagementRange(w, static_cast<double>(w.optimal + w.falloff) + 1.0, 1.0f)); // just past reach
    ER_CHECK(!InEngagementRange(w, 99999.0, 1.0f));                                // far out
    // A sensor damp (factor < 1) on the shooter shrinks the engagement reach.
    ER_CHECK(!InEngagementRange(w, static_cast<double>(w.optimal + w.falloff) * 0.9, 0.5f));
}

// --- live firing: cadence gating + typed damage on hit ----------------------

ER_TEST(Projectile, WeaponCadenceGatesFireRate)
{
    ServerUniverse su(false);
    const uint32_t mine = su.SpawnFleetShipFit(1, ServerUniverse::ShipShapeId(), { 0, 0, 0 }, "fighter-kin");
    const uint32_t foe  = su.SpawnFleetShipFit(2, ServerUniverse::ShipShapeId(), { 2000, 0, 0 }, "fighter-kin");
    if (auto* d = su.DefenseOf(foe)) { d->shield.cur = d->shield.max = 1000000; } // unkillable dummy

    Attack(su, 1, mine, foe);
    su.Step(1.0f / 30.0f);                                  // first volley
    const size_t afterVolley = su.ProjectileCount();
    ER_CHECK(afterVolley >= 1);
    ER_CHECK(afterVolley <= 3);                             // ≤ 3 railguns, NOT per-tick spam
    su.Step(1.0f / 30.0f); su.Step(1.0f / 30.0f);          // 2 more ticks, far inside the 1 s cadence
    ER_CHECK(su.ProjectileCount() <= afterVolley);          // no new shots (cooldown-gated)
}

ER_TEST(Projectile, ProjectileHitDealsTypedDamageThroughLayers)
{
    ServerUniverse su(false);
    const uint32_t mine = su.SpawnFleetShipFit(1, ServerUniverse::ShipShapeId(), { 0, 0, 0 }, "fighter-kin"); // kinetic railguns
    const uint32_t foe  = su.SpawnFleetShipFit(2, ServerUniverse::ShipShapeId(), { 1500, 0, 0 }, "fighter-kin");
    const int32_t hp0 = su.DefenseOf(foe)->TotalCur();

    Attack(su, 1, mine, foe);
    for (int i = 0; i < 60; ++i) su.Step(1.0f / 30.0f); // 2 s — projectiles cross + hit
    ER_CHECK(su.DefenseOf(foe)->TotalCur() < hp0);       // damage landed via projectiles → area C
}

ER_TEST(Projectile, FiringIsDeterministic)
{
    auto run = [] {
        ServerUniverse su(false);
        const uint32_t a = su.SpawnFleetShipFit(1, ServerUniverse::ShipShapeId(), { 0, 0, 0 }, "fighter-kin");
        const uint32_t b = su.SpawnFleetShipFit(2, ServerUniverse::ShipShapeId(), { 1500, 0, 0 }, "fighter-kin");
        FleetCommand c; c.intent = IntentType::Attack; c.units = { a }; c.targetNetId = b;
        su.ApplyFleetCommand(1, c);
        for (int i = 0; i < 40; ++i) su.Step(1.0f / 30.0f);
        return su.SimHash();
    };
    ER_CHECK_EQ(run(), run()); // identical inputs → identical sim (projectiles included)
}
