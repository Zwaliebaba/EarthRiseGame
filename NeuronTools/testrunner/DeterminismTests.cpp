// Record/replay determinism + full-loop integration tests (masterplan §16.1/§16.2;
// M3 area H). The ERHeadless bots drive the whole M3 loop via a ScriptedController
// command log; replaying the same log on a fresh sim must reproduce the run
// bit-for-bit (ServerUniverse::SimHash). This is the end-to-end gate the plan's
// "Done" section calls for, exercised server-authoritatively on the Linux runner
// (the ERHeadless.cpp Winsock harness drives the same logs against a live server).
//
// Component ids are bound once per binary in ShapeCatalogTests.cpp.

#include "../datacook/UniverseSource.h"
#include "ScriptedController.h" // NeuronClient
#include "ServerUniverse.h"
#include "TestRunner.h"

#include <string>

using namespace Neuron::Sim;
using namespace ertest;
using Neuron::Client::ScriptedController;
using Neuron::Universe::UniversePos;

namespace
{
    constexpr int ORE = static_cast<int>(ResourceType::Ore);

    // A connected graph + fast economy so a scripted bot can run the full loop in a
    // bounded number of ticks.
    const char* SRC =
        "region R { security = high bounds = -64 64 -64 64 -64 64 yield_mult = 1 }\n"
        "beacon HUB { region = R pos = 0 0 0       links = RIM kind = public }\n"
        "beacon RIM { region = R pos = 200000 0 0  links = HUB kind = public }\n"
        "tuning { warp_align = 0 warp_speed_ship = 50000 jump_fuel_ship = 10 jump_spool_ship = 0.5\n"
        "         jump_cooldown = 0.5 beacon_range = 3000 ship_fuel_max = 100 base_fuel_max = 300 }\n"
        "economy { fleet_cap = 8 cargo_capacity = 200 storage_capacity = 10000 harvest_rate = 1000\n"
        "          build_ore = 300 build_ice = 0 build_seconds = 0.1 build_ship_type = 1\n"
        "          harvester_speed = 100000 harvest_range = 600 sensor_range_ship = 8000\n"
        "          sensor_range_base = 50000 }\n";

    UniverseDataset Data()
    {
        UniverseDataset ds;
        std::vector<std::string> errs;
        const bool ok = Neuron::Tools::ParseUniverseSource(SRC, ds, errs);
        ER_CHECK(ok && errs.empty());
        return ds;
    }

    // Run the scripted bot loop for 'ticks' steps; return the final SimHash. Built
    // freshly each call so two calls are independent runs. 'withCommands' false runs
    // the same world with an empty command log (a divergent input). Net ids are
    // resolved from the deterministic spawn order, so both runs script identically.
    uint64_t RunScript(const UniverseDataset& data, uint32_t ticks, bool withCommands)
    {
        ServerUniverse su(false);
        su.LoadUniverse(data);
        const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 }); // on HUB
        const uint32_t harv = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 500, 0, 0 });
        const uint32_t node = su.SpawnResourceNode(ResourceType::Ore, 5000.0f, { 4000, 0, 0 });

        ScriptedController ctrl;
        if (withCommands) {
            FleetCommand harvest; harvest.intent = IntentType::Harvest; harvest.units = { harv }; harvest.targetNetId = node;
            ctrl.Add(base, 1, harvest);
            FleetCommand jump; jump.intent = IntentType::Jump; jump.units = { harv }; jump.beacon = "RIM";
            ctrl.Add(base, 60, jump);
        }
        for (uint32_t t = 0; t < ticks; ++t) {
            for (const auto* step : ctrl.StepsForTick(t))
                su.ApplyFleetCommand(step->player, step->cmd);
            su.Step(0.1f);
        }
        return su.SimHash();
    }
}

ER_TEST(Determinism, SameLogReproducesSimHash)
{
    const auto data = Data();
    const uint64_t hashA = RunScript(data, 120, true);
    const uint64_t hashB = RunScript(data, 120, true);
    ER_CHECK(hashA == hashB); // identical input log → identical sim
}

ER_TEST(Determinism, DivergentLogChangesSimHash)
{
    const auto data = Data();
    const uint64_t hashA = RunScript(data, 120, true);
    const uint64_t hashB = RunScript(data, 120, false);
    ER_CHECK(hashA != hashB); // different input → different state (hash is sensitive)
}

ER_TEST(Determinism, HashStableAcrossStorageChurn)
{
    // Spawning + destroying entities churns ECS storage slots; the netId-sorted
    // hash must not depend on that internal layout.
    const auto data = Data();
    ServerUniverse su(false);
    su.LoadUniverse(data);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 500, 0, 0 });
    const uint16_t site = su.SpawnNpcSite({ 4000, 0, 0 }, 2, 200.0f);
    for (int i = 0; i < 5; ++i) su.Step(0.1f);
    const uint64_t before = su.SimHash();

    // Re-run the identical sequence on a fresh universe → identical hash.
    ServerUniverse su2(false);
    su2.LoadUniverse(data);
    const uint32_t base2 = su2.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    su2.SpawnFleetShip(base2, ServerUniverse::ShipShapeId(), { 500, 0, 0 });
    su2.SpawnNpcSite({ 4000, 0, 0 }, 2, 200.0f);
    for (int i = 0; i < 5; ++i) su2.Step(0.1f);
    ER_CHECK(su2.SimHash() == before);
    (void)site;
}

ER_TEST(Determinism, ScriptedFullLoopHarvestBuildJump)
{
    // The whole eXploit + navigation loop driven by the bot command log: the ship
    // harvests → deposits → the base builds a ship → the harvester jumps a beacon.
    const auto data = Data();
    ServerUniverse su(false);
    su.LoadUniverse(data);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t harv = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 500, 0, 0 });
    const uint32_t node = su.SpawnResourceNode(ResourceType::Ore, 5000.0f, { 4000, 0, 0 });

    ScriptedController ctrl;
    FleetCommand harvest; harvest.intent = IntentType::Harvest; harvest.units = { harv }; harvest.targetNetId = node;
    ctrl.Add(base, 1, harvest);

    // Drive the harvest loop until storage is stocked, then enqueue a build.
    bool enqueued = false;
    for (uint32_t t = 0; t < 200; ++t) {
        for (const auto* s : ctrl.StepsForTick(t)) su.ApplyFleetCommand(s->player, s->cmd);
        if (!enqueued && su.StorageOf(base)->amount[ORE] >= 300.0f) {
            ER_CHECK(su.EnqueueBuild(base));
            enqueued = true;
        }
        su.Step(0.1f);
        if (enqueued && su.OwnedShipCount(base) >= 2) break;
    }
    ER_CHECK(su.OwnedShipCount(base) == 2); // harvester + the freshly built ship

    // Park the harvester on the HUB beacon (cancels the harvest auto-pilot), then
    // jump it across the beacon network HUB→RIM.
    FleetCommand moveHub; moveHub.intent = IntentType::Move; moveHub.units = { harv }; moveHub.targetPoint = { 0, 0, 0 };
    ER_CHECK(su.ApplyFleetCommand(base, moveHub) == 1);
    for (int i = 0; i < 200 && su.FleetOrderOf(harv)->current.type != OrderType::Idle; ++i) su.Step(0.1f);

    FleetCommand jump; jump.intent = IntentType::Jump; jump.units = { harv }; jump.beacon = "RIM";
    ER_CHECK(su.ApplyFleetCommand(base, jump) == 1); // on HUB, RIM linked, fuel ok
    for (int i = 0; i < 40 && su.NavOf(harv)->phase != NavPhase::Idle; ++i) su.Step(0.1f);
    UniversePos p; ER_CHECK(su.GetBasePos(harv, p));
    ER_CHECK(p == UniversePos({ 200000, 0, 0 })); // arrived at RIM
}
