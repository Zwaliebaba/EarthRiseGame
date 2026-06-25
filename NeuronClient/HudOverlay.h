#pragma once
// HudOverlay — client-side, platform-independent helpers for the in-world HUD
// overlays (playable slice): selection rings + health bars. The snapshot record
// carries an entity's *current* hp but not its max (§8.4, App. A), so a health bar
// derives the fraction from a per-kind nominal max that mirrors the server's
// placeholder combat constants (§13.7; M6 replaces these with the real fitting/
// resist model). Kept free of WinRT/D3D so it unit-tests on the Linux runner; the
// EarthRise app does the screen projection + drawing.

#include "Components.h" // EntityKind

#include <algorithm>
#include <cstdint>

namespace Neuron::Client
{

// Per-kind nominal max HP (0 = entity shows no health bar). Mirrors the M3
// placeholder values in ServerUniverse (SHIP_HP / NPC_HP / base 1000).
[[nodiscard]] inline int NominalMaxHp(Neuron::Sim::EntityKind kind) noexcept
{
    switch (kind)
    {
    case Neuron::Sim::EntityKind::Base:    return 1000;
    case Neuron::Sim::EntityKind::Ship:    return 500;
    case Neuron::Sim::EntityKind::NpcUnit: return 300;
    default:                               return 0;
    }
}

[[nodiscard]] inline bool ShowsHealthBar(Neuron::Sim::EntityKind kind) noexcept
{
    return NominalMaxHp(kind) > 0;
}

// Health fraction in [0,1] for the bar fill; -1 when the kind shows no bar.
[[nodiscard]] inline float HealthFraction(Neuron::Sim::EntityKind kind, int32_t hp) noexcept
{
    const int max = NominalMaxHp(kind);
    if (max <= 0) return -1.0f;
    return std::clamp(static_cast<float>(hp) / static_cast<float>(max), 0.0f, 1.0f);
}

} // namespace Neuron::Client
