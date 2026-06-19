#pragma once
// Shared sim components — §13 entities. Used identically by client and server
// ECS (the masterplan mandates identical client/server sim code, §7.2).
//
// Component type IDs are assigned here as an enum and bound to types via
// NEURON_DEFINE_COMPONENT in a single TU (SimComponents.cpp on the server,
// and the client's replica TU). Keep IDs stable — they are part of the wire
// contract for snapshots.

#include "ecs/Ecs.h"
#include "world/WorldPos.h"

#include <DirectXMath.h>
#include <cstdint>

namespace Neuron::Sim
{

// Stable component IDs (≤ 64; see ECS kMaxComponentTypes).
enum ComponentSlot : uint8_t
{
    Slot_Transform = 0,
    Slot_Velocity  = 1,
    Slot_BaseTag   = 2,
    Slot_ShipTag   = 3,
    Slot_NetId     = 4,
    Slot_Health    = 5,
};

// Entity kinds carried in snapshots (matches §13 entity list).
enum class EntityKind : uint8_t
{
    Unknown   = 0,
    Base      = 1,
    Ship      = 2,
    NpcUnit   = 3,
    ResourceNode = 4,
    Projectile= 5,
    LootContainer = 6,
};

// --- Components ---

// Absolute world position (int64 metres) + sector-local float offset for
// smooth sub-metre motion (§6.1). The offset is kept in [0, kSectorSize) and
// rebased into 'pos' by the movement system when it leaves the sector.
struct Transform
{
    Neuron::World::WorldPos pos{};      // authoritative integer position
    DirectX::XMFLOAT3       localOffset{ 0, 0, 0 }; // fractional metres within sector
};

struct Velocity
{
    DirectX::XMFLOAT3 metresPerSecond{ 0, 0, 0 };
};

struct BaseTag { uint8_t _pad{ 0 }; }; // marks the mobile home base
struct ShipTag { uint8_t shipType{ 0 }; };

// Network identity assigned by the server; stable across snapshots.
struct NetId { uint32_t value{ 0 }; };

struct Health { int32_t hp{ 100 }; int32_t maxHp{ 100 }; };

} // namespace Neuron::Sim
