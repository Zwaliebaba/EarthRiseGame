#pragma once
// Interpolator — blends between two snapshot states for smooth rendering (§8.4).
// M0 skeleton: snap-on-ack (no prediction); predict/reconcile deferred to post-M1.

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

    // Advance: discard prev, make curr the new prev, zero alpha.
    void Advance(const ReplicaSet& next) noexcept
    {
        prev  = curr;
        curr  = next;
        alpha = 0.0f;
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
