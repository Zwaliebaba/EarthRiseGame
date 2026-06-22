#pragma once
// Fleet.h — shared, pure sim rules for RTS fleet command (§23.4), placeholder
// combat (§13.7) and server NPC AI (§13.7). Like Economy.h / Navigation.h these
// are deterministic (no globals/time) so client/server/bots agree and the
// record/replay harness reproduces runs (§7.2). Cross-entity sequencing (looking
// targets up by net id) lives in ServerUniverse; the per-unit maths live here.
//
// Combat is *placeholder* at M3: flat damage to the existing Health component, no
// resists / damage-types / fitting — those are M6. AI is patrol / aggro / flee /
// defend; escalation is M7.

#include "Components.h"
#include "Navigation.h" // UniverseDistance, StepToward
#include "UniversePos.h"

#include <algorithm>
#include <cstdint>

namespace Neuron::Sim
{

// --- movement orders (§23.4) ------------------------------------------------

// Steer 'tr' to satisfy a stand-off order: drive toward 'target' until within
// 'range' metres (Move/Guard with range 0 = arrive on the point; Orbit/KeepRange
// hold the ring). Returns true once the unit is within range (order can advance).
// Pure: a thin wrapper over StepToward so all sublight steering shares one path.
inline bool StepStandoff(Transform& tr, const Neuron::Universe::UniversePos& target,
                         double maxStep, double range) noexcept
{
    const double dist = UniverseDistance(tr.pos, target);
    if (dist <= range) return true;
    // Don't overshoot the stand-off ring: step at most (dist - range).
    StepToward(tr, target, std::min(maxStep, dist - range));
    return UniverseDistance(tr.pos, target) <= range;
}

// --- placeholder combat (§13.7; full model M6) ------------------------------

// Apply flat damage to Health, clamped at 0. Returns true if this blow killed it
// (hp crossed to ≤ 0 this call). No resists / damage-types at M3.
inline bool ApplyDamage(Health& hp, int32_t dmg) noexcept
{
    if (hp.hp <= 0) return false; // already dead
    hp.hp -= dmg;
    if (hp.hp <= 0) { hp.hp = 0; return true; }
    return false;
}

// Whole-unit damage a weapon deals this tick, accumulating sub-unit fractions in
// Weapon.pending so low dps × small dt still lands over time (Health is integer).
// Deterministic (pending is plain state advanced by a fixed dt). The caller gates
// this on the target being within Weapon.range.
[[nodiscard]] inline int32_t WeaponDamage(Weapon& w, float dt) noexcept
{
    w.pending += w.dps * dt;
    if (w.pending < 1.0f) return 0;
    const int32_t d = static_cast<int32_t>(w.pending);
    w.pending -= static_cast<float>(d);
    return d;
}

[[nodiscard]] inline bool InWeaponRange(const Weapon& w,
                                        const Neuron::Universe::UniversePos& self,
                                        const Neuron::Universe::UniversePos& target) noexcept
{
    return UniverseDistance(self, target) <= static_cast<double>(w.range);
}

// --- NPC AI (§13.7) — pure state transition ---------------------------------

// Decide the NPC's next state from what it can sense this tick. Flee always wins
// (self-preservation) once HP drops below the flee fraction; otherwise aggro when
// a hostile is inside the aggro ring, else fall back to defending its home/site.
// 'hasTarget' = a live hostile is known; 'targetInAggro' = it is within aggroRange.
[[nodiscard]] inline AiState NextAiState(const NpcAi& ai, bool hasTarget, bool targetInAggro,
                                         float hpFrac) noexcept
{
    if (hpFrac <= ai.fleeHpFrac) return AiState::Flee;
    if (hasTarget && targetInAggro) return AiState::Aggro;
    return AiState::Defend;
}

} // namespace Neuron::Sim
