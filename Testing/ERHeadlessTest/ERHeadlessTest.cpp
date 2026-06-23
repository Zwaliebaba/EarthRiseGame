// ERHeadlessTest.cpp — Windows MSTest home (§16.1) for the ERHeadless bot harness +
// record/replay determinism substrate (§10.3, §16.1/§16.2).
//
// ERHeadless drives many NeuronClient sessions with scripted/bot controllers (no
// rendering) for integration, determinism and load runs. Its bot loop + IOCP/socket
// host live inline in ERHeadless.cpp alongside main(), so the *building blocks* it
// composes are what is unit-testable here without a live host:
//   * ScriptedController — the deterministic (tick → FleetCommand) input log a bot replays.
//   * FleetCommand wire   — the encode/decode the bots use to talk to the server (§8.5
//     protocol-version gate included).
//   * Record/replay determinism — the core harness property: the same input log on a
//     fresh ServerUniverse reproduces the run bit-for-bit (ServerUniverse::SimHash). This
//     is the MSTest mirror of NeuronTools/testrunner/DeterminismTests.cpp (§16.2).
//
// NOT covered here (by design): the hundreds-session *contested-sector load* run (M4
// area J) — that is a live ERHeadless drive on the build agent under real overload, an
// integration/perf gate (§16.3), not a unit test.

#include "pch.h"
#include "CppUnitTest.h"

#include "ServerUniverse.h"     // Neuron::Sim::ServerUniverse, SimHash, SpawnBase/SpawnFleetShip
#include "ScriptedController.h" // Neuron::Client::ScriptedController
#include "Command.h"            // Neuron::Sim::FleetCommand, Encode/DecodeFleetCommand

#include <cstdint>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron::Sim;
using Neuron::Client::ScriptedController;
using Neuron::Universe::UniversePos;

// Single ECS component-id definition site for THIS test binary — exactly one per linked
// executable (as SimComponents.cpp does for ERHeadless/ERServer/the UWP client, and
// ShapeCatalogTests.cpp does for the Linux runner). Without it the sim's component slots
// are unbound and ServerUniverse spawns would assert.
NEURON_DEFINE_COMPONENT(Neuron::Sim::Transform,       Neuron::Sim::Slot_Transform);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Velocity,        Neuron::Sim::Slot_Velocity);
NEURON_DEFINE_COMPONENT(Neuron::Sim::BaseTag,         Neuron::Sim::Slot_BaseTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::ShipTag,         Neuron::Sim::Slot_ShipTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::NetId,           Neuron::Sim::Slot_NetId);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Health,          Neuron::Sim::Slot_Health);
NEURON_DEFINE_COMPONENT(Neuron::Sim::ShapeId,         Neuron::Sim::Slot_ShapeId);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Fuel,            Neuron::Sim::Slot_Fuel);
NEURON_DEFINE_COMPONENT(Neuron::Sim::NavState,        Neuron::Sim::Slot_NavState);
NEURON_DEFINE_COMPONENT(Neuron::Sim::BeaconTag,       Neuron::Sim::Slot_BeaconTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::OwnerId,         Neuron::Sim::Slot_OwnerId);
NEURON_DEFINE_COMPONENT(Neuron::Sim::ResourceNodeTag, Neuron::Sim::Slot_ResourceNodeTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Cargo,           Neuron::Sim::Slot_Cargo);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Storage,         Neuron::Sim::Slot_Storage);
NEURON_DEFINE_COMPONENT(Neuron::Sim::BuildQueue,      Neuron::Sim::Slot_BuildQueue);
NEURON_DEFINE_COMPONENT(Neuron::Sim::FleetMember,     Neuron::Sim::Slot_FleetMember);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Sensor,          Neuron::Sim::Slot_Sensor);
NEURON_DEFINE_COMPONENT(Neuron::Sim::HarvestOrder,    Neuron::Sim::Slot_HarvestOrder);
NEURON_DEFINE_COMPONENT(Neuron::Sim::FleetOrder,      Neuron::Sim::Slot_FleetOrder);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Weapon,          Neuron::Sim::Slot_Weapon);
NEURON_DEFINE_COMPONENT(Neuron::Sim::NpcAi,           Neuron::Sim::Slot_NpcAi);

namespace ERHeadlessTest
{

// Build a fresh world (one base + one owned ship), replay a one-command bot script, and
// return the final SimHash. Spawn order is deterministic so the ship's net id is stable
// across calls — two calls with the same target reproduce the run bit-for-bit; a
// different target diverges the world (and therefore the hash).
static uint64_t RunBotScript(int64_t moveTargetX, uint32_t ticks)
{
    ServerUniverse su(false); // no demo seed — a clean, self-contained world
    const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
    const uint32_t ship = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 500, 0, 0 });

    ScriptedController ctrl;
    FleetCommand mv;
    mv.intent      = IntentType::Move;
    mv.units       = { ship };
    mv.targetPoint = { moveTargetX, 0, 0 };
    ctrl.Add(/*player=*/base, /*tick=*/1, mv);

    for (uint32_t t = 0; t < ticks; ++t)
    {
        for (const auto* step : ctrl.StepsForTick(t))
            su.ApplyFleetCommand(step->player, step->cmd);
        su.Step(0.1f);
    }
    return su.SimHash();
}

// --- ScriptedController: the bot input log ------------------------------------------
TEST_CLASS(ScriptedControllerTests)
{
public:
    TEST_METHOD(LogPreservesOrderAndDueTick)
    {
        ScriptedController c;
        FleetCommand move; move.intent = IntentType::Move; move.targetPoint = { 1000, 0, 0 };
        FleetCommand stop; stop.intent = IntentType::Stop;
        c.Add(7, 5, move);
        c.Add(7, 5, stop);   // same tick → insertion order must be preserved
        c.Add(7, 20, move);

        Assert::AreEqual<size_t>(3, c.Log().size());
        Assert::AreEqual<uint32_t>(20u, c.LastTick());

        auto due = c.StepsForTick(5);
        Assert::AreEqual<size_t>(2, due.size());
        Assert::IsTrue(due[0]->cmd.intent == IntentType::Move);
        Assert::IsTrue(due[1]->cmd.intent == IntentType::Stop); // insertion order
        Assert::AreEqual<size_t>(0, c.StepsForTick(6).size());  // nothing due off-tick
    }
};

// --- FleetCommand wire: how bots talk to the server ---------------------------------
TEST_CLASS(FleetCommandWireTests)
{
public:
    TEST_METHOD(EncodeDecodeRoundTrip)
    {
        FleetCommand c;
        c.clientTick  = 42;
        c.intent      = IntentType::Move;
        c.queue       = true;
        c.units       = { 11, 22, 33 };
        c.targetPoint = { -1234, 5678, 9 };
        c.range       = 250.0f;

        const auto bytes = EncodeFleetCommand(c);
        FleetCommand out;
        Assert::IsTrue(DecodeFleetCommand(bytes, out));
        Assert::IsTrue(out.intent == IntentType::Move);
        Assert::IsTrue(out.queue);
        Assert::AreEqual<size_t>(3, out.units.size());
        Assert::AreEqual<uint32_t>(33u, out.units[2]);
        Assert::AreEqual<int64_t>(5678, out.targetPoint.y);
        Assert::AreEqual<uint32_t>(42u, out.clientTick);
    }

    TEST_METHOD(WrongVersionByteRejected)
    {
        FleetCommand c; c.intent = IntentType::Stop;
        auto bytes = EncodeFleetCommand(c);
        bytes[0] = 0xFE; // corrupt the protocol-version byte (§8.5 gate)
        FleetCommand out;
        Assert::IsFalse(DecodeFleetCommand(bytes, out));
    }
};

// --- Record/replay determinism: the core ERHeadless harness property ----------------
TEST_CLASS(DeterminismSmokeTests)
{
public:
    TEST_METHOD(SameLogReproducesSimHash)
    {
        const uint64_t a = RunBotScript(/*moveTargetX=*/8000, /*ticks=*/30);
        const uint64_t b = RunBotScript(/*moveTargetX=*/8000, /*ticks=*/30);
        Assert::AreEqual(a, b); // identical input log → identical sim, bit-for-bit
    }

    TEST_METHOD(DivergentLogChangesSimHash)
    {
        const uint64_t a = RunBotScript(/*moveTargetX=*/ 8000, /*ticks=*/30);
        const uint64_t b = RunBotScript(/*moveTargetX=*/-8000, /*ticks=*/30);
        Assert::AreNotEqual(a, b); // different bot command → different state (hash is sensitive)
    }
};

} // namespace ERHeadlessTest
