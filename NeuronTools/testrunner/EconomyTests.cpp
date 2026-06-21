// Economy tests (masterplan §13.4; M3 area A) — the eXploit rules (harvest,
// deposit, build, fuel, sensor) + fleet spawning/ownership and the build-queue
// system on ServerUniverse. Pure, platform-independent, so the Linux runner
// covers them; mirrors the NeuronCoreTest cases the plan lists.
//
// Component ids are bound once per binary in ShapeCatalogTests.cpp.

#include "../datacook/UniverseSource.h" // ParseUniverseSource (+ UniverseData.h)
#include "Economy.h"
#include "ServerUniverse.h"
#include "TestRunner.h"

#include <string>

using namespace Neuron::Sim;
using namespace ertest;
using Neuron::Universe::UniversePos;

namespace
{
    constexpr int kOre = static_cast<int>(ResourceType::Ore);
    constexpr int kIce = static_cast<int>(ResourceType::Ice);

    // Tiny economy for fast build/cap tests.
    const char* kSrc =
        "region R { security = high bounds = -64 64 -64 64 -64 64 yield_mult = 1 }\n"
        "economy { fleet_cap = 2  cargo_capacity = 500  storage_capacity = 2000  harvest_rate = 100\n"
        "          sensor_range_ship = 4000  sensor_range_base = 9000  build_ore = 100  build_ice = 50\n"
        "          build_seconds = 1  build_ship_type = 1 }\n";

    void Load(ServerUniverse& su)
    {
        UniverseDataset ds;
        std::vector<std::string> errs;
        const bool ok = Neuron::Tools::ParseUniverseSource(kSrc, ds, errs);
        ER_CHECK(ok && errs.empty());
        su.LoadUniverse(ds);
    }
}

// --- pure rules -------------------------------------------------------------

ER_TEST(Economy, HarvestDepletesNodeAndFillsCargo)
{
    ResourceNodeTag node; node.type = static_cast<uint8_t>(ResourceType::Ore); node.remaining = 100.0f;
    Cargo cargo; cargo.capacity = 1000.0f;

    ER_CHECK(HarvestStep(node, cargo, 50.0f, 1.0f) == 50.0f); // rate 50 × dt 1
    ER_CHECK(node.remaining == 50.0f);
    ER_CHECK(cargo.amount[kOre] == 50.0f);

    ER_CHECK(HarvestStep(node, cargo, 1000.0f, 1.0f) == 50.0f); // clamps to remaining
    ER_CHECK(node.remaining == 0.0f);
    ER_CHECK(HarvestStep(node, cargo, 50.0f, 1.0f) == 0.0f);    // empty node
}

ER_TEST(Economy, HarvestClampsToCargoCapacity)
{
    ResourceNodeTag node; node.type = 0; node.remaining = 1000.0f;
    Cargo cargo; cargo.capacity = 30.0f;
    ER_CHECK(HarvestStep(node, cargo, 100.0f, 1.0f) == 30.0f); // wants 100, only 30 fits
    ER_CHECK(CargoFree(cargo) == 0.0f);
}

ER_TEST(Economy, DepositMovesCargoToStorageClamped)
{
    Cargo c; c.capacity = 1000.0f; c.amount[kOre] = 100.0f; c.amount[kIce] = 50.0f;
    Storage s; s.capacity = 1000.0f;
    ER_CHECK(DepositAll(c, s) == 150.0f);
    ER_CHECK(s.amount[kOre] == 100.0f && s.amount[kIce] == 50.0f);
    ER_CHECK(c.amount[kOre] == 0.0f && c.amount[kIce] == 0.0f);

    Cargo c2; c2.capacity = 1000.0f; c2.amount[kOre] = 100.0f;
    Storage s2; s2.capacity = 40.0f; // only 40 room
    ER_CHECK(DepositAll(c2, s2) == 40.0f);
    ER_CHECK(s2.amount[kOre] == 40.0f && c2.amount[kOre] == 60.0f);
}

ER_TEST(Economy, BuildPaysOnceThenCompletes)
{
    EconomyTuning e; e.buildOreCost = 100.0f; e.buildIceCost = 50.0f; e.buildSeconds = 2.0f;
    BuildQueue q; q.active = true;
    Storage s; s.capacity = 1000.0f; s.amount[kOre] = 200.0f; s.amount[kIce] = 100.0f;

    ER_CHECK(BuildStep(q, s, e, 1.0f) == BuildResult::InProgress);
    ER_CHECK(q.paid);
    ER_CHECK(s.amount[kOre] == 100.0f && s.amount[kIce] == 50.0f); // charged once
    ER_CHECK(BuildStep(q, s, e, 1.0f) == BuildResult::Completed);  // progress 2 ≥ 2
    ER_CHECK(!q.active);
}

ER_TEST(Economy, BuildRejectedWhenInsufficient)
{
    EconomyTuning e; e.buildOreCost = 100.0f; e.buildIceCost = 50.0f; e.buildSeconds = 2.0f;
    BuildQueue q; q.active = true;
    Storage s; s.capacity = 1000.0f; s.amount[kOre] = 50.0f; // not enough ore
    ER_CHECK(BuildStep(q, s, e, 1.0f) == BuildResult::Insufficient);
    ER_CHECK(!q.active);
    ER_CHECK(s.amount[kOre] == 50.0f); // not charged
}

ER_TEST(Economy, FuelAndSensorRules)
{
    Fuel f{ 100.0f, 100.0f };
    ER_CHECK(ConsumeFuel(f, 30.0f));
    ER_CHECK(f.current == 70.0f);
    ER_CHECK(!ConsumeFuel(f, 1000.0f)); // insufficient → unchanged
    ER_CHECK(f.current == 70.0f);

    const UniversePos a{ 0, 0, 0 }, nearby{ 1000, 0, 0 }, far{ 10000, 0, 0 };
    ER_CHECK(SensorDetect(a, nearby, 5000.0f));
    ER_CHECK(!SensorDetect(a, far, 5000.0f));
}

// --- ServerUniverse integration --------------------------------------------

ER_TEST(Economy, BaseHasStorageAndSensorFromTuning)
{
    ServerUniverse su;
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    ER_CHECK(su.StorageOf(base) != nullptr);
    ER_CHECK(su.StorageOf(base)->capacity == 2000.0f);
    ER_CHECK(su.BuildQueueOf(base) != nullptr);
    ER_CHECK(su.Economy().fleetCap == 2);
}

ER_TEST(Economy, FleetCapIsEnforced)
{
    ServerUniverse su;
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t player = base; // a player ≈ their base net id
    ER_CHECK(su.SpawnFleetShip(player, ServerUniverse::ShipShapeId(), { 0, 0, 0 }) != 0);
    ER_CHECK(su.SpawnFleetShip(player, ServerUniverse::ShipShapeId(), { 0, 0, 0 }) != 0);
    ER_CHECK(su.SpawnFleetShip(player, ServerUniverse::ShipShapeId(), { 0, 0, 0 }) == 0); // cap 2
    ER_CHECK(su.OwnedShipCount(player) == 2);
}

ER_TEST(Economy, BuildQueueSpawnsAShip)
{
    ServerUniverse su;
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    // stock the base storage with enough to build (needs 100 ore, 50 ice)
    su.StorageOf(base)->amount[kOre] = 200.0f;
    su.StorageOf(base)->amount[kIce] = 100.0f;

    ER_CHECK(su.EnqueueBuild(base));
    ER_CHECK(su.OwnedShipCount(base) == 0);

    su.Step(0.5f); // pays + progresses (build_seconds = 1)
    ER_CHECK(su.DrainBuildCompleted().empty());
    su.Step(0.5f); // completes → ship spawns

    auto completed = su.DrainBuildCompleted();
    ER_CHECK_EQ(completed.size(), size_t(1));
    ER_CHECK(su.OwnedShipCount(base) == 1);
    ER_CHECK(su.StorageOf(base)->amount[kOre] == 100.0f); // charged once
    ER_CHECK(!su.BuildQueueOf(base)->active);
}

ER_TEST(Economy, BuildWithEmptyStorageSpawnsNothing)
{
    ServerUniverse su;
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 }); // storage empty
    ER_CHECK(su.EnqueueBuild(base));
    su.Step(0.5f);
    ER_CHECK(su.DrainBuildCompleted().empty());
    ER_CHECK(su.OwnedShipCount(base) == 0);
    ER_CHECK(!su.BuildQueueOf(base)->active); // cancelled — insufficient
}
