#pragma once
// RtsCamera — client-side, platform-independent RTS camera controller (playable
// slice). Holds an orbit camera (focus point + yaw/pitch/distance) and produces an
// eye/at/up basis the renderer turns into a view matrix. Kept free of WinRT/D3D and
// of XMVECTOR math (plain scalar trig over XMFLOAT3) so it builds and unit-tests on
// Linux; the EarthRise UWP app wires pointer drag / wheel / WASD into the mutators
// and reads Eye()/At()/Up() each frame.
//
// Replaces the M1b fixed look-at (a static offset from the player's base, which made
// the live universe read as a diorama): the player can now orbit, zoom and pan, and
// "follow" snaps the focus back onto their base.

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>

namespace Neuron::Client
{

using DirectX::XMFLOAT3;

class RtsCamera
{
public:
    // Tuning (metres / radians). Distance and pitch are clamped so the camera can't
    // invert or clip through the focus; pitch stays off the poles.
    static constexpr float kMinDistance = 60.0f;
    static constexpr float kMaxDistance = 6000.0f;
    static constexpr float kMinPitch    = 0.12f; // ~7°  (near-horizon)
    static constexpr float kMaxPitch    = 1.45f; // ~83° (near top-down)

    // --- focus / follow ------------------------------------------------------

    // Snap the focus to a world point (the app calls this with the player's base
    // each frame while Follow() is on, so the camera tracks the drifting base).
    void SetFocus(const XMFLOAT3& f) noexcept { m_focus = f; }
    [[nodiscard]] const XMFLOAT3& Focus() const noexcept { return m_focus; }

    [[nodiscard]] bool Follow() const noexcept { return m_follow; }
    void SetFollow(bool on) noexcept { m_follow = on; }

    // --- mutators (input maps onto these) ------------------------------------

    // Orbit: yaw wraps; pitch clamps to [kMinPitch, kMaxPitch].
    void Rotate(float dYaw, float dPitch) noexcept
    {
        m_yaw += dYaw;
        m_pitch = std::clamp(m_pitch + dPitch, kMinPitch, kMaxPitch);
    }

    // Multiplicative zoom (factor < 1 zooms in, > 1 out) — natural for a mouse wheel.
    void Zoom(float factor) noexcept
    {
        if (factor > 0.0f) m_distance = std::clamp(m_distance * factor, kMinDistance, kMaxDistance);
    }
    // Additive zoom in metres (for key/step zoom).
    void ZoomBy(float deltaMetres) noexcept
    {
        m_distance = std::clamp(m_distance + deltaMetres, kMinDistance, kMaxDistance);
    }

    // Pan the focus in the camera's ground plane: +dRight moves it screen-right,
    // +dForward moves it away from the camera. Manual panning turns Follow() off.
    void PanWorld(float dRight, float dForward) noexcept
    {
        const float s = std::sin(m_yaw), c = std::cos(m_yaw);
        // Ground forward (eye→focus, y dropped) and its right-hand perpendicular.
        const float fx = -s, fz = -c;
        const float rx = -c, rz =  s;
        m_focus.x += rx * dRight + fx * dForward;
        m_focus.z += rz * dRight + fz * dForward;
        m_follow = false;
    }

    // --- derived basis (renderer reads these) --------------------------------

    // Eye sits 'distance' from the focus along the yaw/pitch direction (above when
    // pitch > 0). |Eye() - Focus()| == Distance() exactly, so orbiting never dollies.
    [[nodiscard]] XMFLOAT3 Eye() const noexcept
    {
        const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
        return { m_focus.x + m_distance * (cp * std::sin(m_yaw)),
                 m_focus.y + m_distance * sp,
                 m_focus.z + m_distance * (cp * std::cos(m_yaw)) };
    }
    [[nodiscard]] XMFLOAT3 At() const noexcept { return m_focus; }
    [[nodiscard]] XMFLOAT3 Up() const noexcept { return { 0.0f, 1.0f, 0.0f }; }

    [[nodiscard]] float Distance() const noexcept { return m_distance; }
    [[nodiscard]] float Yaw()      const noexcept { return m_yaw; }
    [[nodiscard]] float Pitch()    const noexcept { return m_pitch; }

private:
    XMFLOAT3 m_focus{ 0.0f, 0.0f, 0.0f };
    float    m_distance{ 380.0f };
    float    m_yaw{ 3.14159265f }; // start behind the base on -Z, looking toward +Z scenery
    float    m_pitch{ 0.45f };     // ~26° down-tilt
    bool     m_follow{ true };
};

} // namespace Neuron::Client
