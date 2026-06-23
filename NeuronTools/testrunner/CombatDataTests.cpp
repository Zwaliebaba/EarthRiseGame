// Combat catalog game-data tests (CombatData.h; M6 area A) — the cook/load codec,
// the referential-integrity rules `datacheck` enforces, and the shared ValidateFit
// budget/slot rule. Pure data + Serde, platform-independent, so the Linux runner
// covers them (mirrors the NeuronCoreTest cases the M6 plan lists for area A).

#include "CombatData.h"
#include "TestRunner.h"

#include <string>
#include <vector>

using namespace Neuron::Sim;
using namespace ertest;

namespace
{
    // Structural equality for the round-trip check (codec must be lossless).
    bool SameCatalog(const CombatCatalog& a, const CombatCatalog& b)
    {
        if (a.hulls.size() != b.hulls.size() || a.modules.size() != b.modules.size() ||
            a.fits.size() != b.fits.size())
            return false;
        for (size_t i = 0; i < a.hulls.size(); ++i) {
            const auto& x = a.hulls[i]; const auto& y = b.hulls[i];
            if (x.code != y.code || x.size != y.size || x.tier != y.tier) return false;
            if (x.slotsHigh != y.slotsHigh || x.slotsMid != y.slotsMid || x.slotsLow != y.slotsLow) return false;
            if (x.pgMax != y.pgMax || x.cpuMax != y.cpuMax) return false;
            if (x.shieldHp != y.shieldHp || x.armorHp != y.armorHp || x.hullHp != y.hullHp) return false;
            if (x.signature != y.signature || x.maxSpeed != y.maxSpeed) return false;
            for (int l = 0; l < kDefenseLayerCount; ++l)
                for (int t = 0; t < kDamageTypeCount; ++t)
                    if (x.resists.resist[l][t] != y.resists.resist[l][t]) return false;
        }
        for (size_t i = 0; i < a.modules.size(); ++i) {
            const auto& x = a.modules[i]; const auto& y = b.modules[i];
            if (x.code != y.code || x.kind != y.kind || x.slot != y.slot) return false;
            if (x.pgCost != y.pgCost || x.cpuCost != y.cpuCost) return false;
            if (x.damageType != y.damageType || x.baseDamage != y.baseDamage) return false;
            if (x.optimal != y.optimal || x.falloff != y.falloff || x.tracking != y.tracking) return false;
            if (x.projectileSpeed != y.projectileSpeed) return false;
            if (x.effectLayer != y.effectLayer || x.strength != y.strength) return false;
            if (x.duration != y.duration || x.range != y.range) return false;
        }
        for (size_t i = 0; i < a.fits.size(); ++i) {
            if (a.fits[i].name != b.fits[i].name || a.fits[i].hull != b.fits[i].hull) return false;
            if (a.fits[i].modules != b.fits[i].modules) return false;
        }
        return true;
    }
}

// --- the authored catalog is internally consistent --------------------------

ER_TEST(CombatData, DefaultCatalogValidates)
{
    const CombatCatalog c = DefaultCombatCatalog();
    std::vector<std::string> errs;
    ER_CHECK(ValidateCombatCatalog(c, errs));
    ER_CHECK(errs.empty());
    // Sanity: the §3 hull ladder + §2.2 damage triangle + §6 fits are all present.
    ER_CHECK(c.hulls.size() >= 5);
    ER_CHECK(c.FindHull("hull.medium") != nullptr);
    ER_CHECK(c.FindModule("module.railgun.t1") != nullptr);
    ER_CHECK(c.FindFit("fighter-em") != nullptr);
}

ER_TEST(CombatData, CookLoadRoundTrips)
{
    const CombatCatalog c = DefaultCombatCatalog();
    const auto blob = EncodeCombatCatalog(c);
    const auto back = DecodeCombatCatalog(blob);
    ER_CHECK(back.has_value());
    ER_CHECK(SameCatalog(c, *back));
}

ER_TEST(CombatData, DecodeRejectsCorruptBlob)
{
    auto blob = EncodeCombatCatalog(DefaultCombatCatalog());
    blob[0] ^= 0xFF; // corrupt the Serde version header → protocol gate rejects
    ER_CHECK(!DecodeCombatCatalog(blob).has_value());
}

// --- datacheck-style rejection of malformed rows ----------------------------

ER_TEST(CombatData, RejectsNegativePgModule)
{
    CombatCatalog c = DefaultCombatCatalog();
    c.modules.front().pgCost = -1.0f; // a railgun with negative PG cost
    std::vector<std::string> errs;
    ER_CHECK(!ValidateCombatCatalog(c, errs));
    ER_CHECK(!errs.empty());
}

ER_TEST(CombatData, RejectsHullWithNoHp)
{
    CombatCatalog c = DefaultCombatCatalog();
    HullClass& h = c.hulls[1]; // hull.medium
    h.shieldHp = h.armorHp = h.hullHp = 0;
    std::vector<std::string> errs;
    ER_CHECK(!ValidateCombatCatalog(c, errs));
}

ER_TEST(CombatData, RejectsOutOfRangeResist)
{
    CombatCatalog c = DefaultCombatCatalog();
    c.hulls[1].resists.Set(DefenseLayer::Shield, DamageType::EM, 1.5f); // ≥ 1.0 = invincible
    std::vector<std::string> errs;
    ER_CHECK(!ValidateCombatCatalog(c, errs));
}

ER_TEST(CombatData, RejectsFitOverBudget)
{
    CombatCatalog c = DefaultCombatCatalog();
    // Add an over-budget fit: pile high-cost modules past the medium hull's PG budget.
    FitTemplate over;
    over.name = "over-pg"; over.hull = "hull.medium";
    over.modules = { "module.railgun.t1", "module.railgun.t1", "module.railgun.t1" }; // 3 highs OK on slots
    // Shrink the hull PG so the same fit no longer fits → must be rejected.
    c.hulls[1].pgMax = 10.0f;
    c.fits.push_back(over);
    std::vector<std::string> errs;
    ER_CHECK(!ValidateCombatCatalog(c, errs));
}

ER_TEST(CombatData, RejectsFitUnknownHull)
{
    CombatCatalog c = DefaultCombatCatalog();
    c.fits.push_back({ "ghost", "hull.nonexistent", {} });
    std::vector<std::string> errs;
    ER_CHECK(!ValidateCombatCatalog(c, errs));
}

// --- the shared ValidateFit rule (area A check == area B runtime guard) ------

ER_TEST(CombatData, ValidateFitAcceptsLegalAndRejectsOverAndWrongSlot)
{
    const CombatCatalog c = DefaultCombatCatalog();
    const HullClass* medium = c.FindHull("hull.medium");
    ER_CHECK(medium != nullptr);

    // Legal: the authored fighter loadout fits the medium hull exactly.
    const FitTemplate* fighter = c.FindFit("fighter-em");
    ER_CHECK(fighter != nullptr);
    ER_CHECK(ValidateFit(*medium, fighter->modules, c).ok);

    // Wrong slot: too many High-slot weapons for a 3-High hull.
    {
        std::vector<std::string> fit = { "module.railgun.t1", "module.railgun.t1",
                                         "module.railgun.t1", "module.railgun.t1" };
        ER_CHECK(!ValidateFit(*medium, fit, c).ok);
    }
    // Over PG: a light hull can't carry a fat heavy-style gun stack.
    {
        const HullClass* light = c.FindHull("hull.light");
        std::vector<std::string> fit = { "module.railgun.t1", "module.railgun.t1" }; // 2 highs > light 2? ok count
        // Light has 2 High slots, PG 50; two railguns (15 each) fit. Force over-PG by budget.
        HullClass tiny = *light; tiny.pgMax = 5.0f;
        ER_CHECK(!ValidateFit(tiny, fit, c).ok);
    }
    // Unknown module is rejected.
    {
        std::vector<std::string> fit = { "module.doesnotexist" };
        ER_CHECK(!ValidateFit(*medium, fit, c).ok);
    }
}

ER_TEST(CombatData, EveryFitResolvesToRealCodes)
{
    const CombatCatalog c = DefaultCombatCatalog();
    for (const auto& f : c.fits) {
        ER_CHECK(c.FindHull(f.hull) != nullptr);
        for (const auto& mc : f.modules)
            ER_CHECK(c.FindModule(mc) != nullptr); // resolves to a real ItemDefs code
        ER_CHECK(ValidateFit(*c.FindHull(f.hull), f.modules, c).ok);
    }
}
