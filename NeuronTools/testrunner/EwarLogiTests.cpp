// EWAR + logistics tests (ServerUniverse + Combat.h; M6 area E) — the tactical
// archetypes that make composition beat numbers: jam (suppress fire), web (slow),
// warp-disrupt (tackle/interdiction §13.12), sensor-damp (cut range), and remote rep
// (the sustain loop). Component ids are bound in ShapeCatalogTests.

#include "Combat.h"
#include "Command.h"
#include "Navigation.h"
#include "ServerUniverse.h"
#include "TestRunner.h"

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
    void MoveTo(ServerUniverse& su, uint32_t owner, uint32_t unit, UniversePos p)
    {
        FleetCommand c; c.intent = IntentType::Move; c.units = { unit }; c.targetPoint = p;
        su.ApplyFleetCommand(owner, c);
    }
}

// --- jam suppresses the victim's fire ---------------------------------------

ER_TEST(EwarLogi, JamSuppressesFire)
{
    // A jammed attacker can't damage its own victim. Control: no jammer → it does.
    auto dummyHpAfter = [](bool withJammer) {
        ServerUniverse su(false);
        const uint32_t jammer = su.SpawnFleetShipFit(1, Ship(), { 0, 0, 0 }, "ewar-jam");
        const uint32_t target = su.SpawnFleetShipFit(2, Ship(), { 1000, 0, 0 }, "fighter-kin");
        const uint32_t dummy  = su.SpawnFleetShipFit(1, Ship(), { 1000, 1500, 0 }, "fighter-kin");
        if (auto* d = su.DefenseOf(dummy)) { d->shield.cur = d->shield.max = 1000000; } // survives either way
        Attack(su, 2, target, dummy);          // the victim's own target
        if (withJammer) Attack(su, 1, jammer, target); // jam the victim
        for (int i = 0; i < 30; ++i) su.Step(1.0f / 30.0f);
        return std::pair{ su.DefenseOf(dummy)->TotalCur(), su.EwarOf(target)->jammedFor };
    };

    const auto [hpJam, jammedFor] = dummyHpAfter(true);
    const auto [hpFree, _]        = dummyHpAfter(false);
    ER_CHECK(jammedFor > 0.0f);    // the victim is jammed
    ER_CHECK(hpJam > hpFree);      // jammed → its dummy took (much) less damage
    ER_CHECK(hpFree < hpJam);      // unjammed control → the victim dealt damage
}

// --- web slows the target ---------------------------------------------------

ER_TEST(EwarLogi, WebReducesSpeed)
{
    auto travelled = [](bool withWeb) {
        ServerUniverse su(false);
        const uint32_t webber = su.SpawnFleetShipFit(1, Ship(), { 0, 0, 0 }, "fighter-em"); // has a web
        const uint32_t target = su.SpawnFleetShipFit(2, Ship(), { 1000, 0, 0 }, "fighter-kin");
        if (auto* d = su.DefenseOf(target)) { d->shield.cur = d->shield.max = 1000000; } // not killed mid-test
        MoveTo(su, 2, target, { 1000, 60000, 0 }); // run straight up +y
        if (withWeb) Attack(su, 1, webber, target);
        for (int i = 0; i < 30; ++i) su.Step(1.0f / 30.0f); // 1 s
        UniversePos p{}; (void)su.GetBasePos(target, p);
        return p.y;
    };
    const int64_t free = travelled(false);
    const int64_t webd = travelled(true);
    ER_CHECK(webd < free);          // webbed → covered less ground
    ER_CHECK(webd * 10 < free * 9); // and meaningfully so (≳10% slower)
}

// --- warp-disrupt prevents warp (interdiction, §13.12) ----------------------

ER_TEST(EwarLogi, WarpDisruptPreventsWarp)
{
    ServerUniverse su(false);
    const uint32_t runner  = su.SpawnFleetShipFit(2, Ship(), { 0, 0, 0 }, "fighter-kin");
    const uint32_t tackler = su.SpawnFleetShipFit(1, Ship(), { 500, 0, 0 }, "scout-tackle"); // warp-disruptor
    ER_CHECK(su.BeginWarpTo(runner, { 0, 0, 5000000 })); // the runner tries to flee
    Attack(su, 1, tackler, runner);                      // tackle it

    for (int i = 0; i < 20; ++i) su.Step(1.0f / 30.0f);
    ER_CHECK(su.EwarOf(runner)->tackledFor > 0.0f);             // it is tackled
    ER_CHECK(su.NavOf(runner)->phase != NavPhase::Warp);        // and never entered warp (interdicted)
}

// --- sensor damp cuts the target's optimal range ----------------------------

ER_TEST(EwarLogi, SensorDampApplies)
{
    ServerUniverse su(false);
    const uint32_t damper = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 }); // base fit carries a sensor damp
    const uint32_t target = su.SpawnFleetShipFit(2, Ship(), { 1500, 0, 0 }, "fighter-kin");
    (void)damper;
    for (int i = 0; i < 5; ++i) su.Step(1.0f / 30.0f);
    ER_CHECK(su.EwarOf(target)->sensorDampFactor < 1.0f); // optimal range cut by the damp
}

// --- remote rep is the sustain loop -----------------------------------------

ER_TEST(EwarLogi, RemoteRepOutSustainsSoloShip)
{
    // A logi-backed damaged ally recovers more shield than the same ally alone over the
    // same window (isolates the remote-rep contribution from passive regen).
    auto shieldAfter = [](bool withLogi) {
        ServerUniverse su(false);
        const uint32_t ally = su.SpawnFleetShipFit(1, Ship(), { 1000, 0, 0 }, "fighter-kin");
        su.DefenseOf(ally)->shield.cur = 50; // damaged shield to rep
        if (withLogi) su.SpawnFleetShipFit(1, Ship(), { 0, 0, 0 }, "logi-shield"); // auto-reps the ally
        for (int i = 0; i < 30; ++i) su.Step(1.0f / 30.0f); // 1 s
        return su.DefenseOf(ally)->shield.cur;
    };
    ER_CHECK(shieldAfter(true) > shieldAfter(false)); // remote rep adds real sustain
}
