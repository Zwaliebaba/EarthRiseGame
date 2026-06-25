#pragma once
// Picking — client-side, platform-independent selection hit-testing (playable slice).
// The EarthRise app projects each owned unit to screen with DirectXMath (Windows) and
// hands the resulting 2D points here to resolve a click (nearest unit within a radius)
// or a drag-box (every unit inside the rectangle). Kept free of WinRT/D3D so the pick
// decision unit-tests on the Linux runner; only the projection lives in the app.

#include <cstdint>
#include <span>
#include <vector>

namespace Neuron::Client
{

struct ScreenPoint
{
    uint32_t id{ 0 };
    float    x{ 0.0f }, y{ 0.0f };
    bool     visible{ false }; // off-screen / behind the camera → not pickable
};

// Nearest visible point to (px,py) within 'radiusPx'; 0 if nothing is in range.
[[nodiscard]] inline uint32_t PickNearest(std::span<const ScreenPoint> pts,
                                          float px, float py, float radiusPx)
{
    uint32_t best = 0;
    float    bestD2 = radiusPx * radiusPx;
    for (const ScreenPoint& p : pts)
    {
        if (!p.visible) continue;
        const float dx = p.x - px, dy = p.y - py;
        const float d2 = dx * dx + dy * dy;
        if (d2 <= bestD2) { bestD2 = d2; best = p.id; }
    }
    return best;
}

// Ids of every visible point inside the rectangle (corners in any order). 'out' is
// cleared first.
inline void PickBox(std::span<const ScreenPoint> pts, float x0, float y0, float x1, float y1,
                    std::vector<uint32_t>& out)
{
    const float lx = x0 < x1 ? x0 : x1, hx = x0 < x1 ? x1 : x0;
    const float ly = y0 < y1 ? y0 : y1, hy = y0 < y1 ? y1 : y0;
    out.clear();
    for (const ScreenPoint& p : pts)
    {
        if (!p.visible) continue;
        if (p.x >= lx && p.x <= hx && p.y >= ly && p.y <= hy) out.push_back(p.id);
    }
}

// A left-drag becomes a box-select once it exceeds this; below it, it's a click.
inline constexpr float DRAG_THRESHOLD_PX = 6.0f;

[[nodiscard]] inline bool IsDrag(float x0, float y0, float x1, float y1) noexcept
{
    const float dx = x1 - x0, dy = y1 - y0;
    return (dx * dx + dy * dy) > DRAG_THRESHOLD_PX * DRAG_THRESHOLD_PX;
}

} // namespace Neuron::Client
