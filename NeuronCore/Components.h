#pragma once
// Shared sim components — §13 entities. Used identically by client and server
// ECS (the masterplan mandates identical client/server sim code, §7.2).
//
// Component type IDs are assigned here as an enum and bound to types via
// NEURON_DEFINE_COMPONENT in a single TU (SimComponents.cpp on the server,
// and the client's replica TU). Keep IDs stable — they are part of the wire
// contract for snapshots.

#include "CombatData.h" // M6 area A — combat catalog types (ModuleDef) + CombatTypes
#include "Ecs.h"
#include "UniversePos.h"

#include <DirectXMath.h>
#include <cstdint>
#include <vector>

namespace Neuron::Sim
{

// Stable component IDs (≤ 64; see ECS MAX_COMPONENT_TYPES).
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
    Slot_HarvestOrder    = 17,
    Slot_FleetOrder      = 18,
    Slot_Weapon          = 19,
    Slot_NpcAi           = 20,
    // M6 combat model (areas B–G).
    Slot_DefenseLayers   = 21,
    Slot_ResistProfile   = 22,
    Slot_Fitting         = 23,
    Slot_EwarStatus      = 24,
    Slot_Projectile      = 25,
    Slot_LootContainer   = 26,
    Slot_BaseCombat      = 27,
    Slot_HullInfo        = 28,
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
// smooth sub-metre motion (§6.1). The offset is kept in [0, SECTOR_SIZE) and
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
    uint16_t   value{ 0xFFFF };            // INVALID_SHAPE_ID
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
// RESOURCE_SLOTS must match UniverseData::RESOURCE_TYPE_COUNT (asserted in Economy.h).
inline constexpr int RESOURCE_SLOTS = 3;
struct Cargo   { float amount[RESOURCE_SLOTS]{}; float capacity{ 0.0f }; };
struct Storage { float amount[RESOURCE_SLOTS]{}; float capacity{ 0.0f }; };

// Per-base build queue (§13.4 economy chain). M3: one slot building one recipe —
// consumes Storage, advances over time, spawns a ship at the base on completion.
struct BuildQueue { bool active{ false }; bool paid{ false }; uint8_t recipe{ 0 }; float progress{ 0.0f }; };

// Fleet membership / control-group tag (§23.4). Light server-side group state;
// the client maps groups to selection sets.
struct FleetMember { uint8_t group{ 0 }; };

// Sensor range in metres (§13.0 eXplore). Drives per-player detection / fog (area E).
struct Sensor { float range{ 0.0f }; };

// Harvest auto-pilot order (§13.0 eXploit). The harvest system drives a harvester
// node → base → node: travel to the node, mine its yield into Cargo, return, and
// deposit into the base Storage — the closed "harvest → return → build" loop.
enum class HarvestPhase : uint8_t { Idle = 0, ToNode = 1, Harvesting = 2, ToBase = 3, Depositing = 4 };
struct HarvestOrder
{
    HarvestPhase phase{ HarvestPhase::Idle };
    uint32_t     nodeNetId{ 0 }; // target resource node
    uint32_t     baseNetId{ 0 }; // home base to deposit at
};

// --- fleet command (§8.4 / §23.4) — RTS intents, server-validated ----------

// The concrete movement/combat order a commandable unit (player ship or NPC) is
// executing. Player ships get these from validated FleetCommands (area B); NPCs
// get them from the server AI (area F). One order machine drives both. Warp/Jump/
// Harvest are one-shot intents routed to their own systems (Navigation/Harvest),
// not stored here. KeepRange/Orbit hold at 'range' metres from the target.
enum class OrderType : uint8_t
{
    Idle = 0, Move = 1, Attack = 2, Guard = 3, Orbit = 4, KeepRange = 5, Retreat = 6
};

struct FleetOrderEntry
{
    OrderType                     type{ OrderType::Idle };
    uint32_t                      targetNetId{ 0 };  // entity target (Attack/Guard/Orbit/KeepRange)
    Neuron::Universe::UniversePos targetPoint{};     // point target (Move/Retreat)
    float                         range{ 0.0f };     // orbit / keep-range stand-off distance
};

// A unit's current order + a shift-chained queue (§23.4 "commands queue"). The
// queue is drained front-to-back as each order completes. In-memory at M3 (M5
// snapshots a flattened form).
struct FleetOrder
{
    FleetOrderEntry              current{};
    std::vector<FleetOrderEntry> queue;
};

// Placeholder weapon (§13.7 PvE; full fitting/resist model is M6). Applies flat
// 'dps' to a target's Health while it is within 'range' and the unit's FleetOrder
// is Attack. No damage types / resists at M3. 'pending' carries sub-unit fractional
// damage between ticks so low-dps weapons still land at the 30 Hz step (Health is
// integer); it is server-internal (not replicated).
struct Weapon { float range{ 0.0f }; float dps{ 0.0f }; float pending{ 0.0f }; };

// Server NPC AI (§13.7) — patrol/aggro/flee/defend over a hand-placed site. NPCs
// are server ECS entities (distinct from ERHeadless bots, §10.3). The AI writes
// the NPC's FleetOrder (move home / attack) so combat + movement reuse one path.
enum class AiState : uint8_t { Patrol = 0, Aggro = 1, Flee = 2, Defend = 3 };
struct NpcAi
{
    AiState                       state{ AiState::Patrol };
    Neuron::Universe::UniversePos home{};        // patrol anchor / where it defends
    float                         aggroRange{ 0.0f };
    float                         fleeHpFrac{ 0.0f }; // flee below this fraction of maxHp
    uint32_t                      targetNetId{ 0 };
    uint16_t                      siteId{ 0 };        // which NPC site this guardian belongs to
};

// --- combat: layered defense + fitting (§13.2; M6 area B) -------------------

struct LayerHp { int32_t cur{ 0 }; int32_t max{ 0 }; };

// The three defense layers, depleted outside-in (combat-balance.md §2.1). This is
// the AUTHORITATIVE combat HP for ships / NPCs / the base. The single-layer Health
// (above) is kept as a synced derived MIRROR (= total cur/max) so the snapshot wire,
// SimHash, HUD and the M5 persistence mapping stay unchanged. Field order shield→
// armor→hull matches the depletion order and the schema's Shield/Armor/HullHp cols.
struct DefenseLayers
{
    LayerHp shield{}, armor{}, hull{};
    float   shieldRegenPerSec{ 0.0f };  // armor/hull have no passive regen (§2.1)
    float   regenPending{ 0.0f };        // sub-1 regen carry (HP is integer; server-internal)

    [[nodiscard]] int32_t TotalCur() const noexcept { return shield.cur + armor.cur + hull.cur; }
    [[nodiscard]] int32_t TotalMax() const noexcept { return shield.max + armor.max + hull.max; }
    [[nodiscard]] float   HullFrac() const noexcept
    {
        return hull.max > 0 ? static_cast<float>(hull.cur) / static_cast<float>(hull.max) : 0.0f;
    }
};

// ResistProfile (CombatTypes.h) is registered directly as the per-entity resist
// component — the per-layer × damage-type reduction the damage rule (Combat.h) reads.

// A fitted module instance: the resolved catalog def (copied at fit time so the pure
// combat rules + snapshots never chase the catalog) + per-instance runtime state.
struct ModuleInstance
{
    ModuleDef def{};            // resolved CombatData def (area A)
    float     cooldown{ 0.0f }; // seconds until a weapon can fire again
    float     pending{ 0.0f };  // sub-1 carry for continuous effects (remote rep), HP is integer
};

// Per-unit hull combat profile (combat-balance.md §3): signature (hit size — small =
// hard for big guns to track) + max sublight speed (drives both commanded movement and
// the tracking speed model — the size rock-paper-scissors). Read by Combat.h.
struct HullInfo
{
    float    signature{ 100.0f };
    float    maxSpeed{ 0.0f };
    HullSize size{ HullSize::Medium };
};

// A hull's fitting grid + PG/CPU budget (combat-balance.md §5). ValidateFit (area A)
// gates what gets here; the server never installs an over-budget fit (§8.4).
struct Fitting
{
    std::vector<ModuleInstance> modules;
    float   pgUsed{ 0.0f }, pgMax{ 0.0f };
    float   cpuUsed{ 0.0f }, cpuMax{ 0.0f };
    uint8_t slots[SLOT_TYPE_COUNT]{}; // High/Mid/Low capacities
};

// Active EWAR debuffs on a unit (§13.2). Each is a countdown; the EWAR system sets
// them, the combat/movement/nav systems read them, and they tick down each step.
struct EwarStatus
{
    float jammedFor{ 0.0f };         // > 0 → weapons suppressed (can't fire)
    float tackledFor{ 0.0f };        // > 0 → cannot warp (interdiction §13.12)
    float webFactor{ 1.0f };         // max-speed multiplier (1 = unaffected)
    float webFor{ 0.0f };
    float sensorDampFactor{ 1.0f };  // optimal-range multiplier (1 = unaffected)
    float sensorDampFor{ 0.0f };
};

// --- combat: projectiles & loot (§13.11; M6 areas D/G) ----------------------

// A short-lived BALLISTIC projectile entity (§13.11). Transient → lives only in the
// snapshot stream + the warm-restart blob, never normalized (§15). It travels in a
// straight line at 'vel' (it does NOT home — a homing/clamped shot could never tunnel,
// which is the whole point of area D's sub-stepping). Carries the resolved weapon stats
// so a hit resolves with no catalog lookup; 'origin' anchors the falloff distance and
// 'targetNetId' lets sub-stepping test intercept against the target's swept position.
struct Projectile
{
    uint32_t                      sourceNetId{ 0 };
    uint32_t                      targetNetId{ 0 };
    DamageType                    damageType{ DamageType::Kinetic };
    float                         baseDamage{ 0.0f };
    DirectX::XMFLOAT3             vel{ 0.0f, 0.0f, 0.0f }; // m/s, straight-line
    Neuron::Universe::UniversePos origin{};                // firing position (falloff anchor)
    float                         ttl{ 0.0f };             // seconds left before it expires (a miss)
    float                         optimal{ 0.0f }, falloff{ 0.0f }, tracking{ 0.0f };
};

// A recoverable loot drop from a destroyed ship (§13.2 loot-on-kill). Items are an
// itemised fraction of the victim's cargo/fit value over the §13.4 resource slots;
// recovery transfers them to the looter's cargo (area G). expiresAt despawns it.
struct LootContainer
{
    float items[RESOURCE_SLOTS]{};
    float expiresAt{ 0.0f }; // sim-seconds after which it despawns
};

// Base capital combat state (§13.1 disable-not-destroy). Mirrors Bases.BaseState /
// RetreatUntil — a base is forced to retreat at low hull and is NEVER destroyed.
enum class BaseState : uint8_t { Active = 0, Retreating = 1, Disabled = 2 };
struct BaseCombat
{
    BaseState state{ BaseState::Active };
    float     retreatUntil{ 0.0f }; // sim-seconds the retreat/disable cooldown ends
    bool      cargoLost{ false };   // emergency-jump cargo loss applied once
};

} // namespace Neuron::Sim
