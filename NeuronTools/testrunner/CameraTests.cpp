// RTS camera controller tests (playable slice). The free orbit/zoom/pan camera
// replaces the M1b fixed look-at; this exercises the platform-independent math the
// EarthRise UWP app drives from pointer/wheel/key input (§16.2 Linux mirror).

#include "RtsCamera.h"
#include "TestRunner.h"

#include <cmath>

using namespace ertest;
using Neuron::Client::RtsCamera;

namespace
{
    float Dist(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
}

ER_TEST(Camera, EyeSitsAboveAndAtFocusDistance)
{
    RtsCamera cam;
    cam.SetFocus({ 0, 0, 0 });
    const auto eye = cam.Eye();
    ER_CHECK(eye.y > 0.0f);                                  // pitched up above the focus
    ER_CHECK(std::fabs(Dist(eye, cam.Focus()) - cam.Distance()) < 0.01f); // exactly 'distance' away
}

ER_TEST(Camera, OrbitKeepsDistanceConstant)
{
    RtsCamera cam;
    cam.SetFocus({ 100, 0, 50 });
    const float d0 = cam.Distance();
    cam.Rotate(1.2f, 0.0f); // yaw a lot
    ER_CHECK(std::fabs(Dist(cam.Eye(), cam.Focus()) - d0) < 0.01f); // orbit, not dolly
}

ER_TEST(Camera, ZoomClampsToRange)
{
    RtsCamera cam;
    cam.Zoom(0.0001f); // hard zoom in
    ER_CHECK(std::fabs(cam.Distance() - RtsCamera::MIN_DISTANCE) < 0.01f);
    cam.Zoom(100000.0f); // hard zoom out
    ER_CHECK(std::fabs(cam.Distance() - RtsCamera::MAX_DISTANCE) < 0.01f);
}

ER_TEST(Camera, PitchClampsOffThePoles)
{
    RtsCamera cam;
    cam.Rotate(0.0f, +10.0f);
    ER_CHECK(std::fabs(cam.Pitch() - RtsCamera::MAX_PITCH) < 0.001f);
    cam.Rotate(0.0f, -10.0f);
    ER_CHECK(std::fabs(cam.Pitch() - RtsCamera::MIN_PITCH) < 0.001f);
}

ER_TEST(Camera, PanMovesFocusAndDisablesFollow)
{
    RtsCamera cam;
    cam.SetFollow(true);
    cam.SetFocus({ 0, 0, 0 });
    cam.PanWorld(250.0f, 0.0f);
    ER_CHECK(!cam.Follow());                       // manual pan releases the base follow
    const auto f = cam.Focus();
    ER_CHECK(std::fabs(f.x) + std::fabs(f.z) > 1.0f); // focus actually moved in the ground plane
    ER_CHECK(std::fabs(f.y) < 0.001f);                // pan stays in the ground plane
}

ER_TEST(Camera, FollowReSnapsFocusToTarget)
{
    RtsCamera cam;
    cam.SetFocus({ 4096, 10, -2048 }); // app feeds the player's base each frame
    const auto eye = cam.Eye();
    ER_CHECK(std::fabs(eye.x - 4096.0f) < cam.Distance() + 1.0f); // eye tracks the new focus
    ER_CHECK(cam.At().x == 4096.0f && cam.At().z == -2048.0f);
}
