#pragma once
// Shared sim components — §13 entities. Used identically by client and server
// ECS (the masterplan mandates identical client/server sim code, §7.2).
//
// Component type IDs are assigned here as an enum and bound to types via
// NEURON_DEFINE_COMPONENT in a single TU (SimComponents.cpp on the server,
// and the client's replica TU). Keep IDs stable — they are part of the wire
// contract for snapshots.

#include "Ecs.h"
#include "UniversePos.h"

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
    Slot_ShapeId   = 6,
    Slot_Fuel      = 7,
    Slot_NavState  = 8,
    Slot_BeaconTag = 9,
    Slot_OwnerId         = 10,
    Slot_ResourceNodeTag = 11,
    Slot_Cargo           = 12,
    Slot_Storage         = 13,
    Slot_BuildQueue      = 14,
    Slot_FleetMember     = 15,
    Slot_Sensor          = 16,
};

// Entity kinds carried in snapshots (matches §13 entity list). The first seven
// are the original gameplay roles; the trailing ones classify the static/scenery
// catalog families (see ShapeCatalog.h ShapeCategory→EntityKind) so the client
// can pick sensible default colours/scales per family.
enum class EntityKind : uint8_t
{
    Unknown   = 0,
    Base      = 1,
    Ship      = 2,
    NpcUnit   = 3,
    ResourceNode = 4,
    Projectile= 5,
    LootContainer = 6,
    Station    = 7,
    Asteroid   = 8,
    Debris     = 9,
    Decoration = 10,
    Structure  = 11, // jumpgates, special platforms/devices
};

// --- Components ---

// Absolute universe position (int64 metres) + sector-local float offset for
// smooth sub-metre motion (§6.1). The offset is kept in [0, kSectorSize) and
// rebased into 'pos' by the movement system when it leaves the sector.
struct Transform
{
    Neuron::Universe::UniversePos pos{};      // authoritative integer position
    DirectX::XMFLOAT3       localOffset{ 0, 0, 0 }; // fractional metres within sector
};

struct Velocity
{
    DirectX::XMFLOAT3 metresPerSecond{ 0, 0, 0 };
};

struct BaseTag { uint8_t _pad{ 0 }; }; // marks the mobile home base
struct ShipTag { uint8_t shipType{ 0 }; };

// Render identity for a replicated entity: an index into the ShapeCatalog
// (ShapeCatalog.h, selects the mesh + diffuse) plus the entity kind the snapshot
// carries (colour/scale class on the client). Both are replicated.
struct ShapeId
{
    uint16_t   value{ 0xFFFF };            // kInvalidShapeId
    EntityKind kind{ EntityKind::Unknown };
};

// Network identity assigned by the server; stable across snapshots.
struct NetId { uint32_t value{ 0 }; };

struct Health { int32_t hp{ 100 }; int32_t maxHp{ 100 }; };

// --- navigation (§13.12) — server-authoritative; not replicated at M3 (the
//     fuel/cooldown HUD is area G, which extends the snapshot later) ---

// Jump-drive fuel. A harvested/crafted resource (§13.4) and the economy sink for
// long-haul travel; running dry strands a fleet/base (a real exploration hazard).
struct Fuel { float current{ 0.0f }; float max{ 0.0f }; };

// Travel state machine (§13.12): sublight → align → warp (interdictable), and the
// jump spool-up (a vulnerability window) → cooldown cycle. Drives the Transform
// during travel; all but plain sublight are sim-stepped so they can be interdicted.
enum class NavPhase : uint8_t { Idle = 0, Align = 1, Warp = 2, Spool = 3, Cooldown = 4 };

struct NavState
{
    NavPhase                      phase{ NavPhase::Idle };
    bool                          interdicted{ false }; // tackle/disruptor interrupts warp/spool
    uint16_t                      jumpTarget{ 0xFFFF };  // beacon index for the active jump
    Neuron::Universe::UniversePos target{};              // warp destination / jump arrival
    float                         timer{ 0.0f };         // seconds left in align/spool/cooldown
    float                         warpSpeed{ 0.0f };     // m/s for the active warp
};

// Marks a jump-beacon entity; indexes into the cooked UniverseDataset for its
// links/kind/region (BeaconDef). Kept index-only so Components.h has no data dep.
struct BeaconTag { uint16_t beaconIndex{ 0xFFFF }; };

// --- economy & fleet (§13.1, §13.4) — server-authoritative; in-memory at M3 ---

// Owning player (a session/base net id; 0 = NPC/unowned). Ties a fleet + base to
// a player (§13.8) so the server can enforce "command only your own" (§8.4).
struct OwnerId { uint32_t player{ 0 }; };

// A harvestable resource node (§13.11): one resource type with a remaining yield
// the harvest rule decrements. 'type' holds a ResourceType value (Ore/Ice/Gas).
struct ResourceNodeTag { uint8_t type{ 0 }; float remaining{ 0.0f }; };

// Itemised cargo / storage (§13.4). Index = ResourceType value (Ore=0/Ice=1/Gas=2);
// kResourceSlots must match UniverseData::kResourceTypeCount (asserted in Economy.h).
inline constexpr int kResourceSlots = 3;
struct Cargo   { float amount[kResourceSlots]{}; float capacity{ 0.0f }; };
struct Storage { float amount[kResourceSlots]{}; float capacity{ 0.0f }; };

// Per-base build queue (§13.4 economy chain). M3: one slot building one recipe —
// consumes Storage, advances over time, spawns a ship at the base on completion.
struct BuildQueue { bool active{ false }; bool paid{ false }; uint8_t recipe{ 0 }; float progress{ 0.0f }; };

// Fleet membership / control-group tag (§23.4). Light server-side group state;
// the client maps groups to selection sets.
struct FleetMember { uint8_t group{ 0 }; };

// Sensor range in metres (§13.0 eXplore). Drives per-player detection / fog (area E).
struct Sensor { float range{ 0.0f }; };

} // namespace Neuron::Sim
