#pragma once
// Economy — shared, pure sim rules for the 4X eXploit chain (§13.4, §7.2):
// harvest, deposit, build, fuel, sensor. No balance constants live here — every
// number comes from EconomyTuning (cooked game data, §12.6). Deterministic (no
// globals/time) so client/server/bots agree and record/replay reproduces runs.

#include "Components.h"
#include "Navigation.h"   // UniverseDistance
#include "UniverseData.h" // EconomyTuning, ResourceType, kResourceTypeCount
#include "UniversePos.h"

#include <algorithm>
#include <cstdint>

namespace Neuron::Sim
{

static_assert(kResourceSlots == kResourceTypeCount, "Cargo/Storage slots must match ResourceType");

// --- cargo / storage helpers -----------------------------------------------

[[nodiscard]] inline float SumSlots(const float (&a)[kResourceSlots]) noexcept
{
    float t = 0.0f;
    for (int i = 0; i < kResourceSlots; ++i) t += a[i];
    return t;
}
[[nodiscard]] inline float CargoFree(const Cargo& c) noexcept     { const float f = c.capacity - SumSlots(c.amount); return f > 0.0f ? f : 0.0f; }
[[nodiscard]] inline float StorageFree(const Storage& s) noexcept { const float f = s.capacity - SumSlots(s.amount); return f > 0.0f ? f : 0.0f; }

// --- harvest (eXploit): node yield → cargo, clamped by yield + capacity ------

// Transfer yield from a node into a harvester's cargo over dt. Clamps to the
// node's remaining yield and the cargo's free space. Returns the amount moved.
inline float HarvestStep(ResourceNodeTag& node, Cargo& cargo, float rate, float dt) noexcept
{
    const int slot = node.type < kResourceSlots ? node.type : 0;
    float want = rate * dt;
    want = std::min(want, node.remaining);
    want = std::min(want, CargoFree(cargo));
    if (want <= 0.0f) return 0.0f;
    node.remaining     -= want;
    cargo.amount[slot] += want;
    return want;
}

// Deposit a harvester's whole cargo into base storage, clamped by storage
// capacity. Returns the total transferred.
inline float DepositAll(Cargo& cargo, Storage& storage) noexcept
{
    float moved = 0.0f;
    for (int i = 0; i < kResourceSlots; ++i) {
        const float room = StorageFree(storage);
        if (room <= 0.0f) break;
        const float take = std::min(cargo.amount[i], room);
        cargo.amount[i]   -= take;
        storage.amount[i] += take;
        moved += take;
    }
    return moved;
}

// --- build (eXploit): storage → ship ---------------------------------------

// Can the base afford the basic-ship recipe right now?
[[nodiscard]] inline bool CanAfford(const Storage& s, const EconomyTuning& e) noexcept
{
    return s.amount[static_cast<int>(ResourceType::Ore)] >= e.buildOreCost &&
           s.amount[static_cast<int>(ResourceType::Ice)] >= e.buildIceCost;
}

enum class BuildResult : uint8_t { Idle = 0, InProgress, Completed, Insufficient };

// Advance a base's build queue by dt: pay the recipe cost once on start, then
// accumulate progress; signal Completed when it finishes (the caller spawns the
// ship). Insufficient resources cancel the queued build.
inline BuildResult BuildStep(BuildQueue& q, Storage& s, const EconomyTuning& e, float dt) noexcept
{
    if (!q.active) return BuildResult::Idle;
    if (!q.paid) {
        if (!CanAfford(s, e)) { q.active = false; return BuildResult::Insufficient; }
        s.amount[static_cast<int>(ResourceType::Ore)] -= e.buildOreCost;
        s.amount[static_cast<int>(ResourceType::Ice)] -= e.buildIceCost;
        q.paid = true;
    }
    q.progress += dt;
    if (q.progress >= e.buildSeconds) {
        q.active = false; q.paid = false; q.progress = 0.0f;
        return BuildResult::Completed;
    }
    return BuildResult::InProgress;
}

// --- fuel + sensor ----------------------------------------------------------

// Consume fuel; returns false and leaves it unchanged if there isn't enough.
[[nodiscard]] inline bool ConsumeFuel(Fuel& fuel, float amount) noexcept
{
    if (amount < 0.0f || fuel.current < amount) return false;
    fuel.current -= amount;
    return true;
}

// Is 'target' within 'range' metres of 'observer'? (sensor / scan range, §13.0).
[[nodiscard]] inline bool SensorDetect(const Neuron::Universe::UniversePos& observer,
                                       const Neuron::Universe::UniversePos& target, float range) noexcept
{
    return UniverseDistance(observer, target) <= static_cast<double>(range);
}

} // namespace Neuron::Sim
