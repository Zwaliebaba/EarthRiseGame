// Fleet-command / fog / PvE tests (masterplan §8.4/§23.4, §13.0, §13.7; M3 areas
// B, E, F) — the RTS intent layer (encode/decode + server-validated application),
// sensor/fog visibility, and the basic NPC site a fleet clears. Pure +
// ServerUniverse integration, platform-independent, so the Linux runner covers
// them. Mirrors the NeuronCoreTest / ERServerTest cases the plan lists.
//
// Component ids are bound once per binary in ShapeCatalogTests.cpp.

#include "../datacook/UniverseSource.h" // ParseUniverseSource (+ UniverseData.h)
#include "Command.h"
#include "Fleet.h"
#include "ServerUniverse.h"
#include "TestRunner.h"

#include <string>

using namespace Neuron::Sim;
using namespace ertest;
using Neuron::Universe::UniversePos;

namespace
{
    // Big sensors + fast economy so detection/build aren't in the way of the
    // command/combat behaviour under test.
    const char* SRC =
        "region R { security = high bounds = -64 64 -64 64 -64 64 yield_mult = 1 }\n"
        "economy { fleet_cap = 8  cargo_capacity = 500  storage_capacity = 2000  harvest_rate = 100\n"
        "          sensor_range_ship = 8000  sensor_range_base = 20000  build_ore = 100  build_ice = 0\n"
        "          build_seconds = 1  build_ship_type = 1  harvester_speed = 4000  harvest_range = 600 }\n";

    void Load(ServerUniverse& su)
    {
        UniverseDataset ds;
        std::vector<std::string> errs;
        const bool ok = Neuron::Tools::ParseUniverseSource(SRC, ds, errs);
        ER_CHECK(ok && errs.empty());
        su.LoadUniverse(ds);
    }
}

// --- area B: command encode/decode -----------------------------------------

ER_TEST(Fleet, CommandRoundTrips)
{
    FleetCommand c;
    c.clientTick  = 42;
    c.intent      = IntentType::Attack;
    c.queue       = true;
    c.units       = { 7, 9, 11 };
    c.targetNetId = 1234;
    c.targetPoint = { 100, -200, 300 };
    c.range       = 750.0f;
    c.beacon      = "RIM";

    const auto bytes = EncodeFleetCommand(c);
    FleetCommand d;
    ER_CHECK(DecodeFleetCommand(bytes, d));
    ER_CHECK(d.clientTick == 42);
    ER_CHECK(d.intent == IntentType::Attack);
    ER_CHECK(d.queue == true);
    ER_CHECK_EQ(d.units.size(), size_t(3));
    ER_CHECK(d.units[0] == 7 && d.units[1] == 9 && d.units[2] == 11);
    ER_CHECK(d.targetNetId == 1234);
    ER_CHECK(d.targetPoint == UniversePos({ 100, -200, 300 }));
    ER_CHECK(d.range == 750.0f);
    ER_CHECK(d.beacon == "RIM");
}

ER_TEST(Fleet, DecodeRejectsWrongVersion)
{
    auto bytes = EncodeFleetCommand({});
    bytes[0] = 0xEE; // corrupt the version byte → protocol gate rejects (§8.5)
    FleetCommand d;
    ER_CHECK(!DecodeFleetCommand(bytes, d));
}

// --- area B: server-validated application -----------------------------------

ER_TEST(Fleet, MoveIntentSteersOwnedShipToPoint)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t ship = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 0, 0, 0 });

    FleetCommand cmd;
    cmd.intent = IntentType::Move;
    cmd.units  = { ship };
    cmd.targetPoint = { 6000, 0, 0 };
    ER_CHECK(su.ApplyFleetCommand(base, cmd) == 1);
    ER_CHECK(su.FleetOrderOf(ship)->current.type == OrderType::Move);

    for (int i = 0; i < 200 && su.FleetOrderOf(ship)->current.type != OrderType::Idle; ++i)
        su.Step(0.1f);

    UniversePos p; ER_CHECK(su.GetBasePos(ship, p));
    ER_CHECK(p == UniversePos({ 6000, 0, 0 }));                 // arrived
    ER_CHECK(su.FleetOrderOf(ship)->current.type == OrderType::Idle); // order completed
}

ER_TEST(Fleet, CommandRejectedForUnownedUnit)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t baseA = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t baseB = su.SpawnBase({ 0, 5000, 0 }, { 0, 0, 0 });
    const uint32_t shipB = su.SpawnFleetShip(baseB, ServerUniverse::ShipShapeId(), { 0, 5000, 0 });

    FleetCommand cmd;
    cmd.intent = IntentType::Move;
    cmd.units  = { shipB };
    cmd.targetPoint = { 9999, 0, 0 };
    // Player A cannot command player B's ship (§8.4 ownership check).
    ER_CHECK(su.ApplyFleetCommand(baseA, cmd) == 0);
    ER_CHECK(su.FleetOrderOf(shipB)->current.type == OrderType::Idle);
}

ER_TEST(Fleet, AttackIntentRejectsDeadTarget)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t ship = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 0, 0, 0 });
    FleetCommand cmd;
    cmd.intent = IntentType::Attack;
    cmd.units  = { ship };
    cmd.targetNetId = 999999; // no such entity
    ER_CHECK(su.ApplyFleetCommand(base, cmd) == 0);
}

ER_TEST(Fleet, OrdersQueuePreserveOrder)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t ship = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 0, 0, 0 });

    auto move = [&](int64_t x, bool q) {
        FleetCommand c; c.intent = IntentType::Move; c.units = { ship };
        c.targetPoint = { x, 0, 0 }; c.queue = q;
        ER_CHECK(su.ApplyFleetCommand(base, c) == 1);
    };
    move(2000, false); // current
    move(4000, true);  // queued #1
    move(6000, true);  // queued #2

    FleetOrder* fo = su.FleetOrderOf(ship);
    ER_CHECK(fo->current.targetPoint.x == 2000);
    ER_CHECK_EQ(fo->queue.size(), size_t(2));
    ER_CHECK(fo->queue[0].targetPoint.x == 4000);
    ER_CHECK(fo->queue[1].targetPoint.x == 6000);

    // Drains front-to-back as each leg completes.
    for (int i = 0; i < 400 && !(fo->current.type == OrderType::Idle && fo->queue.empty()); ++i)
        su.Step(0.1f);
    UniversePos p; ER_CHECK(su.GetBasePos(ship, p));
    ER_CHECK(p == UniversePos({ 6000, 0, 0 })); // ended on the last queued leg
}

ER_TEST(Fleet, StopClearsOrders)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t ship = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 0, 0, 0 });
    FleetCommand mv; mv.intent = IntentType::Move; mv.units = { ship }; mv.targetPoint = { 9000, 0, 0 };
    su.ApplyFleetCommand(base, mv);
    su.Step(0.1f);
    FleetCommand stop; stop.intent = IntentType::Stop; stop.units = { ship };
    ER_CHECK(su.ApplyFleetCommand(base, stop) == 1);
    ER_CHECK(su.FleetOrderOf(ship)->current.type == OrderType::Idle);
}

ER_TEST(Fleet, BuildIntentEnqueuesAtBase)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    su.StorageOf(base)->amount[static_cast<int>(ResourceType::Ore)] = 1000.0f;

    FleetCommand c; c.intent = IntentType::Build; c.units = { base }; // base ≈ player id
    ER_CHECK(su.ApplyFleetCommand(base, c) == 1);
    ER_CHECK(su.BuildQueueOf(base)->active);

    for (int i = 0; i < 40 && su.OwnedShipCount(base) < 1; ++i) su.Step(0.1f);
    ER_CHECK(su.OwnedShipCount(base) == 1); // a ship was built from the intent
}

// --- area E: sensor range & fog of war --------------------------------------

ER_TEST(Fleet, DetectedSetMembershipByRange)
{
    ServerUniverse su(false);
    Load(su); // base sensor range 20000
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t near = su.SpawnResourceNode(ResourceType::Ore, 100.0f, { 10000, 0, 0 });
    const uint32_t far  = su.SpawnResourceNode(ResourceType::Ore, 100.0f, { 50000, 0, 0 });

    const auto seen = su.DetectedSet(base);
    ER_CHECK(seen.count(base) == 1);  // own entity always visible
    ER_CHECK(seen.count(near) == 1);  // within sensor range
    ER_CHECK(seen.count(far)  == 0);  // beyond sensor range → fogged
}

ER_TEST(Fleet, ScanRevealsAfterDwell)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t far  = su.SpawnResourceNode(ResourceType::Ore, 100.0f, { 50000, 0, 0 });
    ER_CHECK(!su.IsRevealedTo(base, far));

    // Scan dwell is 3s; 2s isn't enough, the 4th second crosses the threshold.
    ER_CHECK(!su.OrderScan(base, far, 2.0f));
    ER_CHECK(!su.IsRevealedTo(base, far));
    ER_CHECK(su.OrderScan(base, far, 2.0f));
    ER_CHECK(su.IsRevealedTo(base, far));
    ER_CHECK(su.DetectedSet(base).count(far) == 1); // now visible despite range
}

ER_TEST(Fleet, SnapshotForExcludesUndetectedEntities)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t far  = su.SpawnResourceNode(ResourceType::Ore, 100.0f, { 80000, 0, 0 });

    const Snapshot full = su.BuildSnapshot();
    const Snapshot mine = su.BuildSnapshotFor(base);
    ER_CHECK(full.entities.size() > mine.entities.size()); // fog hides the far node

    bool sawFar = false, sawBase = false;
    for (const auto& e : mine.entities) {
        if (e.netId == far)  sawFar = true;
        if (e.netId == base) sawBase = true;
    }
    ER_CHECK(!sawFar);  // undetected → excluded
    ER_CHECK(sawBase);  // own base → included
}

// --- area F: NPC AI + basic site --------------------------------------------

ER_TEST(Fleet, AiStateTransitions)
{
    NpcAi ai; ai.aggroRange = 5000.0f; ai.fleeHpFrac = 0.2f;
    // No target → defend home.
    ER_CHECK(NextAiState(ai, false, false, 1.0f) == AiState::Defend);
    // Target in range, healthy → aggro.
    ER_CHECK(NextAiState(ai, true, true, 1.0f) == AiState::Aggro);
    // Target known but out of aggro ring → defend.
    ER_CHECK(NextAiState(ai, true, false, 1.0f) == AiState::Defend);
    // Low HP → flee regardless of target.
    ER_CHECK(NextAiState(ai, true, true, 0.1f) == AiState::Flee);
}

ER_TEST(Fleet, NpcSiteSpawnsGuardians)
{
    ServerUniverse su(false);
    Load(su);
    const uint16_t site = su.SpawnNpcSite({ 30000, 0, 0 }, 3);
    ER_CHECK(su.NpcSiteAlive(site) == 3);
    ER_CHECK(!su.IsNpcSiteCleared(site));
    ER_CHECK_EQ(su.NpcSiteMembers(site).size(), size_t(3));

    int npcs = 0;
    for (const auto& e : su.BuildSnapshot().entities)
        if (e.kind == EntityKind::NpcUnit) ++npcs;
    ER_CHECK_EQ(npcs, 3);
}

ER_TEST(Fleet, FleetClearsNpcSiteAndFiresOnce)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    // A 2-guardian site nearby; a 3-ship fleet spawned right on top of it.
    const UniversePos siteCenter{ 4000, 0, 0 };
    const uint16_t site = su.SpawnNpcSite(siteCenter, 2, 400.0f);
    std::vector<uint32_t> ships;
    for (int i = 0; i < 3; ++i)
        ships.push_back(su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 4000, 0, 0 }));

    // Command the whole fleet to attack the site's guardians (focus-fire the first,
    // re-target as guardians die).
    for (int i = 0; i < 600 && !su.IsNpcSiteCleared(site); ++i) {
        auto members = su.NpcSiteMembers(site);
        if (!members.empty()) {
            FleetCommand atk; atk.intent = IntentType::Attack; atk.units = ships;
            atk.targetNetId = members.front();
            su.ApplyFleetCommand(base, atk);
        }
        su.Step(0.1f);
    }

    ER_CHECK(su.IsNpcSiteCleared(site));
    auto cleared = su.DrainClearedSites();
    ER_CHECK_EQ(cleared.size(), size_t(1));
    ER_CHECK(cleared.front() == site);
    ER_CHECK(su.DrainClearedSites().empty()); // fires once
}

ER_TEST(Fleet, LowDpsStillDamagesAtSimTickRate)
{
    // Regression: WeaponDamage truncated dps*dt to int each tick, so any dps below
    // the tick rate (e.g. NPC dps 20 at 30 Hz → 0.667 → 0) never dealt damage on a
    // live server. The fractional accumulator must make it land over time.
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t ship = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 0, 0, 0 });
    su.WeaponOf(ship)->dps = 20.0f;                 // below 30 Hz → < 1 dmg per tick
    const uint16_t site = su.SpawnNpcSite({ 200, 0, 0 }, 1, 50.0f); // adjacent, in range
    const uint32_t npc  = su.NpcSiteMembers(site).front();

    FleetCommand atk; atk.intent = IntentType::Attack; atk.units = { ship }; atk.targetNetId = npc;
    ER_CHECK(su.ApplyFleetCommand(base, atk) == 1);
    const int32_t hp0 = su.HealthOf(npc)->hp;
    for (int i = 0; i < 90; ++i) su.Step(1.0f / 30.0f); // 3 s at the real tick rate
    ER_CHECK(su.HealthOf(npc)->hp < hp0);           // damage accumulated despite < 1/tick
}

ER_TEST(Fleet, AttackerClosesToItsOwnWeaponRange)
{
    // Regression: the stand-off used the *target's* weapon range, not the attacker's.
    // Give the target a much longer range than the attacker and start far away: the
    // attacker must still close to within ITS OWN range and deal damage.
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t ship = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 0, 0, 0 });
    const float shipRange = su.WeaponOf(ship)->range;
    const uint16_t site = su.SpawnNpcSite({ 40000, 0, 0 }, 1, 50.0f); // far away
    const uint32_t npc  = su.NpcSiteMembers(site).front();
    su.WeaponOf(npc)->range = shipRange * 4.0f;      // target out-ranges the attacker
    su.NpcAiOf(npc)->aggroRange = 0.0f;              // keep the NPC parked (defend)

    FleetCommand atk; atk.intent = IntentType::Attack; atk.units = { ship }; atk.targetNetId = npc;
    ER_CHECK(su.ApplyFleetCommand(base, atk) == 1);
    const int32_t hp0 = su.HealthOf(npc)->hp;
    for (int i = 0; i < 200 && su.HealthOf(npc) && su.HealthOf(npc)->hp == hp0; ++i) su.Step(0.1f);

    UniversePos sp, np;
    ER_CHECK(su.GetBasePos(ship, sp) && su.GetBasePos(npc, np));
    ER_CHECK(UniverseDistance(sp, np) <= static_cast<double>(shipRange)); // closed to own range
    ER_CHECK(su.HealthOf(npc)->hp < hp0);            // and dealt damage (not stuck)
}

ER_TEST(Fleet, NpcAggrosOnNearbyPlayerUnit)
{
    ServerUniverse su(false);
    Load(su);
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint16_t site = su.SpawnNpcSite({ 3000, 0, 0 }, 1, 100.0f);
    const uint32_t npc  = su.NpcSiteMembers(site).front();

    // A player ship within the NPC aggro ring → the NPC switches to Attack.
    su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 3500, 0, 0 });
    su.Step(0.1f);
    ER_CHECK(su.NpcAiOf(npc)->state == AiState::Aggro);
    ER_CHECK(su.FleetOrderOf(npc)->current.type == OrderType::Attack);
}
