// Balance-gate sim tests (ServerUniverse; M6 area M — the "fit & composition beat raw
// numbers" Done gate). The full statistical N-sim sweep with win-rate BANDS runs on the
// Windows ERHeadless agent; this is the platform-independent SIM half (§16.2): a focus-
// fire fleet-vs-fleet harness that resolves each matchup deterministically and asserts
// the intended qualitative outcome (combat-balance.md §6–§7). Component ids are bound in
// ShapeCatalogTests.

#include "Combat.h"
#include "Command.h"
#include "ServerUniverse.h"
#include "TestRunner.h"

#include <vector>

using namespace Neuron::Sim;
using namespace ertest;
using Neuron::Universe::UniversePos;

namespace
{
    uint16_t Ship() { return ServerUniverse::ShipShapeId(); }

    // Living members (entities still present have DefenseLayers; a kill removes them).
    int Alive(ServerUniverse& su, const std::vector<uint32_t>& fleet)
    {
        int n = 0; for (uint32_t id : fleet) if (su.DefenseOf(id)) ++n; return n;
    }

    // The enemy's primary by combat-balance.md §4 priority: logi > EWAR > nearest.
    int Priority(ServerUniverse& su, uint32_t id)
    {
        const Fitting* f = su.FittingOf(id);
        if (!f) return -1;
        bool logi = false, ewar = false;
        for (const auto& mi : f->modules) {
            if (mi.def.kind == ModuleKind::RemoteRep) logi = true;
            else if (mi.def.kind == ModuleKind::Jammer || mi.def.kind == ModuleKind::Web ||
                     mi.def.kind == ModuleKind::WarpDisruptor || mi.def.kind == ModuleKind::SensorDamp) ewar = true;
        }
        return logi ? 2 : (ewar ? 1 : 0);
    }

    void FocusFire(ServerUniverse& su, uint32_t owner, const std::vector<uint32_t>& mine,
                   const std::vector<uint32_t>& foe)
    {
        uint32_t primary = 0; int best = -2;
        for (uint32_t e : foe) { const int p = Priority(su, e); if (p > best) { best = p; primary = e; } }
        if (!primary) return;
        std::vector<uint32_t> living;
        for (uint32_t m : mine) if (su.DefenseOf(m)) living.push_back(m);
        if (living.empty()) return;
        FleetCommand c; c.intent = IntentType::Attack; c.units = living; c.targetNetId = primary;
        su.ApplyFleetCommand(owner, c);
    }

    // Ticks for one 'attackerFit' ship to kill a SHIELD-TANKED dummy (big shield, no
    // armor) under 'cat'. Isolates the damage-type-vs-tank counter from fleet noise.
    int TicksToKillShieldTank(const char* attackerFit, const CombatCatalog& cat)
    {
        ServerUniverse su(false);
        su.LoadCombat(cat);
        const uint32_t atk = su.SpawnFleetShipFit(1, Ship(), { 0, 0, 0 }, attackerFit);
        const uint32_t tgt = su.SpawnFleetShipFit(2, Ship(), { 1500, 0, 0 }, "fighter-kin");
        DefenseLayers* d = su.DefenseOf(tgt);
        d->shield.cur = d->shield.max = 2000; d->armor.cur = d->armor.max = 0; d->hull.cur = d->hull.max = 200;
        for (int t = 0; t < 5000; ++t) {
            if (!su.DefenseOf(tgt)) return t;
            FleetCommand c; c.intent = IntentType::Attack; c.units = { atk }; c.targetNetId = tgt;
            su.ApplyFleetCommand(1, c);
            su.Step(1.0f / 30.0f);
        }
        return 100000; // never killed
    }

    struct Outcome { int survA; int survB; };

    // Run two fleets to the death (focus-fire bots, deterministic). Spawns A near -x and
    // B near +x, in mutual weapon range, and steps until one side is wiped or the cap.
    Outcome Fight(const std::vector<const char*>& fitsA, const std::vector<const char*>& fitsB)
    {
        ServerUniverse su(false);
        std::vector<uint32_t> A, B;
        for (size_t i = 0; i < fitsA.size(); ++i)
            A.push_back(su.SpawnFleetShipFit(1, Ship(), { -1000, static_cast<int64_t>(i) * 200, 0 }, fitsA[i]));
        for (size_t i = 0; i < fitsB.size(); ++i)
            B.push_back(su.SpawnFleetShipFit(2, Ship(), {  1000, static_cast<int64_t>(i) * 200, 0 }, fitsB[i]));

        for (int t = 0; t < 3000; ++t) {
            if (Alive(su, A) == 0 || Alive(su, B) == 0) break;
            FocusFire(su, 1, A, B);
            FocusFire(su, 2, B, A);
            su.Step(1.0f / 30.0f);
        }
        return { Alive(su, A), Alive(su, B) };
    }
}

// Goal #1 (composition > numbers): a balanced mixed comp beats an equal-count mono-DPS
// blob of the same hull — logi sustain + EWAR suppression + an anchor tank win out.
ER_TEST(Balance, MixedCompBeatsMonoDps)
{
    const Outcome o = Fight(
        { "heavy-anchor", "logi-shield", "ewar-jam", "fighter-kin", "fighter-kin" },
        { "fighter-kin", "fighter-kin", "fighter-kin", "fighter-kin", "fighter-kin" });
    ER_CHECK(o.survA > 0);   // the mixed comp wins...
    ER_CHECK(o.survB == 0);  // ...wiping the mono blob
}

// Goal #2 (fit > tier / damage type matters): against a SHIELD-tanked target, the right
// damage type (EM, which shields resist poorly) kills decisively faster than the wrong
// one (Kinetic, which shields resist well) — scout the tank, bring the counter (§2.2).
ER_TEST(Balance, RightDamageTypeBeatsWrongVsTank)
{
    const int em  = TicksToKillShieldTank("fighter-em",  DefaultCombatCatalog()); // EM = right
    const int kin = TicksToKillShieldTank("fighter-kin", DefaultCombatCatalog()); // Kinetic = wrong
    ER_CHECK(em < kin);          // EM kills the shield tank faster
    ER_CHECK(em * 5 < kin * 4);  // by a clear margin (≳ 25% faster)
}

// The sustain loop: ADDING a logi to a fleet strictly improves its outcome against the
// same enemy — a logi-backed trio ends a fight with more survivors than the bare trio.
ER_TEST(Balance, LogiImprovesFleetSurvival)
{
    const Outcome bare    = Fight({ "fighter-kin", "fighter-kin", "fighter-kin" },
                                  { "fighter-kin", "fighter-kin", "fighter-kin" });
    const Outcome withLogi = Fight({ "fighter-kin", "fighter-kin", "fighter-kin", "logi-shield" },
                                   { "fighter-kin", "fighter-kin", "fighter-kin" });
    ER_CHECK(withLogi.survA > bare.survA); // logi sustain is load-bearing
    ER_CHECK(withLogi.survB <= bare.survB); // and the enemy fares no better against it
}

// Size rock-paper-scissors: a swarm of small fast Lights survives a single Heavy's poor
// tracking and grinds it down — the swarm wins (not a one-sided heavy steamroll).
ER_TEST(Balance, SwarmOfLightBeatsSingleHeavy)
{
    const Outcome o = Fight(
        { "scout-tackle", "scout-tackle", "scout-tackle", "scout-tackle", "scout-tackle", "scout-tackle" },
        { "heavy-anchor" });
    ER_CHECK(o.survB == 0);  // the heavy goes down...
    ER_CHECK(o.survA > 0);   // ...with light survivors (tracking kept them alive)
}

// A deliberately mis-tuned datum trips the gate (the gate actually bites): flatten every
// resist in the catalog and the EM-vs-Kinetic counter vanishes — EM and Kinetic kill the
// shield tank at the SAME rate — proving the outcome is driven by the data, not noise.
ER_TEST(Balance, MisTunedResistRemovesDamageTypeEdge)
{
    const CombatCatalog real = DefaultCombatCatalog();
    CombatCatalog flat = real;
    for (auto& h : flat.hulls)
        for (int l = 0; l < DEFENSE_LAYER_COUNT; ++l)
            for (int t = 0; t < DAMAGE_TYPE_COUNT; ++t)
                h.resists.resist[l][t] = 0.0f; // no counters anywhere

    const int emReal  = TicksToKillShieldTank("fighter-em",  real);
    const int kinReal = TicksToKillShieldTank("fighter-kin", real);
    const int emFlat  = TicksToKillShieldTank("fighter-em",  flat);
    const int kinFlat = TicksToKillShieldTank("fighter-kin", flat);

    ER_CHECK(emReal < kinReal);                      // with resists, EM has the edge
    const int realGap = kinReal - emReal;
    const int flatGap = (kinFlat > emFlat) ? (kinFlat - emFlat) : (emFlat - kinFlat);
    ER_CHECK(flatGap * 4 < realGap);                 // flattened → the edge collapses (gate bites)
}
