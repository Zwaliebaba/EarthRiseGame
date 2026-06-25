// Harvest-loop tests (masterplan §13.0/§13.4; M3 area C) — the closed eXploit
// micro-loop (harvester → node → return → deposit → build) and field→node
// spawning, server-authoritative on ServerUniverse. Mirrors the NeuronCoreTest
// "full loop deterministic sequence" the plan lists.
//
// Component ids are bound once per binary in ShapeCatalogTests.cpp.

#include "../datacook/UniverseSource.h" // ParseUniverseSource (+ UniverseData.h)
#include "ServerUniverse.h"
#include "TestRunner.h"

#include <string>

using namespace Neuron::Sim;
using namespace ertest;
using Neuron::Universe::UniversePos;

namespace
{
    constexpr int ORE = static_cast<int>(ResourceType::Ore);

    // Fast economy: small cargo, instant travel, cheap ore-only ship, quick build.
    const char* ECON =
        "region R { security = high bounds = -64 64 -64 64 -64 64 yield_mult = 1 }\n"
        "economy { fleet_cap = 8  cargo_capacity = 200  storage_capacity = 10000  harvest_rate = 1000\n"
        "          build_ore = 300  build_ice = 0  build_seconds = 0.1  build_ship_type = 1\n"
        "          harvester_speed = 100000  harvest_range = 600 }\n";

    void Load(ServerUniverse& su, const char* src)
    {
        UniverseDataset ds;
        std::vector<std::string> errs;
        const bool ok = Neuron::Tools::ParseUniverseSource(src, ds, errs);
        ER_CHECK(ok && errs.empty());
        su.LoadUniverse(ds);
    }
}

ER_TEST(Harvest, FullLoopNodeToCargoToStorageToShip)
{
    ServerUniverse su(false);
    Load(su, ECON);

    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t harv = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 500, 0, 0 });
    const uint32_t node = su.SpawnResourceNode(ResourceType::Ore, 2000.0f, { 5000, 0, 0 });
    ER_CHECK(harv != 0 && node != 0);
    ER_CHECK(su.OwnedShipCount(base) == 1); // the harvester

    ER_CHECK(su.OrderHarvest(harv, node));

    // Run the loop: the harvester shuttles node↔base, ore flows node → cargo → storage.
    for (int i = 0; i < 80 && su.StorageOf(base)->amount[ORE] < 400.0f; ++i)
        su.Step(0.1f);

    ER_CHECK(su.StorageOf(base)->amount[ORE] >= 400.0f); // returned + deposited
    ER_CHECK(su.ResourceNodeOf(node)->remaining < 2000.0f); // node depleted

    // Enqueue a build off the deposited ore → a ship is born.
    ER_CHECK(su.EnqueueBuild(base));
    for (int i = 0; i < 20 && su.OwnedShipCount(base) < 2; ++i)
        su.Step(0.1f);

    ER_CHECK(su.OwnedShipCount(base) == 2); // harvester + the built ship
    ER_CHECK(!su.DrainBuildCompleted().empty());
}

ER_TEST(Harvest, HarvesterDepositsThenIdlesWhenNodeEmpty)
{
    ServerUniverse su(false);
    Load(su, ECON);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t harv = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 100, 0, 0 });
    const uint32_t node = su.SpawnResourceNode(ResourceType::Ore, 150.0f, { 2000, 0, 0 }); // less than one cargo
    ER_CHECK(su.OrderHarvest(harv, node));

    for (int i = 0; i < 40 && su.HarvestOrderOf(harv)->phase != HarvestPhase::Idle; ++i)
        su.Step(0.1f);

    ER_CHECK(su.HarvestOrderOf(harv)->phase == HarvestPhase::Idle); // finished (node drained)
    ER_CHECK(su.ResourceNodeOf(node)->remaining == 0.0f);
    ER_CHECK(su.StorageOf(base)->amount[ORE] == 150.0f);          // all of it banked
    ER_CHECK(su.CargoOf(harv)->amount[ORE] == 0.0f);              // cargo emptied on deposit
}

ER_TEST(Harvest, OrderHarvestValidates)
{
    ServerUniverse su(false);
    Load(su, ECON);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t harv = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 0, 0, 0 });
    const uint32_t node = su.SpawnResourceNode(ResourceType::Ore, 100.0f, { 1000, 0, 0 });

    ER_CHECK(su.OrderHarvest(harv, node));      // ok
    ER_CHECK(!su.OrderHarvest(harv, base));     // base is not a resource node
    ER_CHECK(!su.OrderHarvest(node, node));     // a node can't harvest (no Cargo/Owner)
    ER_CHECK(!su.OrderHarvest(99999, node));    // unknown ship
}

ER_TEST(Harvest, FieldsSpawnResourceNodes)
{
    ServerUniverse su(false);
    Load(su,
        "region R { security = high bounds = -64 64 -64 64 -64 64 yield_mult = 1 }\n"
        "field BELT { region = R center = 0 0 0 radius = 1000 nodes = Ore:0.6 Ice:0.4\n"
        "            count = 4 4 yield = 100 200 respawn = 60 }\n");

    const Snapshot snap = su.BuildSnapshot();
    int nodes = 0;
    for (const auto& e : snap.entities)
        if (e.kind == EntityKind::ResourceNode) ++nodes;
    ER_CHECK_EQ(nodes, 4); // clamp(countMin=4, 1, 8)
}
