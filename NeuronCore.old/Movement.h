#pragma once
// Movement — shared sim rule (§7.2 "pure functions for movement").
//
// Integrates velocity into a Transform at the fixed sim step, carrying the
// fractional position in the sector-local float offset and rebasing into the
// int64 WorldPos when the offset leaves the sector (§6.1). Deterministic and
// pure (no globals, no time calls) so client and server step identically and
// the record/replay determinism harness reproduces runs exactly.

#include "Components.h"
#include "WorldPos.h"

#include <DirectXMath.h>
#include <cmath>

namespace Neuron::Sim
{

// Advance one Transform by 'dtSeconds' given a velocity. Pure function.
inline void IntegrateMovement(Transform& t, const Velocity& v, float dtSeconds) noexcept
{
    // Accumulate displacement in the sector-local float offset.
    t.localOffset.x += v.metresPerSecond.x * dtSeconds;
    t.localOffset.y += v.metresPerSecond.y * dtSeconds;
    t.localOffset.z += v.metresPerSecond.z * dtSeconds;

    // Rebase whole-sector / integer-metre carry back into the int64 position.
    // We fold the integer part of the local offset into pos and keep only the
    // fractional remainder locally, then renormalise the sector.
    auto fold = [](int64_t& posAxis, float& localAxis) {
        const float whole = std::floor(localAxis);
        posAxis  += static_cast<int64_t>(whole);
        localAxis -= whole; // now in [0, 1)
    };
    fold(t.pos.x, t.localOffset.x);
    fold(t.pos.y, t.localOffset.y);
    fold(t.pos.z, t.localOffset.z);
}

// Clamp a requested speed to a maximum (server-authoritative validation, §8.4).
inline DirectX::XMFLOAT3 ClampSpeed(DirectX::XMFLOAT3 vel, float maxSpeed) noexcept
{
    const float sq = vel.x * vel.x + vel.y * vel.y + vel.z * vel.z;
    const float maxSq = maxSpeed * maxSpeed;
    if (sq <= maxSq || sq <= 0.0f) return vel;
    const float scale = maxSpeed / std::sqrt(sq);
    return { vel.x * scale, vel.y * scale, vel.z * scale };
}

// System: integrate all entities that have Transform + Velocity. Deterministic
// iteration order is guaranteed by ECS::World::ForEach (ascending index).
inline void MovementSystem(Neuron::ECS::World& world, float dtSeconds)
{
    world.ForEach<Transform, Velocity>([dtSeconds](Transform& t, Velocity& v) {
        IntegrateMovement(t, v, dtSeconds);
    });
}

} // namespace Neuron::Sim
