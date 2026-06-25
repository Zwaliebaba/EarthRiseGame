#pragma once
// CombatTypes.h — small, dependency-free combat primitives (§13.2, M6 area A/B):
// damage types, defense layers, fitting slot types, module archetypes, hull sizes,
// and the per-layer × per-damage-type ResistProfile.
//
// Kept free of Serde/ECS so BOTH the authored game-data catalog (CombatData.h) and
// the ECS components (Components.h) share ONE set of enums with no drift — the same
// "one model, no drift" rule UniverseData/ShapeCatalog follow. Wire/cooked values
// are stable; the sets are extensible (a new damage type — e.g. Explosive, §2.2 — is
// added as data + a trailing enum value, never a format break for the existing ones).

#include <cstdint>

namespace Neuron::Sim
{

// Launch damage types (combat-balance.md §2.2). The counter triangle is encoded in
// the per-layer resist spreads (CombatData.h), not in code. Explosive is post-launch
// (a trailing enum value + a catalog row when it lands), so it is data, not a rewrite.
enum class DamageType : uint8_t { Kinetic = 0, Thermal = 1, EM = 2 };
inline constexpr uint8_t DAMAGE_TYPE_COUNT = 3;

// Three defense layers, depleted outside-in (combat-balance.md §2.1).
enum class DefenseLayer : uint8_t { Shield = 0, Armor = 1, Hull = 2 };
inline constexpr uint8_t DEFENSE_LAYER_COUNT = 3;

// Fitting slot families (combat-balance.md §5): High = weapons/remote-rep/mining,
// Mid = shield/EWAR/propulsion/tackle, Low = armor/hull/damage-tracking mods.
enum class SlotType : uint8_t { High = 0, Mid = 1, Low = 2 };
inline constexpr uint8_t SLOT_TYPE_COUNT = 3;

// Module archetypes (combat-balance.md §5; §13.2 EWAR/logi/tackle). The effect of a
// fitted module on the sim is dispatched on this kind (Combat.h). Trailing-extensible.
enum class ModuleKind : uint8_t
{
    Weapon           = 0,  // High — deals 'damageType' damage at 'baseDamage'/shot
    ShieldBooster    = 1,  // Mid  — local shield regen boost ('strength' hp/s)
    RemoteRep        = 2,  // High — logistics remote rep to 'effectLayer' at 'strength' hp/s within 'range'
    Jammer           = 3,  // Mid  — EWAR: suppress target weapons for 'duration' s within 'range'
    Web              = 4,  // Mid  — EWAR: scale target max speed by 'strength' for 'duration' s
    WarpDisruptor    = 5,  // Mid  — tackle: prevent warp ('duration' s, 'range') — interdiction (§13.12)
    SensorDamp       = 6,  // Mid  — EWAR: scale target optimal range by 'strength' for 'duration' s
    ArmorPlate       = 7,  // Low  — passive buffer: +'strength' max armor HP
    DamageAmp        = 8,  // Low  — passive: +('strength' fraction) weapon damage
    TrackingEnhancer = 9,  // Low  — passive: +'strength' weapon tracking
    Afterburner      = 10, // Mid  — passive: ×'strength' max speed
};
inline constexpr uint8_t MODULE_KIND_COUNT = 11;

// Hull size ladder (combat-balance.md §3) — drives signature / tracking interplay.
enum class HullSize : uint8_t { Light = 0, Medium = 1, Heavy = 2, Industrial = 3, Capital = 4 };

// Per-layer × per-damage-type damage-reduction fractions (combat-balance.md §2.2).
// resist[layer][type] ∈ [0,1): 0 = full damage taken, 0.4 = 40% reduced. The damage
// rule (Combat.h) reads this; the counter triangle lives entirely in these numbers.
struct ResistProfile
{
    float resist[DEFENSE_LAYER_COUNT][DAMAGE_TYPE_COUNT]{};

    [[nodiscard]] float At(DefenseLayer l, DamageType t) const noexcept
    {
        return resist[static_cast<int>(l)][static_cast<int>(t)];
    }
    void Set(DefenseLayer l, DamageType t, float v) noexcept
    {
        resist[static_cast<int>(l)][static_cast<int>(t)] = v;
    }
};

} // namespace Neuron::Sim
