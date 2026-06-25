#pragma once
// FleetControl — client-side, platform-independent fleet-command logic (§23.4):
// the smart context-action resolver, control groups, and the overview contact
// list. The client turns selection + a target into a concrete FleetCommand; the
// SERVER still validates and applies it (§8.4 — these helpers never mutate sim
// state). Kept free of DirectX/WinRT so it builds and tests on Linux (the
// EarthRise UWP app wires pointer/key input into these).

#include "Command.h"   // IntentType, FleetCommand (NeuronCore)
#include "Components.h" // EntityKind
#include "Replica.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Neuron::Client
{

// IFF / target classification for the smart action (§23.4). Derived from a
// contact's EntityKind + ownership relative to the local player.
enum class SmartTarget : uint8_t { EmptySpace = 0, Ally, Enemy, ResourceNode, Beacon, Loot };

// Classify a replicated contact for the smart action. NPCs (owner 0) and other
// players' combat units read as Enemy; your own units read as Ally.
[[nodiscard]] inline SmartTarget ClassifyTarget(Neuron::Sim::EntityKind kind,
                                                uint32_t ownerPlayer, uint32_t selfPlayer) noexcept
{
    using K = Neuron::Sim::EntityKind;
    switch (kind) {
    case K::ResourceNode:  return SmartTarget::ResourceNode;
    case K::LootContainer: return SmartTarget::Loot;           // recoverable kill loot
    case K::Structure:     return SmartTarget::Beacon;         // jump beacon / gate
    case K::Base:
    case K::Ship:         return (ownerPlayer == selfPlayer && selfPlayer != 0)
                                 ? SmartTarget::Ally : SmartTarget::Enemy;
    case K::NpcUnit:      return SmartTarget::Enemy;
    default:              return SmartTarget::EmptySpace;       // scenery → treat as empty
    }
}

// The smart single action (§23.1): empty space = move, enemy = attack, node =
// harvest, beacon = warp/jump, ally = guard/assist, loot = claim.
[[nodiscard]] inline Neuron::Sim::IntentType ResolveSmartAction(SmartTarget t) noexcept
{
    using I = Neuron::Sim::IntentType;
    switch (t) {
    case SmartTarget::Enemy:        return I::Attack;
    case SmartTarget::ResourceNode: return I::Harvest;
    case SmartTarget::Beacon:       return I::Jump;
    case SmartTarget::Ally:         return I::Guard;
    case SmartTarget::Loot:         return I::ClaimLoot;
    case SmartTarget::EmptySpace:   return I::Move;
    }
    return I::Move;
}

// Build the FleetCommand for a smart action against a (possibly empty) target.
// 'units' are the locally-selected owned net ids; the server re-checks ownership.
// A Beacon target resolves to a Jump, which the server keys by beacon *name* (not
// net id) — so the caller must supply 'beaconName' (the starmap has it); a Jump
// with an empty name is rejected server-side.
[[nodiscard]] inline Neuron::Sim::FleetCommand
MakeSmartCommand(SmartTarget t, const std::vector<uint32_t>& units,
                 uint32_t targetNetId, Neuron::Universe::UniversePos targetPoint,
                 bool queue = false, std::string beaconName = {})
{
    Neuron::Sim::FleetCommand c;
    c.intent = ResolveSmartAction(t);
    c.units  = units;
    c.queue  = queue;
    if (t == SmartTarget::EmptySpace)   c.targetPoint = targetPoint;
    else if (t == SmartTarget::Beacon)  c.beacon      = std::move(beaconName);
    else                                c.targetNetId = targetNetId;
    return c;
}

// --- control groups (§23.2 Ctrl+# set / # recall) ---------------------------

// Ten client-side selection sets. Pure UI state — the server stores no grouping;
// recall just yields the member net ids to put in a command's 'units' (§23.4).
class ControlGroups
{
public:
    static constexpr int kCount = 10;

    void Set(int group, std::vector<uint32_t> units)
    {
        if (group < 0 || group >= kCount) return;
        std::sort(units.begin(), units.end());
        units.erase(std::unique(units.begin(), units.end()), units.end());
        m_groups[static_cast<size_t>(group)] = std::move(units);
    }

    [[nodiscard]] const std::vector<uint32_t>& Recall(int group) const
    {
        static const std::vector<uint32_t> kEmpty;
        if (group < 0 || group >= kCount) return kEmpty;
        return m_groups[static_cast<size_t>(group)];
    }

    // Drop a destroyed/gone unit from every group (call when it leaves the replica).
    void Forget(uint32_t netId)
    {
        for (auto& g : m_groups)
            g.erase(std::remove(g.begin(), g.end(), netId), g.end());
    }

private:
    std::array<std::vector<uint32_t>, kCount> m_groups;
};

// --- overview list (§22.3 primary surface) ----------------------------------

// One row in the overview. IFF + distance let it sort/filter like EVE's overview.
struct OverviewContact
{
    uint32_t                netId{ 0 };
    Neuron::Sim::EntityKind kind{ Neuron::Sim::EntityKind::Unknown };
    SmartTarget             iff{ SmartTarget::EmptySpace };
    int32_t                 hp{ 0 };
    double                  distance{ 0.0 }; // render-space metres from the camera focus
};

// Build the overview from the current replica: classify IFF, compute distance to
// 'focus' (e.g. the player's own base in render space), and sort nearest-first.
// 'kindMask' bit i set ⇒ include EntityKind i (0 = include all) — the filter
// (§22.3). Operates on the already-fog-filtered replica (area E).
[[nodiscard]] inline std::vector<OverviewContact>
BuildOverview(const ReplicaSet& rs, uint32_t selfPlayer,
              float focusX, float focusY, float focusZ, uint64_t kindMask = 0)
{
    std::vector<OverviewContact> out;
    for (uint32_t i = 0; i < rs.count; ++i) {
        const ReplicaEntity& e = rs.entities[i];
        if (!e.valid) continue;
        if (kindMask != 0 && !((kindMask >> e.entityType) & 1ull)) continue;
        const auto kind = static_cast<Neuron::Sim::EntityKind>(e.entityType);
        const double dx = e.x - focusX, dy = e.y - focusY, dz = e.z - focusZ;
        out.push_back({ e.networkId, kind, ClassifyTarget(kind, e.ownerPlayer, selfPlayer),
                        e.hp, std::sqrt(dx * dx + dy * dy + dz * dz) });
    }
    std::sort(out.begin(), out.end(), [](const OverviewContact& a, const OverviewContact& b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        return a.netId < b.netId; // stable tiebreak
    });
    return out;
}

} // namespace Neuron::Client
