#pragma once
// Universe coordinate system — §6 of the masterplan.
//
// Absolute position: int64_t per axis, 1 unit = 1 metre, signed, origin (0,0,0).
// Sector: 2^SECTOR_SHIFT metres per side (default 14 → 16 384 m).
// Per-sector float offset: sub-metre precision ~1 mm @ S=14.
// Float vectors (RelativeVec3) are only ever produced within interest range,
// so the int64→float cast cannot overflow.

#include <DirectXMath.h>
#include <cstdint>
#include <functional> // std::hash

using namespace DirectX;

namespace Neuron::Universe
{

// ---------------------------------------------------------------------------
// Absolute position — 1 unit = 1 metre, signed, int64_t per axis.
// ---------------------------------------------------------------------------
struct UniversePos
{
    int64_t x{ 0 }, y{ 0 }, z{ 0 };
    bool operator==(const UniversePos&) const = default;
};

// ---------------------------------------------------------------------------
// Sector
// ---------------------------------------------------------------------------
static constexpr int     SECTOR_SHIFT = 14;          // sector side = 2^14 = 16 384 m
static constexpr int64_t SECTOR_SIZE  = int64_t(1) << SECTOR_SHIFT;

struct SectorId
{
    int64_t x{ 0 }, y{ 0 }, z{ 0 };
    bool operator==(const SectorId&) const = default;
};

struct SectorHash
{
    size_t operator()(const SectorId& s) const noexcept
    {
        // Mix three 64-bit values. Uses the same mix as FNV-style but with
        // multiplication to spread bits — good enough for a hash-map key.
        auto mix = [](uint64_t a, uint64_t b) -> uint64_t {
            a ^= b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2);
            return a;
        };
        const uint64_t h = mix(mix(static_cast<uint64_t>(s.x),
                                    static_cast<uint64_t>(s.y)),
                                static_cast<uint64_t>(s.z));
        return static_cast<size_t>(h);
    }
};

// ---------------------------------------------------------------------------
// Coordinate math
// ---------------------------------------------------------------------------

// Sector index for a universe coordinate (arithmetic right-shift preserves sign).
[[nodiscard]] inline SectorId UniverseToSector(const UniversePos& p) noexcept
{
    return { p.x >> SECTOR_SHIFT, p.y >> SECTOR_SHIFT, p.z >> SECTOR_SHIFT };
}

// Universe position of the sector's (0,0,0) corner.
[[nodiscard]] inline UniversePos SectorToOrigin(const SectorId& s) noexcept
{
    return { s.x << SECTOR_SHIFT, s.y << SECTOR_SHIFT, s.z << SECTOR_SHIFT };
}

// Sector-local float offset in metres [0, SECTOR_SIZE) per axis.
[[nodiscard]] inline XMFLOAT3 UniverseToLocalOffset(const UniversePos& p) noexcept
{
    const int64_t mask = SECTOR_SIZE - 1;
    // Signed modulo: use bitwise AND (works for powers of two with signed types
    // only when we want the positive remainder — adjust for negative coords).
    const int64_t lx = p.x & mask; // This gives positive remainder (C++ truncation)
    const int64_t ly = p.y & mask;
    const int64_t lz = p.z & mask;
    return { static_cast<float>(lx), static_cast<float>(ly), static_cast<float>(lz) };
}

// Rebuild UniversePos from a sector and its floating local offset.
// Call when the local float offset has left [0, SECTOR_SIZE) after physics integration.
[[nodiscard]] inline UniversePos RebuildFromSectorLocal(const SectorId& s, const XMFLOAT3& local) noexcept
{
    const UniversePos origin = SectorToOrigin(s);
    // Round to nearest integer metre; caller should rebase sector if |local| >= SECTOR_SIZE.
    return {
        origin.x + static_cast<int64_t>(local.x),
        origin.y + static_cast<int64_t>(local.y),
        origin.z + static_cast<int64_t>(local.z)
    };
}

// Relative axis delta — valid while |a - b| < 2^63.
[[nodiscard]] inline int64_t AxisDelta(int64_t a, int64_t b) noexcept { return a - b; }

// Relative float vector from 'from' to 'to'. Valid within interest range (~km).
[[nodiscard]] inline XMFLOAT3 RelativeVec3(const UniversePos& from, const UniversePos& to) noexcept
{
    return {
        static_cast<float>(AxisDelta(to.x, from.x)),
        static_cast<float>(AxisDelta(to.y, from.y)),
        static_cast<float>(AxisDelta(to.z, from.z))
    };
}

// ---------------------------------------------------------------------------
// Floating-origin rendering helper
// Per frame, pick a render origin (e.g. camera sector corner).
// Upload all entity positions as camera-relative float3 — no int64 on the GPU.
// Rebase origin when the camera moves to a new sector.
// ---------------------------------------------------------------------------
struct FloatingOriginHelper
{
    UniversePos origin;  // current render origin (sector corner or any fixed point)

    // Camera-relative float3 for an entity at 'p'. Feed directly to DX12 upload.
    [[nodiscard]] XMFLOAT3 ToRenderSpace(const UniversePos& p) const noexcept
    {
        return RelativeVec3(origin, p);
    }

    // Call when the camera's sector changes to keep offsets small.
    void RebaseToSector(const SectorId& s) noexcept
    {
        origin = SectorToOrigin(s);
    }

    // True when the camera has moved far enough to warrant a rebase
    // (defaults to one sector width — tune as needed).
    [[nodiscard]] bool NeedsRebase(const UniversePos& cameraPos) const noexcept
    {
        const XMFLOAT3 rel = RelativeVec3(origin, cameraPos);
        const float    limit = static_cast<float>(SECTOR_SIZE);
        return std::abs(rel.x) >= limit ||
               std::abs(rel.y) >= limit ||
               std::abs(rel.z) >= limit;
    }
};

} // namespace Neuron::Universe
