// Selection hit-testing tests (playable slice). The pure pick decision (nearest
// within a radius, box containment) is what the EarthRise app calls after projecting
// owned units to screen; mirrors that platform-independent logic (§16.2).

#include "Picking.h"
#include "TestRunner.h"

#include <vector>

using namespace ertest;
using Neuron::Client::IsDrag;
using Neuron::Client::PickBox;
using Neuron::Client::PickNearest;
using Neuron::Client::ScreenPoint;

ER_TEST(Picking, NearestWithinRadiusWins)
{
    const std::vector<ScreenPoint> pts = {
        { 10, 100.f, 100.f, true },
        { 11, 130.f, 100.f, true }, // 30 px away from (105,100)
        { 12, 106.f, 100.f, true }, // 1 px away — the winner
    };
    ER_CHECK_EQ(PickNearest(pts, 105.f, 100.f, 20.f), uint32_t{ 12 });
}

ER_TEST(Picking, NothingWithinRadiusPicksNone)
{
    const std::vector<ScreenPoint> pts = { { 10, 500.f, 500.f, true } };
    ER_CHECK_EQ(PickNearest(pts, 100.f, 100.f, 20.f), uint32_t{ 0 });
}

ER_TEST(Picking, InvisiblePointsAreNotPickable)
{
    const std::vector<ScreenPoint> pts = { { 10, 100.f, 100.f, false } }; // behind camera
    ER_CHECK_EQ(PickNearest(pts, 100.f, 100.f, 20.f), uint32_t{ 0 });
    std::vector<uint32_t> box;
    PickBox(pts, 0.f, 0.f, 200.f, 200.f, box);
    ER_CHECK(box.empty());
}

ER_TEST(Picking, BoxSelectsInsidePointsAnyCornerOrder)
{
    const std::vector<ScreenPoint> pts = {
        { 1, 50.f, 50.f, true },    // inside
        { 2, 150.f, 150.f, true },  // inside
        { 3, 300.f, 300.f, true },  // outside
        { 4, 80.f, 90.f, false },   // inside but invisible → excluded
    };
    std::vector<uint32_t> box;
    PickBox(pts, 200.f, 200.f, 0.f, 0.f, box); // corners reversed on purpose
    ER_CHECK_EQ(box.size(), size_t{ 2 });
    ER_CHECK(box[0] == 1 && box[1] == 2);
}

ER_TEST(Picking, DragThresholdSeparatesClickFromBox)
{
    ER_CHECK(!IsDrag(100.f, 100.f, 102.f, 101.f)); // tiny move = click
    ER_CHECK(IsDrag(100.f, 100.f, 140.f, 130.f));  // big move = box
}
