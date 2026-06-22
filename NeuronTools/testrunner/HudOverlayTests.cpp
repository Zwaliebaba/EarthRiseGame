// In-world HUD overlay helper tests (playable slice). The snapshot carries current
// hp but not max, so the health-bar fraction comes from a per-kind nominal max;
// mirrors the platform-independent logic the EarthRise app draws (§16.2). Component
// ids are bound once per binary in ShapeCatalogTests.

#include "HudOverlay.h"
#include "TestRunner.h"

#include <cmath>

using namespace ertest;
using Neuron::Client::HealthFraction;
using Neuron::Client::NominalMaxHp;
using Neuron::Client::ShowsHealthBar;
using K = Neuron::Sim::EntityKind;

ER_TEST(HudOverlay, NominalMaxPerKind)
{
    ER_CHECK_EQ(NominalMaxHp(K::Base), 1000);
    ER_CHECK_EQ(NominalMaxHp(K::Ship), 500);
    ER_CHECK_EQ(NominalMaxHp(K::NpcUnit), 300);
    ER_CHECK_EQ(NominalMaxHp(K::Asteroid), 0); // scenery has no bar
}

ER_TEST(HudOverlay, ShowsBarOnlyForCombatKinds)
{
    ER_CHECK(ShowsHealthBar(K::Ship));
    ER_CHECK(ShowsHealthBar(K::NpcUnit));
    ER_CHECK(ShowsHealthBar(K::Base));
    ER_CHECK(!ShowsHealthBar(K::Decoration));
}

ER_TEST(HudOverlay, FractionClampsAndScales)
{
    ER_CHECK(std::fabs(HealthFraction(K::Ship, 250) - 0.5f) < 1e-4f); // half of 500
    ER_CHECK(std::fabs(HealthFraction(K::Ship, 500) - 1.0f) < 1e-4f);
    ER_CHECK(std::fabs(HealthFraction(K::Ship, 900) - 1.0f) < 1e-4f); // clamps high
    ER_CHECK(std::fabs(HealthFraction(K::Ship, -50) - 0.0f) < 1e-4f); // clamps low
    ER_CHECK(HealthFraction(K::Asteroid, 100) < 0.0f);                // no bar → negative
}
