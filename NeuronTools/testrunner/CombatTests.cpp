// Combat sim-rule tests (Combat.h; M6 area C) — the pure, deterministic damage math:
// the resist counter triangle, outside-in layer depletion, shield regen (armor/hull
// none), remote rep, and the tracking/falloff range factors. Platform-independent
// (no ECS), so the Linux runner covers them; mirrors the NeuronCoreTest area-C cases.

#include "Combat.h"
#include "CombatData.h"
#include "TestRunner.h"

using namespace Neuron::Sim;
using namespace ertest;

namespace
{
    // The launch resist spreads (combat-balance.md §2.2) from the authored catalog.
    ResistProfile Resists() { return DefaultCombatCatalog().FindHull("hull.medium")->resists; }

    DefenseLayers MakeLayers(int32_t sh, int32_t ar, int32_t hu, float regen = 0.0f)
    {
        DefenseLayers d;
        d.shield = { sh, sh }; d.armor = { ar, ar }; d.hull = { hu, hu };
        d.shieldRegenPerSec = regen;
        return d;
    }

    // Damage actually removed by one typed hit of 'base' against a fresh target.
    int32_t HitDamage(int32_t sh, int32_t ar, int32_t hu, DamageType t, float base)
    {
        DefenseLayers d = MakeLayers(sh, ar, hu);
        const int32_t before = d.TotalCur();
        (void)ApplyDamage(d, Resists(), t, base, 1.0f, 1.0f);
        return before - d.TotalCur();
    }
}

// --- the resist counter triangle (combat-balance.md §2.2) -------------------

ER_TEST(Combat, EmBeatsShieldKineticWeakVsShield)
{
    // Against a pure shield tank: EM (0 resist) out-damages Kinetic (0.40 resist),
    // and Kinetic is the weakest of the three — the shield-tank counter.
    const int32_t em  = HitDamage(1000, 0, 0, DamageType::EM, 100.0f);
    const int32_t kin = HitDamage(1000, 0, 0, DamageType::Kinetic, 100.0f);
    const int32_t the = HitDamage(1000, 0, 0, DamageType::Thermal, 100.0f);
    ER_CHECK(em > the);
    ER_CHECK(the > kin); // kinetic is the shield's strong resist → least damage
}

ER_TEST(Combat, ThermalBeatsArmor)
{
    // Against a pure armor tank: Thermal (0 resist) out-damages EM/Kinetic (resisted).
    const int32_t the = HitDamage(0, 1000, 0, DamageType::Thermal, 100.0f);
    const int32_t em  = HitDamage(0, 1000, 0, DamageType::EM, 100.0f);
    const int32_t kin = HitDamage(0, 1000, 0, DamageType::Kinetic, 100.0f);
    ER_CHECK(the > em);
    ER_CHECK(the > kin);
}

ER_TEST(Combat, HullIsFlatAcrossTypes)
{
    // Hull is flat (no resists): every damage type lands equally.
    ER_CHECK_EQ(HitDamage(0, 0, 1000, DamageType::EM, 100.0f), HitDamage(0, 0, 1000, DamageType::Kinetic, 100.0f));
    ER_CHECK_EQ(HitDamage(0, 0, 1000, DamageType::Thermal, 100.0f), HitDamage(0, 0, 1000, DamageType::Kinetic, 100.0f));
}

// --- outside-in depletion + regen / rep (§2.1) ------------------------------

ER_TEST(Combat, DepletesShieldThenArmorThenHull)
{
    DefenseLayers d = MakeLayers(50, 50, 50);
    // A big EM hit: shield (0 EM resist) takes it first, overflow into armor (0.40 EM
    // resist) then hull. The outcome reports the outermost layer broken.
    const DamageOutcome o = ApplyDamage(d, Resists(), DamageType::EM, 60.0f, 1.0f, 1.0f);
    ER_CHECK_EQ(d.shield.cur, 0);          // shield fully gone
    ER_CHECK(d.armor.cur < d.armor.max);   // bled into armor
    ER_CHECK(d.hull.cur == d.hull.max);    // not yet to hull
    ER_CHECK(o == DamageOutcome::ShieldDown);
}

ER_TEST(Combat, KillReportedWhenHullDepleted)
{
    DefenseLayers d = MakeLayers(10, 10, 10);
    const DamageOutcome o = ApplyDamage(d, Resists(), DamageType::EM, 10000.0f, 1.0f, 1.0f);
    ER_CHECK_EQ(d.hull.cur, 0);
    ER_CHECK(o == DamageOutcome::Killed);
}

ER_TEST(Combat, ShieldRegensArmorHullDoNot)
{
    DefenseLayers d = MakeLayers(100, 100, 100, /*regen=*/30.0f);
    d.shield.cur = 10; d.armor.cur = 10; d.hull.cur = 10;
    for (int i = 0; i < 30; ++i) RegenDefenses(d, 1.0f / 30.0f); // 1 s
    ER_CHECK(d.shield.cur > 10);           // shield recovered
    ER_CHECK_EQ(d.armor.cur, 10);          // armor never passively regens
    ER_CHECK_EQ(d.hull.cur, 10);           // hull never passively regens
    ER_CHECK(d.shield.cur <= d.shield.max);
}

ER_TEST(Combat, RemoteRepRestoresTargetLayer)
{
    DefenseLayers d = MakeLayers(100, 100, 100);
    d.armor.cur = 20;
    const int32_t done = RemoteRep(d, DefenseLayer::Armor, 50);
    ER_CHECK_EQ(done, 50);
    ER_CHECK_EQ(d.armor.cur, 70);
    // Can't overheal past max.
    ER_CHECK_EQ(RemoteRep(d, DefenseLayer::Armor, 1000), 30);
    ER_CHECK_EQ(d.armor.cur, 100);
}

// --- tracking + falloff (combat-balance.md §2.3) ----------------------------

ER_TEST(Combat, FalloffDecaysPastOptimal)
{
    ER_CHECK_EQ(FalloffFactor(1000.0, 1500.0f, 800.0f), 1.0f);   // inside optimal
    ER_CHECK(FalloffFactor(1900.0, 1500.0f, 800.0f) < 1.0f);     // into falloff band
    ER_CHECK(FalloffFactor(1900.0, 1500.0f, 800.0f) > 0.0f);
    ER_CHECK_EQ(FalloffFactor(3000.0, 1500.0f, 800.0f), 0.0f);   // well past falloff
}

ER_TEST(Combat, TrackingPenalisesSmallFastTargets)
{
    // A big slow target (high sig, low speed) is hit far more reliably than a small
    // fast one — the size rock-paper-scissors (a light tackle survives heavy fire).
    const float bigSlow   = TrackingFactor(0.6f, 300.0f, 1200.0f);
    const float smallFast = TrackingFactor(0.6f, 40.0f, 3200.0f);
    ER_CHECK(bigSlow > smallFast);
    ER_CHECK(smallFast < 0.6f);               // notably reduced
    // A stationary target is always tracked (factor → 1).
    ER_CHECK_EQ(TrackingFactor(0.6f, 40.0f, 0.0f), 1.0f);
    // A tracking enhancer (higher tracking) recovers some of the hit chance.
    ER_CHECK(TrackingFactor(0.9f, 40.0f, 3200.0f) > smallFast);
}

ER_TEST(Combat, DamageRuleIsDeterministic)
{
    auto run = [] {
        DefenseLayers d = MakeLayers(123, 234, 345);
        for (int i = 0; i < 20; ++i)
            (void)ApplyDamage(d, Resists(), static_cast<DamageType>(i % 3), 17.0f + i, 0.8f, 0.9f);
        return d.TotalCur();
    };
    ER_CHECK_EQ(run(), run()); // same inputs → identical result (no RNG / wall-clock)
}
