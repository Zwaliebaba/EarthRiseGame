#pragma once
// ReplicaManager — decodes server snapshots into a client-side entity mirror.
//
// Converts absolute int64 UniversePos → sector-relative float3 via
// FloatingOriginHelper so no int64_t ever reaches the GPU (§6, M1b criterion).
// The rendering origin rebases automatically when the tracked player moves to a
// new sector, keeping render-space offsets small (< 16 384 m).

#include "Replica.h"
#include "Snapshot.h"
#include "UniversePos.h"

#include <cstdint>
#include <span>

namespace Neuron::Client
{

class ReplicaManager
{
public:
    // Update from an encoded delta-snapshot body. Returns false on decode failure.
    // The server's live path ships the M4 quantized delta wire format (§8.4), so we
    // feed each body into the stateful decoder, which reconstructs full entity state
    // (last-writer-wins by tick, idempotent under reorder/dup) and then project the
    // accumulated set into render space.
    // playerNetId: the local player's network entity ID (used to track the
    // floating origin sector; 0 = unknown, use any entity as fallback).
    bool Update(std::span<const uint8_t> snapshotBytes, uint32_t playerNetId)
    {
        if (!m_decode.Apply(snapshotBytes)) return false;

        m_tick = m_decode.LatestTick();
        m_current.Clear();

        const auto& ents = m_decode.Entities();

        // First pass: rebase floating origin to the tracked player's sector (or any
        // entity if the player isn't known/visible yet).
        for (const auto& [netId, e] : ents) {
            if (netId == playerNetId || playerNetId == 0) {
                const auto sec = Neuron::Universe::UniverseToSector(e.pos);
                m_floatingOrigin.RebaseToSector(sec);
                break;
            }
        }

        // Second pass: project every entity into render space.
        for (const auto& [netId, e] : ents) {
            if (m_current.count >= ReplicaSet::kMaxEntities) break;

            // Combine base UniversePos with the sub-metre localOffset before projection.
            const Neuron::Universe::UniversePos universeWithOffset{
                e.pos.x + static_cast<int64_t>(e.localOffset.x),
                e.pos.y + static_cast<int64_t>(e.localOffset.y),
                e.pos.z + static_cast<int64_t>(e.localOffset.z)
            };
            // ToRenderSpace returns a float3 relative to the floating origin —
            // no int64_t is propagated past this point.
            const auto rs = m_floatingOrigin.ToRenderSpace(universeWithOffset);

            ReplicaEntity& r = m_current.entities[m_current.count++];
            r.networkId  = e.netId;
            r.x          = rs.x;
            r.y          = rs.y;
            r.z          = rs.z;
            r.entityType = static_cast<uint8_t>(e.kind);
            r.shapeId    = e.shapeId;
            r.hp         = e.hp;
            r.ownerPlayer = e.ownerPlayer;
            r.valid      = true;
        }
        return true;
    }

    [[nodiscard]] const ReplicaSet& Current() const noexcept { return m_current; }
    [[nodiscard]] uint32_t          LastTick() const noexcept { return m_tick; }

    [[nodiscard]] const Neuron::Universe::FloatingOriginHelper& FloatingOrigin() const noexcept
        { return m_floatingOrigin; }

private:
    ReplicaSet                          m_current;
    Neuron::Sim::DeltaDecodeState       m_decode;       // accumulates full state from deltas
    Neuron::Universe::FloatingOriginHelper m_floatingOrigin{};
    uint32_t                            m_tick{ 0 };
};

} // namespace Neuron::Client
