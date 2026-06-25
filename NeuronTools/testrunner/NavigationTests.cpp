// Navigation tests (masterplan §13.12; M3 area D) — warp + jump-beacon travel,
// fuel, spool/cooldown, interdiction. Pure rules (Navigation.h) + the
// ServerUniverse integration (load the cooked beacon graph, jump across it),
// all server-authoritative and platform-independent, so the Linux runner covers
// them. Mirrors the NeuronCoreTest/ERServerTest cases the plan lists.
//
// Component ids are bound once per binary in ShapeCatalogTests.cpp.

#include "../datacook/UniverseSource.h" // ParseUniverseSource (+ UniverseData.h)
#include "Navigation.h"
#include "ServerUniverse.h"
#include "TestRunner.h"

#include <string>

using namespace Neuron::Sim;
using namespace ertest;
using Neuron::Universe::UniversePos;

namespace
{
    // Small connected graph + fast tuning for the integration tests.
    const char* SRC =
        "region R { security = high bounds = -64 64 -64 64 -64 64 yield_mult = 1 }\n"
        "beacon HUB { region = R pos = 0 0 0        links = RIM     kind = public }\n"
        "beacon RIM { region = R pos = 100000 0 0   links = HUB FAR kind = public }\n"
        "beacon FAR { region = R pos = 500000 0 0   links = RIM     kind = public }\n"
        "tuning { warp_align = 0  warp_speed_base = 10000  jump_fuel_base = 20\n"
        "         jump_spool_base = 1  jump_cooldown = 1  beacon_range = 2000  base_fuel_max = 100 }\n";

    UniverseDataset Load(ServerUniverse& su)
    {
        UniverseDataset ds;
        std::vector<std::string> errs;
        const bool ok = Neuron::Tools::ParseUniverseSource(SRC, ds, errs);
        ER_CHECK(ok && errs.empty());
        su.LoadUniverse(ds);
        return ds;
    }
}

// --- pure rules -------------------------------------------------------------

ER_TEST(Navigation, WarpTravelTimeProportionalToDistance)
{
    // align is fixed; the travel part scales linearly with distance.
    const float a1 = WarpArrivalSeconds(1000.0, 100.0f, 2.0f); // 2 + 10
    const float a2 = WarpArrivalSeconds(2000.0, 100.0f, 2.0f); // 2 + 20
    ER_CHECK((a1 - 2.0f) == 10.0f);
    ER_CHECK((a2 - 2.0f) == 2.0f * (a1 - 2.0f)); // doubling distance doubles travel time
}

ER_TEST(Navigation, WarpArrivesAtTargetExactly)
{
    NavTuning t; // unused here (StepNav reads cooldown from it)
    NavState nav; Transform tr; tr.pos = { 0, 0, 0 };
    BeginWarp(nav, { 10000, 0, 0 }, 1000.0f, 0.0f); // speed 1000 m/s, no align

    NavEvent ev = NavEvent::None;
    int guard = 0;
    while (nav.phase != NavPhase::Idle && guard++ < 1000)
        ev = StepNav(nav, tr, t, 1.0f);

    ER_CHECK(ev == NavEvent::Arrived);
    const UniversePos dest{ 10000, 0, 0 };
    ER_CHECK(tr.pos == dest);
}

ER_TEST(Navigation, CheckJumpReadyFuelAndBusy)
{
    NavState nav; Fuel f{ 100.0f, 100.0f };
    ER_CHECK(CheckJumpReady(nav, f, 20.0f) == JumpReject::Accepted);

    f.current = 10.0f; // below cost
    ER_CHECK(CheckJumpReady(nav, f, 20.0f) == JumpReject::NoFuel);

    f.current = 100.0f; nav.phase = NavPhase::Cooldown; // mid-cycle
    ER_CHECK(CheckJumpReady(nav, f, 20.0f) == JumpReject::Busy);
}

// --- ServerUniverse integration --------------------------------------------

ER_TEST(Navigation, LoadUniverseSpawnsBeacons)
{
    ServerUniverse su(false);
    Load(su);
    ER_CHECK_EQ(su.Universe().beacons.size(), size_t(3));
    ER_CHECK(su.BeaconNetId("HUB") != 0);
    ER_CHECK(su.BeaconNetId("RIM") != 0);
    ER_CHECK(su.BeaconNetId("nope") == 0);
}

ER_TEST(Navigation, JumpAcrossLinkedBeaconsConsumesFuelAndArrives)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 }); // sitting on HUB
    ER_CHECK(su.FuelOf(base)->current == 100.0f);

    const JumpReject r = su.BeginJumpTo(base, "RIM"); // HUB ↔ RIM linked
    ER_CHECK(r == JumpReject::Accepted);
    ER_CHECK(su.FuelOf(base)->current == 80.0f);      // 100 − 20
    ER_CHECK(su.NavOf(base)->phase == NavPhase::Spool);

    const UniversePos hub{ 0, 0, 0 };
    const UniversePos rim{ 100000, 0, 0 };
    su.Step(0.5f); // spool 1.0 → 0.5
    UniversePos p; ER_CHECK(su.GetBasePos(base, p));
    ER_CHECK(p == hub);                               // still at source during spool
    su.Step(0.5f); // spool fires → teleport to RIM, enter cooldown

    ER_CHECK(su.GetBasePos(base, p));
    ER_CHECK(p == rim);
    ER_CHECK(su.NavOf(base)->phase == NavPhase::Cooldown);

    su.Step(0.5f); su.Step(0.5f); // cooldown 1.0 elapses
    ER_CHECK(su.NavOf(base)->phase == NavPhase::Idle);
}

ER_TEST(Navigation, JumpRejectedWhenNotLinked)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 }); // on HUB
    // HUB links only RIM; FAR is not reachable in one jump from HUB.
    ER_CHECK(su.BeginJumpTo(base, "FAR") == JumpReject::NotLinked);
}

ER_TEST(Navigation, JumpRejectedWhenNotAtBeacon)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 50000, 0, 0 }, { 0, 0, 0 }); // far from every beacon
    ER_CHECK(su.BeginJumpTo(base, "RIM") == JumpReject::NotAtBeacon);
}

ER_TEST(Navigation, JumpRejectedWhenOutOfFuel)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    su.FuelOf(base)->current = 5.0f; // below the 20 cost
    ER_CHECK(su.BeginJumpTo(base, "RIM") == JumpReject::NoFuel);
    ER_CHECK(su.FuelOf(base)->current == 5.0f); // not charged on rejection
}

ER_TEST(Navigation, InterdictionDropsBaseOutOfWarp)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    ER_CHECK(su.BeginWarpTo(base, { 300000, 0, 0 })); // align 0, base speed 10000 m/s

    su.Step(0.1f); // Align → Warp (no move yet)
    su.Step(0.1f); // Warp: ~1000 m toward target
    UniversePos mid; ER_CHECK(su.GetBasePos(base, mid));
    ER_CHECK(mid.x > 0 && mid.x < 300000);

    su.Interdict(base);
    su.Step(0.1f); // interdicted → dropped out of warp
    ER_CHECK(su.NavOf(base)->phase == NavPhase::Idle);

    UniversePos after; ER_CHECK(su.GetBasePos(base, after));
    ER_CHECK(after.x < 300000); // never reached the destination
}

ER_TEST(Navigation, BusyUnitCannotStartASecondTravel)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    ER_CHECK(su.BeginWarpTo(base, { 300000, 0, 0 }));
    // already aligning/warping → a jump must be refused
    ER_CHECK(su.BeginJumpTo(base, "RIM") == JumpReject::Busy);
    // and a second warp too
    ER_CHECK(!su.BeginWarpTo(base, { 100000, 0, 0 }));
}

ER_TEST(Navigation, InterestPrefetchRecordsDestinationSector)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    su.BeginWarpTo(base, { 1310720, 0, 0 }); // sector x = 1310720 >> 14 = 80
    ER_CHECK(su.LastTravelSector().x == 80);
}
