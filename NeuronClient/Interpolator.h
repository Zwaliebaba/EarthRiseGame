#pragma once
// Interpolator — blends between two snapshot states for smooth rendering (§8.4).
// M0 skeleton: snap-on-ack (no prediction); predict/reconcile deferred to post-M1.
//
// M4 area H (§7.2/§8.5): the server publishes its time-dilation factor in the clock-
// sync echo; when overloaded it slows in-game time toward a floor. The interpolation
// clock must advance against that *dilated authoritative clock*, not wall-clock, or
// it over-extrapolates between snapshots while the server is dilated. AdvanceClock
// scales the per-frame alpha advance by the dilation factor so two snapshots a fixed
// number of *server* ticks apart still blend smoothly while real-time is stretched.

#include "Replica.h"

#include <cstdint>

namespace Neuron::Client
{

// Two-state interpolation buffer: previous and current snapshot.
struct InterpBuffer
{
    ReplicaSet prev;    // state at t-1 snapshot
    ReplicaSet curr;    // state at t   snapshot
    float      alpha;   // blend factor [0, 1) from FixedStepAccumulator::GetAlpha()

    // Server time-dilation factor from the last clock-sync (Session::ServerDilation);
    // 1.0 = full speed. Tracked so AdvanceClock interpolates on dilated server time.
    float      serverDilation{ 1.0f };

    // Advance: discard prev, make curr the new prev, zero alpha.
    void Advance(const ReplicaSet& next) noexcept
    {
        prev  = curr;
        curr  = next;
        alpha = 0.0f;
    }

    // Record the server's current dilation factor (from the clock-sync echo, §8.5).
    void SetServerDilation(float factor) noexcept
    {
        serverDilation = factor <= 0.0f ? 0.0f : (factor > 1.0f ? 1.0f : factor);
    }

    // Advance the interpolation clock by 'dtSeconds' of real time, scaled by the
    // server's dilation factor and normalised to the snapshot spacing 'stepSeconds'
    // (the server snapshot cadence). Clamps alpha at 1 so a late snapshot snaps
    // rather than extrapolating (snap-on-ack). When the server is dilated to e.g.
    // 0.1× speed, alpha advances 10× slower — matching the slowed authoritative clock.
    void AdvanceClock(float dtSeconds, float stepSeconds) noexcept
    {
        if (stepSeconds <= 0.0f) return;
        alpha += (dtSeconds * serverDilation) / stepSeconds;
        if (alpha > 1.0f) alpha = 1.0f;
    }

    // Lerp position of a single entity (snap-on-ack if prediction is off).
    // Returns false if the entity is absent in curr.
    bool GetInterpolatedPos(uint32_t networkId,
                             float& outX, float& outY, float& outZ) const noexcept
    {
        const ReplicaEntity* c = nullptr;
        for (uint32_t i = 0; i < curr.count; ++i) {
            if (curr.entities[i].networkId == networkId) { c = &curr.entities[i]; break; }
        }
        if (!c || !c->valid) return false;

        const ReplicaEntity* p = nullptr;
        for (uint32_t i = 0; i < prev.count; ++i) {
            if (prev.entities[i].networkId == networkId) { p = &prev.entities[i]; break; }
        }

        if (p && p->valid) {
            outX = p->x + (c->x - p->x) * alpha;
            outY = p->y + (c->y - p->y) * alpha;
            outZ = p->z + (c->z - p->z) * alpha;
        } else {
            // Snap on first appearance (snap-on-ack).
            outX = c->x; outY = c->y; outZ = c->z;
        }
        return true;
    }
};

} // namespace Neuron::Client
