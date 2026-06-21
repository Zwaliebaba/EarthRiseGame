#pragma once
// Replica — client-side entity mirror, decoded from server snapshots (§8.4, §10.1).
// M0 skeleton.

#include <cstdint>

namespace Neuron::Client
{

// Replicated entity state (minimal for M0; extended in M1a with full component mirror).
struct ReplicaEntity
{
    uint32_t networkId{ 0 }; // server-assigned entity ID
    float    x{ 0 }, y{ 0 }, z{ 0 }; // sector-local position from last snapshot
    uint8_t  entityType{ 0 };
    uint16_t shapeId{ 0xFFFF }; // ShapeCatalog index (selects the mesh); kInvalidShapeId
    bool     valid{ false };
};

// ReplicaSet — a flat array of replicated entities for the client's subscribed sectors.
// In M1a+ this is populated by snapshot decoding and used by the interpolator.
struct ReplicaSet
{
    static constexpr uint32_t kMaxEntities = 512;
    ReplicaEntity entities[kMaxEntities]{};
    uint32_t      count{ 0 };

    void Clear() { count = 0; }

    ReplicaEntity* FindById(uint32_t id) noexcept
    {
        for (uint32_t i = 0; i < count; ++i)
            if (entities[i].networkId == id) return &entities[i];
        return nullptr;
    }
};

} // namespace Neuron::Client
