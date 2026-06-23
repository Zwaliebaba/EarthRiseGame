#pragma once
// CombatData.h — the cooked COMBAT catalog: hull classes, module definitions,
// damage-type/resist stats, and named fit templates (masterplan §12.6, §13.2, §15;
// design in docs/design/combat-balance.md §2–§6).
//
// Same shape and rules as UniverseData.h: a pure data model + a versioned binary
// codec + the referential-integrity rules `datacheck` enforces, with NO platform
// deps — so the cook tools, the server sim, the client, and bots share ONE format
// and ONE rule set (no drift). Per §15's catalog/balance boundary, SQL holds only
// the canonical `ItemDefs` *codes*; every stat (PG/CPU, damage, resists, ranges)
// lives here as game data, never as a balance literal in the combat rules (Combat.h).
//
// This header owns three things:
//   1. the runtime model (HullClass / ModuleDef / FitTemplate / CombatCatalog),
//   2. the binary codec (EncodeCombatCatalog / DecodeCombatCatalog),
//   3. the rules (ValidateCombatCatalog + the shared ValidateFit), plus a first-pass
//      authored catalog (DefaultCombatCatalog) carrying the combat-balance.md numbers
//      the balance gates (M6 area M) sweep.

#include "CombatTypes.h"
#include "Serde.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Neuron::Sim
{

// --- runtime model ----------------------------------------------------------

// A hull class (combat-balance.md §3): slot grid + PG/CPU budget + base layer HP +
// the hull's own resist profile + sig/speed. `code` keys a canonical ItemDefs row.
struct HullClass
{
    std::string   code;                       // ItemDefs Code, e.g. "hull.cruiser"
    HullSize      size{ HullSize::Medium };
    uint8_t       tier{ 1 };                   // T1→T3 gate (§13.3)
    uint8_t       slotsHigh{ 0 }, slotsMid{ 0 }, slotsLow{ 0 };
    float         pgMax{ 0.0f }, cpuMax{ 0.0f };
    float         mass{ 0.0f };
    float         signature{ 0.0f };           // hit size — small = hard to track
    float         maxSpeed{ 0.0f };            // m/s sublight
    int32_t       shieldHp{ 0 }, armorHp{ 0 }, hullHp{ 0 };
    float         shieldRegenPerSec{ 0.0f };   // passive shield regen (armor/hull = 0)
    ResistProfile resists{};
};

// A fittable module (combat-balance.md §5). One def, many instances on hull slots.
// Most fields are kind-specific (documented on ModuleKind); unused fields are 0.
struct ModuleDef
{
    std::string code;                          // ItemDefs Code, e.g. "module.railgun.t1"
    ModuleKind  kind{ ModuleKind::Weapon };
    SlotType    slot{ SlotType::High };
    uint8_t     tier{ 1 };
    float       pgCost{ 0.0f }, cpuCost{ 0.0f };

    // Weapon params (kind == Weapon).
    DamageType  damageType{ DamageType::Kinetic };
    float       baseDamage{ 0.0f };            // per shot, before resist/tracking/falloff
    float       rateOfFire{ 0.0f };            // shots/sec
    float       optimal{ 0.0f };               // optimal range (m): full falloff factor
    float       falloff{ 0.0f };               // distance past optimal to ~0 (m)
    float       tracking{ 0.0f };              // bigger tracks small/fast targets better
    float       projectileSpeed{ 0.0f };       // m/s; 0 = hitscan (instant, no projectile)

    // Effect params (EWAR / logistics / propulsion / passive tank — see ModuleKind).
    DefenseLayer effectLayer{ DefenseLayer::Shield };
    float        strength{ 0.0f };             // rep/s, speed×, dmg+frac, plate HP, tracking+, …
    float        duration{ 0.0f };             // EWAR effect seconds
    float        range{ 0.0f };                // effect / remote-rep range (m)
};

// A named loadout (combat-balance.md §6): a hull + an ordered module list (one per
// slot it occupies). The data-driven bot fit M6 combat is exercised through — there
// is no player fitting screen until M7 (§22.1), so bots fit these directly.
struct FitTemplate
{
    std::string              name;             // e.g. "fighter-em"
    std::string              hull;             // ref → HullClass.code
    std::vector<std::string> modules;          // refs → ModuleDef.code (each = one slot)
};

struct CombatCatalog
{
    std::vector<HullClass>   hulls;
    std::vector<ModuleDef>   modules;
    std::vector<FitTemplate> fits;

    [[nodiscard]] const HullClass* FindHull(std::string_view c) const noexcept
    {
        for (const auto& h : hulls) if (h.code == c) return &h;
        return nullptr;
    }
    [[nodiscard]] const ModuleDef* FindModule(std::string_view c) const noexcept
    {
        for (const auto& m : modules) if (m.code == c) return &m;
        return nullptr;
    }
    [[nodiscard]] const FitTemplate* FindFit(std::string_view n) const noexcept
    {
        for (const auto& f : fits) if (f.name == n) return &f;
        return nullptr;
    }
};

// --- the shared fitting rule (server-authoritative; area A check + area B runtime) -
//
// A fit is legal iff every module resolves, the per-slot-type counts fit the hull's
// slot grid, and total PG/CPU ≤ the hull budget (combat-balance.md §5). Returns ok +
// (on failure) a human-readable reason. ONE definition used by datacheck (area A) and
// the runtime ValidateFit guard (area B) — no over-budget fit is ever accepted.
struct FitResult
{
    bool        ok{ false };
    std::string error;
    float       pgUsed{ 0.0f }, cpuUsed{ 0.0f };
    uint8_t     used[kSlotTypeCount]{};
};

[[nodiscard]] inline FitResult ValidateFit(const HullClass& hull,
                                           const std::vector<std::string>& moduleCodes,
                                           const CombatCatalog& cat)
{
    FitResult r;
    const uint8_t cap[kSlotTypeCount] = { hull.slotsHigh, hull.slotsMid, hull.slotsLow };
    for (const auto& code : moduleCodes) {
        const ModuleDef* m = cat.FindModule(code);
        if (!m) { r.error = "fit references unknown module '" + code + "'"; return r; }
        const int s = static_cast<int>(m->slot);
        if (++r.used[s] > cap[s]) {
            r.error = "fit exceeds hull '" + hull.code + "' slot capacity for slot " + std::to_string(s);
            return r;
        }
        r.pgUsed  += m->pgCost;
        r.cpuUsed += m->cpuCost;
    }
    if (r.pgUsed > hull.pgMax)  { r.error = "fit over PG budget on hull '" + hull.code + "'";  return r; }
    if (r.cpuUsed > hull.cpuMax){ r.error = "fit over CPU budget on hull '" + hull.code + "'"; return r; }
    r.ok = true;
    return r;
}

// --- binary codec (Serde; one definition shared by cook + runtime) ----------

namespace combat_detail
{
    inline void WriteStr(Serde::WriteBuffer& wb, const std::string& s)
    {
        wb.WriteUint16(static_cast<uint16_t>(s.size()));
        if (!s.empty()) wb.WriteBytes(s.data(), s.size());
    }
    inline std::string ReadStr(Serde::ReadBuffer& rb)
    {
        const uint16_t n = rb.ReadUint16();
        std::string s(n, '\0');
        if (n) rb.ReadBytes(s.data(), n);
        return s;
    }
    inline void WriteResists(Serde::WriteBuffer& wb, const ResistProfile& rp)
    {
        for (int l = 0; l < kDefenseLayerCount; ++l)
            for (int t = 0; t < kDamageTypeCount; ++t)
                wb.WriteFloat(rp.resist[l][t]);
    }
    inline ResistProfile ReadResists(Serde::ReadBuffer& rb)
    {
        ResistProfile rp;
        for (int l = 0; l < kDefenseLayerCount; ++l)
            for (int t = 0; t < kDamageTypeCount; ++t)
                rp.resist[l][t] = rb.ReadFloat();
        return rp;
    }
} // namespace combat_detail

[[nodiscard]] inline std::vector<uint8_t> EncodeCombatCatalog(const CombatCatalog& c)
{
    Serde::WriteBuffer wb(1024);

    wb.WriteUint16(static_cast<uint16_t>(c.hulls.size()));
    for (const auto& h : c.hulls) {
        combat_detail::WriteStr(wb, h.code);
        wb.WriteUint8(static_cast<uint8_t>(h.size));
        wb.WriteUint8(h.tier);
        wb.WriteUint8(h.slotsHigh); wb.WriteUint8(h.slotsMid); wb.WriteUint8(h.slotsLow);
        wb.WriteFloat(h.pgMax); wb.WriteFloat(h.cpuMax);
        wb.WriteFloat(h.mass);  wb.WriteFloat(h.signature); wb.WriteFloat(h.maxSpeed);
        wb.WriteUint32(static_cast<uint32_t>(h.shieldHp));
        wb.WriteUint32(static_cast<uint32_t>(h.armorHp));
        wb.WriteUint32(static_cast<uint32_t>(h.hullHp));
        wb.WriteFloat(h.shieldRegenPerSec);
        combat_detail::WriteResists(wb, h.resists);
    }

    wb.WriteUint16(static_cast<uint16_t>(c.modules.size()));
    for (const auto& m : c.modules) {
        combat_detail::WriteStr(wb, m.code);
        wb.WriteUint8(static_cast<uint8_t>(m.kind));
        wb.WriteUint8(static_cast<uint8_t>(m.slot));
        wb.WriteUint8(m.tier);
        wb.WriteFloat(m.pgCost); wb.WriteFloat(m.cpuCost);
        wb.WriteUint8(static_cast<uint8_t>(m.damageType));
        wb.WriteFloat(m.baseDamage); wb.WriteFloat(m.rateOfFire);
        wb.WriteFloat(m.optimal);    wb.WriteFloat(m.falloff);
        wb.WriteFloat(m.tracking);   wb.WriteFloat(m.projectileSpeed);
        wb.WriteUint8(static_cast<uint8_t>(m.effectLayer));
        wb.WriteFloat(m.strength);   wb.WriteFloat(m.duration); wb.WriteFloat(m.range);
    }

    wb.WriteUint16(static_cast<uint16_t>(c.fits.size()));
    for (const auto& f : c.fits) {
        combat_detail::WriteStr(wb, f.name);
        combat_detail::WriteStr(wb, f.hull);
        wb.WriteUint16(static_cast<uint16_t>(f.modules.size()));
        for (const auto& mc : f.modules) combat_detail::WriteStr(wb, mc);
    }

    wb.Finalise();
    const auto bytes = wb.Data();
    return { bytes.begin(), bytes.end() };
}

[[nodiscard]] inline std::optional<CombatCatalog> DecodeCombatCatalog(std::span<const uint8_t> body)
{
    Serde::ReadBuffer rb(body);
    if (!rb.IsGood()) return std::nullopt;

    CombatCatalog c;

    const uint16_t hullCount = rb.ReadUint16();
    c.hulls.reserve(hullCount);
    for (uint16_t i = 0; i < hullCount; ++i) {
        HullClass h;
        h.code = combat_detail::ReadStr(rb);
        h.size = static_cast<HullSize>(rb.ReadUint8());
        h.tier = rb.ReadUint8();
        h.slotsHigh = rb.ReadUint8(); h.slotsMid = rb.ReadUint8(); h.slotsLow = rb.ReadUint8();
        h.pgMax = rb.ReadFloat(); h.cpuMax = rb.ReadFloat();
        h.mass = rb.ReadFloat();  h.signature = rb.ReadFloat(); h.maxSpeed = rb.ReadFloat();
        h.shieldHp = static_cast<int32_t>(rb.ReadUint32());
        h.armorHp  = static_cast<int32_t>(rb.ReadUint32());
        h.hullHp   = static_cast<int32_t>(rb.ReadUint32());
        h.shieldRegenPerSec = rb.ReadFloat();
        h.resists = combat_detail::ReadResists(rb);
        c.hulls.push_back(std::move(h));
    }

    const uint16_t modCount = rb.ReadUint16();
    c.modules.reserve(modCount);
    for (uint16_t i = 0; i < modCount; ++i) {
        ModuleDef m;
        m.code = combat_detail::ReadStr(rb);
        m.kind = static_cast<ModuleKind>(rb.ReadUint8());
        m.slot = static_cast<SlotType>(rb.ReadUint8());
        m.tier = rb.ReadUint8();
        m.pgCost = rb.ReadFloat(); m.cpuCost = rb.ReadFloat();
        m.damageType = static_cast<DamageType>(rb.ReadUint8());
        m.baseDamage = rb.ReadFloat(); m.rateOfFire = rb.ReadFloat();
        m.optimal = rb.ReadFloat();    m.falloff = rb.ReadFloat();
        m.tracking = rb.ReadFloat();   m.projectileSpeed = rb.ReadFloat();
        m.effectLayer = static_cast<DefenseLayer>(rb.ReadUint8());
        m.strength = rb.ReadFloat();   m.duration = rb.ReadFloat(); m.range = rb.ReadFloat();
        c.modules.push_back(std::move(m));
    }

    const uint16_t fitCount = rb.ReadUint16();
    c.fits.reserve(fitCount);
    for (uint16_t i = 0; i < fitCount; ++i) {
        FitTemplate f;
        f.name = combat_detail::ReadStr(rb);
        f.hull = combat_detail::ReadStr(rb);
        const uint16_t n = rb.ReadUint16();
        f.modules.reserve(n);
        for (uint16_t j = 0; j < n; ++j) f.modules.push_back(combat_detail::ReadStr(rb));
        c.fits.push_back(std::move(f));
    }

    if (!rb.IsGood()) return std::nullopt;
    return c;
}

// --- referential integrity (this IS what the combat `datacheck` enforces) ---
//
// Rules (combat-balance.md §2–§6):
//   hulls   — non-empty + unique codes; PG/CPU/sig/speed ≥ 0; ≥1 layer HP; resists in [-1,1).
//   modules — non-empty + unique codes; PG/CPU ≥ 0; weapons have damage/rof/optimal ≥ 0;
//             a valid slot/kind pairing; damageType/effectLayer in range (by construction).
//   fits    — non-empty + unique names; hull resolves; every module resolves AND the fit
//             obeys the hull slot grid + PG/CPU budget (ValidateFit).
// Returns true iff `errors` is left empty.
[[nodiscard]] inline bool ValidateCombatCatalog(const CombatCatalog& c,
                                                std::vector<std::string>& errors)
{
    const size_t before = errors.size();
    auto err = [&](std::string m) { errors.push_back(std::move(m)); };

    std::unordered_set<std::string> hullCodes;
    for (const auto& h : c.hulls) {
        if (h.code.empty()) err("hull with empty code");
        else if (!hullCodes.insert(h.code).second) err("duplicate hull '" + h.code + "'");
        if (h.pgMax < 0.0f || h.cpuMax < 0.0f) err("hull '" + h.code + "' has negative PG/CPU budget");
        if (h.signature < 0.0f) err("hull '" + h.code + "' has negative signature");
        if (h.maxSpeed < 0.0f)  err("hull '" + h.code + "' has negative max speed");
        if (h.shieldHp < 0 || h.armorHp < 0 || h.hullHp < 0) err("hull '" + h.code + "' has negative layer HP");
        if (h.shieldHp + h.armorHp + h.hullHp <= 0) err("hull '" + h.code + "' has no defense HP");
        if (h.shieldRegenPerSec < 0.0f) err("hull '" + h.code + "' has negative shield regen");
        for (int l = 0; l < kDefenseLayerCount; ++l)
            for (int t = 0; t < kDamageTypeCount; ++t)
                if (h.resists.resist[l][t] <= -1.0f || h.resists.resist[l][t] >= 1.0f)
                    err("hull '" + h.code + "' resist out of (-1,1)");
    }

    std::unordered_set<std::string> modCodes;
    for (const auto& m : c.modules) {
        if (m.code.empty()) err("module with empty code");
        else if (!modCodes.insert(m.code).second) err("duplicate module '" + m.code + "'");
        if (m.pgCost < 0.0f || m.cpuCost < 0.0f) err("module '" + m.code + "' has negative PG/CPU cost");
        if (m.kind == ModuleKind::Weapon) {
            if (m.slot != SlotType::High) err("weapon '" + m.code + "' must be a High-slot module");
            if (m.baseDamage <= 0.0f) err("weapon '" + m.code + "' has non-positive base damage");
            if (m.rateOfFire <= 0.0f) err("weapon '" + m.code + "' has non-positive rate of fire");
            if (m.optimal < 0.0f || m.falloff < 0.0f) err("weapon '" + m.code + "' has negative range");
            if (m.projectileSpeed < 0.0f) err("weapon '" + m.code + "' has negative projectile speed");
        }
        if (m.kind == ModuleKind::RemoteRep && m.range <= 0.0f)
            err("remote-rep '" + m.code + "' needs a positive range");
    }

    std::unordered_set<std::string> fitNames;
    for (const auto& f : c.fits) {
        if (f.name.empty()) { err("fit with empty name"); continue; }
        if (!fitNames.insert(f.name).second) err("duplicate fit '" + f.name + "'");
        const HullClass* hull = c.FindHull(f.hull);
        if (!hull) { err("fit '" + f.name + "' references unknown hull '" + f.hull + "'"); continue; }
        const FitResult fr = ValidateFit(*hull, f.modules, c);
        if (!fr.ok) err("fit '" + f.name + "': " + fr.error);
    }

    return errors.size() == before;
}

// --- first-pass authored catalog (combat-balance.md §2–§6) -------------------
//
// THIS is the authoring source for the launch balance numbers — the single place
// the resist spreads (±40/±25/0, §2.2), the hull ladder (§3), and the role fits (§6)
// live, and exactly what the M6 area-M balance gates sweep. No combat rule (Combat.h)
// carries any of these literals; it reads them from a loaded catalog. Editing balance
// = editing this data (or a cooked override), never the rules.
[[nodiscard]] inline CombatCatalog DefaultCombatCatalog()
{
    CombatCatalog c;

    // Resist spreads (§2.2): shields tank Kinetic well / EM poorly; armor tanks
    // EM/Kinetic well / Thermal poorly; hull is flat. This encodes the counter
    // triangle — EM beats shield, Thermal beats armor, Kinetic is shield's weakness.
    auto resists = [] {
        ResistProfile rp;
        rp.Set(DefenseLayer::Shield, DamageType::Kinetic, 0.40f);
        rp.Set(DefenseLayer::Shield, DamageType::Thermal, 0.25f);
        rp.Set(DefenseLayer::Shield, DamageType::EM,      0.00f);
        rp.Set(DefenseLayer::Armor,  DamageType::Kinetic, 0.25f);
        rp.Set(DefenseLayer::Armor,  DamageType::Thermal, 0.00f);
        rp.Set(DefenseLayer::Armor,  DamageType::EM,      0.40f);
        // Hull: flat 0 across the board.
        return rp;
    }();

    // Hull ladder (§3): slots H/M/L, PG/CPU budget, sig, speed, base layer HP.
    auto hull = [&](const char* code, HullSize sz, uint8_t tier, uint8_t h, uint8_t m, uint8_t l,
                    float pg, float cpu, float sig, float speed, int32_t sh, int32_t ar, int32_t hu,
                    float regen) {
        HullClass x; x.code = code; x.size = sz; x.tier = tier;
        x.slotsHigh = h; x.slotsMid = m; x.slotsLow = l;
        x.pgMax = pg; x.cpuMax = cpu; x.signature = sig; x.maxSpeed = speed;
        x.shieldHp = sh; x.armorHp = ar; x.hullHp = hu; x.shieldRegenPerSec = regen;
        x.resists = resists;
        c.hulls.push_back(std::move(x));
    };
    //    code              size                tier  H  M  L    PG    CPU   sig    speed   shield armor hull  regen
    hull("hull.light",      HullSize::Light,      1,  2, 2, 1,   50,    60,   40,   3200,    180,   140,  120,  6.0f);
    hull("hull.medium",     HullSize::Medium,     1,  3, 3, 3,  120,   150,  120,   2000,    420,   380,  340,  9.0f);
    hull("hull.heavy",      HullSize::Heavy,      2,  4, 3, 4,  250,   220,  300,   1200,    760,   900,  820, 12.0f);
    hull("hull.industrial", HullSize::Industrial, 1,  1, 2, 2,   90,   120,  280,   1100,    300,   360,  520,  5.0f);
    hull("hull.capital",    HullSize::Capital,    1,  4, 4, 4, 1000,  1000, 2000,    400,   6000,  7000, 9000, 30.0f);

    // Modules (§5). Weapons: one per damage type — Railgun=Kinetic (projectile),
    // Laser=Thermal (hitscan), Pulser=EM (projectile). Tank/EWAR/logi/prop follow.
    auto weapon = [&](const char* code, DamageType dt, float pg, float cpu, float dmg, float rof,
                      float optimal, float falloff, float tracking, float projSpeed) {
        ModuleDef m; m.code = code; m.kind = ModuleKind::Weapon; m.slot = SlotType::High;
        m.pgCost = pg; m.cpuCost = cpu; m.damageType = dt; m.baseDamage = dmg; m.rateOfFire = rof;
        m.optimal = optimal; m.falloff = falloff; m.tracking = tracking; m.projectileSpeed = projSpeed;
        c.modules.push_back(std::move(m));
    };
    //     code                  type               PG  CPU  dmg  rof   opt    fall  track  projSpd
    weapon("module.railgun.t1", DamageType::Kinetic, 15, 10,  44,  1.0f, 1800,  900,  0.6f,  6000);
    weapon("module.laser.t1",   DamageType::Thermal, 15, 12,  40,  1.2f, 1400,  700,  0.9f,     0); // hitscan
    weapon("module.pulser.t1",  DamageType::EM,      15, 10,  46,  1.0f, 1500,  800,  0.7f,  5000);

    auto mod = [&](const char* code, ModuleKind kind, SlotType slot, float pg, float cpu,
                   DefenseLayer layer, float strength, float duration, float range) {
        ModuleDef m; m.code = code; m.kind = kind; m.slot = slot;
        m.pgCost = pg; m.cpuCost = cpu; m.effectLayer = layer;
        m.strength = strength; m.duration = duration; m.range = range;
        c.modules.push_back(std::move(m));
    };
    //  code                       kind                       slot            PG  CPU  layer                 str   dur   range
    mod("module.shieldbooster.t1", ModuleKind::ShieldBooster, SlotType::Mid,  10, 20, DefenseLayer::Shield, 15.0f, 0,     0);
    mod("module.remoteshield.t1",  ModuleKind::RemoteRep,     SlotType::High, 18, 22, DefenseLayer::Shield, 25.0f, 0,  3000);
    mod("module.remotearmor.t1",   ModuleKind::RemoteRep,     SlotType::High, 22, 20, DefenseLayer::Armor,  22.0f, 0,  2500);
    mod("module.jammer.t1",        ModuleKind::Jammer,        SlotType::Mid,   8, 30, DefenseLayer::Shield,  0.0f, 5.0f, 2500);
    mod("module.web.t1",           ModuleKind::Web,           SlotType::Mid,   6, 25, DefenseLayer::Shield,  0.5f, 4.0f, 1500);
    mod("module.warpdisruptor.t1", ModuleKind::WarpDisruptor, SlotType::Mid,   5, 20, DefenseLayer::Shield,  0.0f, 6.0f, 2000);
    mod("module.sensordamp.t1",    ModuleKind::SensorDamp,    SlotType::Mid,   7, 28, DefenseLayer::Shield,  0.5f, 5.0f, 2500);
    mod("module.armorplate.t1",    ModuleKind::ArmorPlate,    SlotType::Low,  12,  5, DefenseLayer::Armor, 200.0f, 0,     0);
    mod("module.damageamp.t1",     ModuleKind::DamageAmp,     SlotType::Low,   8, 15, DefenseLayer::Shield,  0.2f, 0,     0);
    mod("module.trackingenh.t1",   ModuleKind::TrackingEnhancer, SlotType::Low, 5, 12, DefenseLayer::Shield, 0.3f, 0,    0);
    mod("module.afterburner.t1",   ModuleKind::Afterburner,   SlotType::Mid,  10, 10, DefenseLayer::Shield,  1.4f, 0,     0);

    // Fit templates (§6 roles) — each obeys its hull's slot grid + PG/CPU budget
    // (asserted by ValidateCombatCatalog and the area-A round-trip test).
    auto fit = [&](const char* name, const char* hullCode, std::vector<std::string> mods) {
        c.fits.push_back({ name, hullCode, std::move(mods) });
    };
    fit("scout-tackle", "hull.light",
        { "module.railgun.t1", "module.warpdisruptor.t1", "module.afterburner.t1", "module.trackingenh.t1" });
    fit("fighter-em", "hull.medium",
        { "module.pulser.t1", "module.pulser.t1", "module.pulser.t1",
          "module.shieldbooster.t1", "module.web.t1", "module.afterburner.t1",
          "module.damageamp.t1", "module.trackingenh.t1", "module.armorplate.t1" });
    fit("fighter-kin", "hull.medium",
        { "module.railgun.t1", "module.railgun.t1", "module.railgun.t1",
          "module.shieldbooster.t1", "module.web.t1", "module.afterburner.t1",
          "module.damageamp.t1", "module.trackingenh.t1", "module.armorplate.t1" });
    fit("heavy-anchor", "hull.heavy",
        { "module.railgun.t1", "module.railgun.t1", "module.railgun.t1", "module.railgun.t1",
          "module.shieldbooster.t1", "module.web.t1", "module.afterburner.t1",
          "module.armorplate.t1", "module.armorplate.t1", "module.damageamp.t1", "module.trackingenh.t1" });
    fit("logi-shield", "hull.medium",
        { "module.remoteshield.t1", "module.remoteshield.t1", "module.remoteshield.t1",
          "module.shieldbooster.t1", "module.afterburner.t1", "module.web.t1",
          "module.armorplate.t1", "module.armorplate.t1" });
    fit("ewar-jam", "hull.medium",
        { "module.pulser.t1",
          "module.jammer.t1", "module.jammer.t1", "module.afterburner.t1",
          "module.damageamp.t1", "module.trackingenh.t1", "module.armorplate.t1" });
    fit("base-firesupport", "hull.capital",
        { "module.railgun.t1", "module.railgun.t1", "module.remoteshield.t1", "module.remoteshield.t1",
          "module.shieldbooster.t1", "module.shieldbooster.t1", "module.sensordamp.t1", "module.afterburner.t1",
          "module.armorplate.t1", "module.armorplate.t1", "module.armorplate.t1", "module.armorplate.t1" });

    return c;
}

} // namespace Neuron::Sim
