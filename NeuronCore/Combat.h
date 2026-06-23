#pragma once
// Combat.h — pure, deterministic combat math (§7.2, §13.2; M6 areas C/D/E). Like
// Economy.h / Navigation.h / Fleet.h these are header-only pure functions (no globals,
// no wall-clock, fixed iteration order) so client/server/bots agree and the record/
// replay determinism harness reproduces runs. Cross-entity sequencing (looking up
// targets by net id, spawning projectile entities, applying effects across a fleet)
// lives in ServerUniverse; the per-pair maths live here, fed by the area-A catalog.
//
// Replaces the M3 placeholder Fleet.h::ApplyDamage (flat damage to a single Health
// bar) with the real model: three layers depleted outside-in, per-layer × per-type
// resists (the counter triangle), tracking/falloff range factors, shield regen, and
// remote rep + EWAR effects.

#include "CombatTypes.h"
#include "Components.h"
#include "Navigation.h" // UniverseDistance

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Neuron::Sim
{

// --- damage & defense (area C) ----------------------------------------------

// Outcome of one damage application, outermost layer that broke this blow (area C/G).
enum class DamageOutcome : uint8_t { Absorbed = 0, ShieldDown = 1, ArmorDown = 2, Killed = 3 };

// Falloff factor (combat-balance.md §2.3): 1.0 within optimal, ramping linearly down
// through the falloff band to 0 past it. Pure; 'optimal'/'falloff' come from area-A
// weapon stats (a sensor-damp EWAR scales 'optimal' before this is called, area E).
[[nodiscard]] inline float FalloffFactor(double dist, float optimal, float falloff) noexcept
{
    if (dist <= static_cast<double>(optimal)) return 1.0f;
    if (falloff <= 0.0f) return 0.0f;
    const float f = 1.0f - static_cast<float>((dist - static_cast<double>(optimal)) / static_cast<double>(falloff));
    return f <= 0.0f ? 0.0f : (f >= 1.0f ? 1.0f : f);
}

// Tracking factor (combat-balance.md §2.3): a big slow gun struggles to hit a small
// fast target. Rises with weapon 'tracking' and target 'signature', falls with target
// 'speed'. A stationary or huge target → ~1.0 (always tracked). Clamped to [0,1]. The
// kTrackingSpeedScale below is a model SHAPE constant (a unit bridge between m/s and
// signature), NOT a balance number — every per-entity input (tracking/sig/speed) is
// game data, so the size rock-paper-scissors is tuned in the catalog (area A/M).
inline constexpr float kTrackingSpeedScale = 0.02f;
[[nodiscard]] inline float TrackingFactor(float tracking, float targetSignature, float targetSpeed) noexcept
{
    const float track = tracking * targetSignature;
    const float denom = track + targetSpeed * kTrackingSpeedScale;
    if (denom <= 0.0f) return 1.0f;
    const float f = track / denom;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

// Apply already-resolved effective base damage of one type, depleting the layers
// outside-in (shield → armor → hull). Each layer applies ITS OWN resist to the typed
// damage; raw overflow past a depleted layer rolls to the next at that layer's resist
// (combat-balance.md §2.1/§2.3). Returns the outermost layer that broke this blow.
[[nodiscard]] inline DamageOutcome ApplyTypedDamage(DefenseLayers& d, const ResistProfile& resist,
                                                    DamageType type, float effectiveBase) noexcept
{
    float raw = effectiveBase;
    if (raw <= 0.0f) return DamageOutcome::Absorbed;

    struct Lref { LayerHp& l; DefenseLayer which; DamageOutcome broke; };
    Lref layers[kDefenseLayerCount] = {
        { d.shield, DefenseLayer::Shield, DamageOutcome::ShieldDown },
        { d.armor,  DefenseLayer::Armor,  DamageOutcome::ArmorDown },
        { d.hull,   DefenseLayer::Hull,   DamageOutcome::Killed },
    };

    DamageOutcome outcome = DamageOutcome::Absorbed;
    for (int i = 0; i < kDefenseLayerCount && raw > 0.0f; ++i) {
        LayerHp& layer = layers[i].l;
        if (layer.cur <= 0) continue;
        const float mult = 1.0f - resist.At(layers[i].which, type); // applied per raw point
        const float effective = raw * mult;
        if (effective < static_cast<float>(layer.cur)) {
            layer.cur -= static_cast<int32_t>(std::lround(effective));
            if (layer.cur < 0) layer.cur = 0;
            raw = 0.0f;
        } else {
            const float rawToBreak = (mult > 0.0f) ? (static_cast<float>(layer.cur) / mult) : raw;
            layer.cur = 0;
            raw = (rawToBreak < raw) ? raw - rawToBreak : 0.0f;
            outcome = layers[i].broke;
        }
    }
    return outcome;
}

// The plan's named entry point (area C): effective = base × tracking × falloff, then
// depleted through the layers. tracking/falloff are the [0,1] factors above.
[[nodiscard]] inline DamageOutcome ApplyDamage(DefenseLayers& d, const ResistProfile& resist,
                                               DamageType type, float baseDamage,
                                               float trackingFactor, float falloffFactor) noexcept
{
    return ApplyTypedDamage(d, resist, type, baseDamage * trackingFactor * falloffFactor);
}

// Passive shield regen per tick (armor/hull NEVER passively regen, §2.1). Uses the
// component's pending carry so sub-1 regen still lands at the 30 Hz step (HP integer).
inline void RegenDefenses(DefenseLayers& d, float dt) noexcept
{
    if (d.hull.cur <= 0) return; // a dead/disabled hull doesn't regen
    if (d.shield.cur >= d.shield.max || d.shieldRegenPerSec <= 0.0f) return;
    d.regenPending += d.shieldRegenPerSec * dt;
    if (d.regenPending >= 1.0f) {
        const int32_t add = static_cast<int32_t>(d.regenPending);
        d.regenPending -= static_cast<float>(add);
        d.shield.cur = std::min(d.shield.max, d.shield.cur + add);
    }
}

// Remote repair a specific layer (logistics, §13.2). Armor/hull have no passive regen
// but CAN be remote-repped. Returns HP actually restored (capped at the layer max).
inline int32_t RemoteRep(DefenseLayers& d, DefenseLayer layer, int32_t amount) noexcept
{
    LayerHp& l = (layer == DefenseLayer::Shield) ? d.shield
               : (layer == DefenseLayer::Armor)  ? d.armor : d.hull;
    if (amount <= 0 || l.cur >= l.max || l.cur <= 0 /* can't rep a popped layer outside-in */)
        return 0;
    const int32_t before = l.cur;
    l.cur = std::min(l.max, l.cur + amount);
    return l.cur - before;
}

// --- EWAR application (area E) ----------------------------------------------
//
// Each EWAR module REFRESHES its target's debuff timer to the module's duration while
// the target is in range; the debuff ticks down (TickEwar) so leaving range lets it
// expire. Idempotent per tick → order-independent, deterministic.
inline void ApplyJam(EwarStatus& s, float duration) noexcept { s.jammedFor = std::max(s.jammedFor, duration); }
inline void ApplyTackle(EwarStatus& s, float duration) noexcept { s.tackledFor = std::max(s.tackledFor, duration); }
inline void ApplyWeb(EwarStatus& s, float factor, float duration) noexcept
{
    s.webFactor = std::min(s.webFactor, factor); // strongest web wins
    s.webFor    = std::max(s.webFor, duration);
}
inline void ApplySensorDamp(EwarStatus& s, float factor, float duration) noexcept
{
    s.sensorDampFactor = std::min(s.sensorDampFactor, factor);
    s.sensorDampFor    = std::max(s.sensorDampFor, duration);
}

// Tick down a unit's active EWAR debuffs; clear factors once their timer lapses.
inline void TickEwar(EwarStatus& s, float dt) noexcept
{
    s.jammedFor  = std::max(0.0f, s.jammedFor - dt);
    s.tackledFor = std::max(0.0f, s.tackledFor - dt);
    s.webFor     = std::max(0.0f, s.webFor - dt);
    if (s.webFor <= 0.0f) s.webFactor = 1.0f;
    s.sensorDampFor = std::max(0.0f, s.sensorDampFor - dt);
    if (s.sensorDampFor <= 0.0f) s.sensorDampFactor = 1.0f;
}

[[nodiscard]] inline bool IsJammed(const EwarStatus& s) noexcept { return s.jammedFor > 0.0f; }
[[nodiscard]] inline bool IsTackled(const EwarStatus& s) noexcept { return s.tackledFor > 0.0f; }

// --- weapons & projectiles (area D) -----------------------------------------

// Whether a weapon can hit at all at this distance: inside optimal + the falloff band
// (beyond that the falloff factor is 0, so no point firing). Pure range gate.
[[nodiscard]] inline bool InEngagementRange(const ModuleDef& weapon, double dist, float sensorDampFactor) noexcept
{
    const double reach = static_cast<double>((weapon.optimal + weapon.falloff) * sensorDampFactor);
    return dist <= reach;
}

// Resolve a single shot's effective base damage at the moment of impact (area C/D):
// base × tracking × falloff. Shared by hitscan weapons and projectile impacts so both
// route through one deterministic formula.
[[nodiscard]] inline float ResolveShotDamage(DamageType /*type*/, float baseDamage,
                                             double dist, float optimal, float falloff,
                                             float tracking, float targetSignature,
                                             float targetSpeed) noexcept
{
    const float tf = TrackingFactor(tracking, targetSignature, targetSpeed);
    const float ff = FalloffFactor(dist, optimal, falloff);
    return baseDamage * tf * ff;
}

// Advance a BALLISTIC projectile one tick in N local sub-steps (§13.2 "local
// sub-stepping for fast shots"), testing intercept against the target's *swept*
// position each sub-step so a fast shot can't tunnel past a small target between the
// 30 Hz ticks. The projectile flies a straight line at 'vel' (m/s); each sub-step
// advances it by vel·subDt and checks the gap to the (interpolated) target. Returns
// true the moment it passes within 'hitRadius'; 'projPos' is left at that sub-step.
// Pure: the caller passes the target's start/end for the tick (its swept segment).
struct SubStepResult { bool hit{ false }; };

[[nodiscard]] inline SubStepResult StepProjectile(Neuron::Universe::UniversePos& projPos,
                                                  const DirectX::XMFLOAT3& vel,
                                                  const Neuron::Universe::UniversePos& targetStart,
                                                  const Neuron::Universe::UniversePos& targetEnd,
                                                  float& ttl, float dt,
                                                  double hitRadius, int subSteps) noexcept
{
    SubStepResult res;
    if (subSteps < 1) subSteps = 1;
    const float subDt = dt / static_cast<float>(subSteps);
    for (int i = 0; i < subSteps; ++i) {
        // Straight-line advance of the projectile (no homing — that is what allows
        // tunneling without sub-stepping, and what sub-stepping defeats).
        projPos.x += static_cast<int64_t>(std::llround(static_cast<double>(vel.x) * subDt));
        projPos.y += static_cast<int64_t>(std::llround(static_cast<double>(vel.y) * subDt));
        projPos.z += static_cast<int64_t>(std::llround(static_cast<double>(vel.z) * subDt));
        ttl -= subDt;
        // The target's position at THIS sub-step's end (swept-target intercept).
        const double a = static_cast<double>(i + 1) / static_cast<double>(subSteps);
        const Neuron::Universe::UniversePos tgt{
            targetStart.x + static_cast<int64_t>(std::llround(static_cast<double>(targetEnd.x - targetStart.x) * a)),
            targetStart.y + static_cast<int64_t>(std::llround(static_cast<double>(targetEnd.y - targetStart.y) * a)),
            targetStart.z + static_cast<int64_t>(std::llround(static_cast<double>(targetEnd.z - targetStart.z) * a)),
        };
        if (UniverseDistance(projPos, tgt) <= hitRadius) { res.hit = true; return res; }
        if (ttl <= 0.0f) return res;
    }
    return res;
}

} // namespace Neuron::Sim
