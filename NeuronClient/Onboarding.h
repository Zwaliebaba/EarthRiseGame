#pragma once
// Onboarding — client-side, platform-independent objective/hint chain (playable
// slice; pulls a thin slice of M7 onboarding forward). It watches only what the
// client can actually observe — the replica set + the local selection — and walks a
// short chain of on-screen objectives, so the live universe reads as a game with
// goals instead of a sandbox demo. The chain is observation-driven (never blocks on
// economy state it can't see) and self-heals if the player does things out of order.
// No WinRT/D3D; unit-tested on the Linux runner. The EarthRise app reduces its
// replica each frame with ObserveWorld(...) and draws CurrentText() in the HUD.

#include "Components.h" // EntityKind
#include "Replica.h"

#include <cstdint>

namespace Neuron::Client
{

// The observable inputs the onboarding chain advances on.
struct ObservedState
{
    bool     hasOwnBase{ false };
    uint32_t ownedShips{ 0 };
    uint32_t selectionCount{ 0 };
    uint32_t npcVisible{ 0 };
};

// Reduce a replica set + local selection to the onboarding's observable inputs.
[[nodiscard]] inline ObservedState ObserveWorld(const ReplicaSet& set, uint32_t selfPlayer,
                                                uint32_t selectionCount)
{
    ObservedState o;
    o.selectionCount = selectionCount;
    for (uint32_t i = 0; i < set.count; ++i)
    {
        const ReplicaEntity& e = set.entities[i];
        if (!e.valid) continue;
        const auto kind = static_cast<Neuron::Sim::EntityKind>(e.entityType);
        if (kind == Neuron::Sim::EntityKind::NpcUnit) { ++o.npcVisible; continue; }
        if (e.ownerPlayer == selfPlayer && selfPlayer != 0)
        {
            if (kind == Neuron::Sim::EntityKind::Base) o.hasOwnBase = true;
            else if (kind == Neuron::Sim::EntityKind::Ship) ++o.ownedShips;
        }
    }
    return o;
}

class Onboarding
{
public:
    enum class Step : uint8_t { Welcome, Select, Engage, Clear, Done };

    // Advance the chain on this frame's observation. Monotonic — never goes back.
    void Observe(const ObservedState& s) noexcept
    {
        if (s.npcVisible > 0) m_sawNpc = true; // sticky: remembers the site was sighted
        switch (m_step)
        {
        case Step::Welcome: if (s.hasOwnBase)               m_step = Step::Select; break;
        case Step::Select:  if (s.selectionCount > 0)       m_step = Step::Engage; break;
        case Step::Engage:  if (m_sawNpc)                   m_step = Step::Clear;  break;
        case Step::Clear:   if (m_sawNpc && s.npcVisible == 0) m_step = Step::Done; break;
        case Step::Done:    break;
        }
    }

    [[nodiscard]] Step Current()  const noexcept { return m_step; }
    [[nodiscard]] bool Complete() const noexcept { return m_step == Step::Done; }

    // One-line objective text for the HUD (ASCII only — the bitmap font is ASCII).
    [[nodiscard]] const char* CurrentText() const noexcept
    {
        switch (m_step)
        {
        case Step::Welcome: return "Welcome, Commander. Right-drag to look, wheel to zoom, arrows to pan.";
        case Step::Select:  return "OBJECTIVE: press A to select your fleet.";
        case Step::Engage:  return "OBJECTIVE: click the radar (lower-left) to send your fleet at the hostiles.";
        case Step::Clear:   return "OBJECTIVE: destroy the red guardian contacts to clear the sector!";
        case Step::Done:    return "Sector cleared. Well done, Commander.";
        }
        return "";
    }

private:
    Step m_step{ Step::Welcome };
    bool m_sawNpc{ false };
};

} // namespace Neuron::Client
