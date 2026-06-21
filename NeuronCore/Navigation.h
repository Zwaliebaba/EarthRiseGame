#pragma once
// Navigation — warp + jump-beacon travel rules (§13.12). Pure, shared sim logic
// (§7.2): *align → warp* (sim-stepped, interdictable) and the jump *spool → cooldown*
// cycle. Server-authoritative; the system drives the Transform during travel. All
// balance comes from NavTuning (cooked game data, §12.6) — never hard-coded here.
//
// Determinism: distance uses double (to avoid int64² overflow at warp range) but
// is otherwise pure (no globals/time), so client and server step identically and
// the record/replay harness reproduces runs (§7.2).

#include "Components.h"
#include "Ecs.h"
#include "UniverseData.h" // NavTuning
#include "UniversePos.h"

#include <cmath>
#include <cstdint>

namespace Neuron::Sim
{

// Straight-line distance in metres. Pairs are only differenced within warp range,
// so the cast cannot overflow.
[[nodiscard]] inline double UniverseDistance(const Neuron::Universe::UniversePos& a,
                                             const Neuron::Universe::UniversePos& b) noexcept
{
    const double dx = static_cast<double>(b.x - a.x);
    const double dy = static_cast<double>(b.y - a.y);
    const double dz = static_cast<double>(b.z - a.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Warp travel time = fixed align + distance/speed (travel time ∝ distance, §13.12).
[[nodiscard]] inline float WarpArrivalSeconds(double distance, float warpSpeed, float alignSeconds) noexcept
{
    if (warpSpeed <= 0.0f) return alignSeconds;
    return alignSeconds + static_cast<float>(distance / static_cast<double>(warpSpeed));
}

// Step a Transform toward 'target' by up to 'maxStep' metres (sublight travel —
// used by the harvest auto-pilot, area C). Snaps + clears the local offset on
// arrival. Pure/deterministic.
inline void StepToward(Transform& tr, const Neuron::Universe::UniversePos& target, double maxStep) noexcept
{
    const double dist = UniverseDistance(tr.pos, target);
    if (dist <= 0.0) return;
    const double step = (maxStep >= dist) ? dist : maxStep;
    const double frac = step / dist;
    tr.pos.x += static_cast<int64_t>(std::llround(static_cast<double>(target.x - tr.pos.x) * frac));
    tr.pos.y += static_cast<int64_t>(std::llround(static_cast<double>(target.y - tr.pos.y) * frac));
    tr.pos.z += static_cast<int64_t>(std::llround(static_cast<double>(target.z - tr.pos.z) * frac));
    if (step >= dist) tr.localOffset = { 0.0f, 0.0f, 0.0f };
}

// Why a jump request was rejected (server validation, §8.4).
enum class JumpReject : uint8_t { Accepted = 0, NotAtBeacon, NotLinked, NoFuel, Busy };

// Pure part of jump validation (the rest — at-beacon / linked — needs the cooked
// beacon graph and lives in ServerUniverse).
[[nodiscard]] inline JumpReject CheckJumpReady(const NavState& nav, const Fuel& fuel, float cost) noexcept
{
    if (nav.phase != NavPhase::Idle) return JumpReject::Busy;   // already travelling / cooling down
    if (fuel.current < cost)         return JumpReject::NoFuel; // running dry strands you
    return JumpReject::Accepted;
}

// Begin a warp to 'dest' at 'warpSpeed' (server-authoritative; call after validation).
inline void BeginWarp(NavState& nav, const Neuron::Universe::UniversePos& dest,
                      float warpSpeed, float alignSeconds) noexcept
{
    nav.phase       = NavPhase::Align;
    nav.target      = dest;
    nav.warpSpeed   = warpSpeed;
    nav.timer       = alignSeconds;
    nav.interdicted = false;
    nav.jumpTarget  = 0xFFFF;
}

// Begin a jump to a linked beacon at 'dest' (fuel already validated + consumed).
inline void BeginJump(NavState& nav, const Neuron::Universe::UniversePos& dest,
                      uint16_t destBeaconIndex, float spoolSeconds) noexcept
{
    nav.phase       = NavPhase::Spool;
    nav.target      = dest;
    nav.jumpTarget  = destBeaconIndex;
    nav.timer       = spoolSeconds;
    nav.interdicted = false;
}

// What StepNav did this tick (for feedback hooks / tests).
enum class NavEvent : uint8_t { None = 0, Arrived, Jumped, DroppedOut };

// Advance one entity's nav state machine by dt. Moves 'tr' along the warp path;
// teleports it on jump completion; honours interdiction. Pure.
inline NavEvent StepNav(NavState& nav, Transform& tr, const NavTuning& tuning, float dt) noexcept
{
    switch (nav.phase)
    {
    case NavPhase::Idle:
        return NavEvent::None;

    case NavPhase::Align:
        nav.timer -= dt;
        if (nav.timer <= 0.0f) { nav.phase = NavPhase::Warp; nav.timer = 0.0f; }
        return NavEvent::None;

    case NavPhase::Warp:
    {
        if (nav.interdicted) { nav.interdicted = false; nav.phase = NavPhase::Idle; return NavEvent::DroppedOut; }
        const double dist = UniverseDistance(tr.pos, nav.target);
        const double step = static_cast<double>(nav.warpSpeed) * static_cast<double>(dt);
        if (dist <= 0.0 || step >= dist) {            // arrived (within one step)
            tr.pos = nav.target; tr.localOffset = { 0, 0, 0 };
            nav.phase = NavPhase::Idle;
            return NavEvent::Arrived;
        }
        const double frac = step / dist;              // advance a fraction of the remaining path
        tr.pos.x += static_cast<int64_t>(std::llround(static_cast<double>(nav.target.x - tr.pos.x) * frac));
        tr.pos.y += static_cast<int64_t>(std::llround(static_cast<double>(nav.target.y - tr.pos.y) * frac));
        tr.pos.z += static_cast<int64_t>(std::llround(static_cast<double>(nav.target.z - tr.pos.z) * frac));
        return NavEvent::None;
    }

    case NavPhase::Spool:
        if (nav.interdicted) { nav.interdicted = false; nav.phase = NavPhase::Idle; return NavEvent::DroppedOut; }
        nav.timer -= dt;
        if (nav.timer <= 0.0f) {                      // spool complete → fire the jump
            tr.pos = nav.target; tr.localOffset = { 0, 0, 0 };
            nav.phase = NavPhase::Cooldown;
            nav.timer = tuning.jumpCooldownSeconds;
            return NavEvent::Jumped;
        }
        return NavEvent::None;

    case NavPhase::Cooldown:
        nav.timer -= dt;
        if (nav.timer <= 0.0f) { nav.phase = NavPhase::Idle; nav.timer = 0.0f; }
        return NavEvent::None;
    }
    return NavEvent::None;
}

// System: step every entity that has NavState + Transform (deterministic order).
inline void NavigationSystem(Neuron::ECS::World& world, const NavTuning& tuning, float dt)
{
    world.ForEach<NavState, Transform>([&tuning, dt](NavState& nav, Transform& tr) {
        StepNav(nav, tr, tuning, dt);
    });
}

} // namespace Neuron::Sim
