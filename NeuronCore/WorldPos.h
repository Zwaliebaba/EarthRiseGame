#pragma once
// World coordinate system — §6 of the masterplan.
//
// Absolute position: int64_t per axis, 1 unit = 1 metre, signed, origin (0,0,0).
// Sector: 2^kSectorShift metres per side (default 14 → 16 384 m).
// Per-sector float offset: sub-metre precision ~1 mm @ S=14.
// Float vectors (RelativeVec3) are only ever produced within interest range,
// so the int64→float cast cannot overflow.

#include <DirectXMath.h>
#include <cstdint>
#include <functional> // std::hash

using namespace DirectX;

namespace Neuron::World
{

// ---------------------------------------------------------------------------
// Absolute position — 1 unit = 1 metre, signed, int64_t per axis.
// ---------------------------------------------------------------------------
struct WorldPos
{
    int64_t x{ 0 }, y{ 0 }, z{ 0 };
    bool operator==(const WorldPos&) const = default;
};

// ---------------------------------------------------------------------------
// Sector
// ---------------------------------------------------------------------------
static constexpr int     kSectorShift = 14;          // sector side = 2^14 = 16 384 m
static constexpr int64_t kSectorSize  = int64_t(1) << kSectorShift;

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

// Sector index for a world coordinate (arithmetic right-shift preserves sign).
[[nodiscard]] inline SectorId WorldToSector(const WorldPos& p) noexcept
{
    return { p.x >> kSectorShift, p.y >> kSectorShift, p.z >> kSectorShift };
}

// World position of the sector's (0,0,0) corner.
[[nodiscard]] inline WorldPos SectorToOrigin(const SectorId& s) noexcept
{
    return { s.x << kSectorShift, s.y << kSectorShift, s.z << kSectorShift };
}

// Sector-local float offset in metres [0, kSectorSize) per axis.
[[nodiscard]] inline XMFLOAT3 WorldToLocalOffset(const WorldPos& p) noexcept
{
    const int64_t mask = kSectorSize - 1;
    // Signed modulo: use bitwise AND (works for powers of two with signed types
    // only when we want the positive remainder — adjust for negative coords).
    const int64_t lx = p.x & mask; // This gives positive remainder (C++ truncation)
    const int64_t ly = p.y & mask;
    const int64_t lz = p.z & mask;
    return { static_cast<float>(lx), static_cast<float>(ly), static_cast<float>(lz) };
}

// Rebuild WorldPos from a sector and its floating local offset.
// Call when the local float offset has left [0, kSectorSize) after physics integration.
[[nodiscard]] inline WorldPos RebuildFromSectorLocal(const SectorId& s, const XMFLOAT3& local) noexcept
{
    const WorldPos origin = SectorToOrigin(s);
    // Round to nearest integer metre; caller should rebase sector if |local| >= kSectorSize.
    return {
        origin.x + static_cast<int64_t>(local.x),
        origin.y + static_cast<int64_t>(local.y),
        origin.z + static_cast<int64_t>(local.z)
    };
}

// Relative axis delta — valid while |a - b| < 2^63.
[[nodiscard]] inline int64_t AxisDelta(int64_t a, int64_t b) noexcept { return a - b; }

// Relative float vector from 'from' to 'to'. Valid within interest range (~km).
[[nodiscard]] inline XMFLOAT3 RelativeVec3(const WorldPos& from, const WorldPos& to) noexcept
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
    WorldPos origin;  // current render origin (sector corner or any fixed point)

    // Camera-relative float3 for an entity at 'p'. Feed directly to DX12 upload.
    [[nodiscard]] XMFLOAT3 ToRenderSpace(const WorldPos& p) const noexcept
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
    [[nodiscard]] bool NeedsRebase(const WorldPos& cameraPos) const noexcept
    {
        const XMFLOAT3 rel = RelativeVec3(origin, cameraPos);
        const float    limit = static_cast<float>(kSectorSize);
        return std::abs(rel.x) >= limit ||
               std::abs(rel.y) >= limit ||
               std::abs(rel.z) >= limit;
    }
};

} // namespace Neuron::World
