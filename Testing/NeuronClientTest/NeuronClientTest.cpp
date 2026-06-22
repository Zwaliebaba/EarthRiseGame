#include "pch.h"
#include "CppUnitTest.h"

#include <cstdint>
#include <string>
#include <vector>

#include "FleetControl.h" // NeuronClient — smart action / control groups / overview
#include "Starmap.h"      // NeuronClient — beacon route solver
#include "UniverseData.h" // NeuronCore — UniverseDataset / BeaconDef

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron::Client;
using Neuron::Sim::EntityKind;
using Neuron::Sim::IntentType;

// NeuronClientTest — the platform-independent client fleet-command logic (M3 area
// G), mirrored from NeuronTools/testrunner/ClientFleetTests.cpp. Pure logic over
// the replica + cooked universe graph, so it needs no ECS binding and no link
// against NeuronCore (header-only) — the same reason the Linux runner covers it.

namespace
{
  // Build the test beacon graph programmatically (no text-parser dependency):
  // HUB ↔ A ↔ B ↔ C plus a HUB ↔ C shortcut, and an isolated ISL.
  Neuron::Sim::UniverseDataset MakeGraph()
  {
    using Neuron::Sim::BeaconDef;
    Neuron::Sim::UniverseDataset ds;
    auto b = [](const char* n, std::vector<std::string> links) {
      BeaconDef d; d.name = n; d.region = "R"; d.links = std::move(links); return d;
    };
    ds.beacons.push_back(b("HUB", { "A", "C" }));
    ds.beacons.push_back(b("A",   { "HUB", "B" }));
    ds.beacons.push_back(b("B",   { "A", "C" }));
    ds.beacons.push_back(b("C",   { "B", "HUB" }));
    ds.beacons.push_back(b("ISL", {}));
    return ds;
  }
}

namespace NeuronClientTest
{
  TEST_CLASS(SmartActionTests)
  {
  public:
    TEST_METHOD(ResolvesByTargetType)
    {
      Assert::IsTrue(ResolveSmartAction(ClassifyTarget(EntityKind::ResourceNode, 0, 100)) == IntentType::Harvest);
      Assert::IsTrue(ResolveSmartAction(ClassifyTarget(EntityKind::Structure, 0, 100))    == IntentType::Jump);
      Assert::IsTrue(ResolveSmartAction(ClassifyTarget(EntityKind::NpcUnit, 0, 100))      == IntentType::Attack);
      Assert::IsTrue(ResolveSmartAction(ClassifyTarget(EntityKind::Ship, 999, 100))       == IntentType::Attack);
      Assert::IsTrue(ResolveSmartAction(ClassifyTarget(EntityKind::Ship, 100, 100))       == IntentType::Guard);
      Assert::IsTrue(ResolveSmartAction(ClassifyTarget(EntityKind::Decoration, 0, 100))   == IntentType::Move);
    }

    TEST_METHOD(MakeSmartCommandFillsTargetByType)
    {
      const std::vector<uint32_t> sel{ 5, 6 };
      auto atk = MakeSmartCommand(SmartTarget::Enemy, sel, 42, { 0, 0, 0 });
      Assert::IsTrue(atk.intent == IntentType::Attack && atk.targetNetId == 42 && atk.units.size() == 2);
      auto mv = MakeSmartCommand(SmartTarget::EmptySpace, sel, 0, { 10, 20, 30 });
      Assert::IsTrue(mv.intent == IntentType::Move && mv.targetNetId == 0);
      Assert::IsTrue(mv.targetPoint == Neuron::Universe::UniversePos({ 10, 20, 30 }));
    }
  };

  TEST_CLASS(ControlGroupTests)
  {
  public:
    TEST_METHOD(SetRecallForget)
    {
      ControlGroups cg;
      cg.Set(1, { 7, 7, 3, 9 });
      const auto& g = cg.Recall(1);
      Assert::IsTrue(g.size() == 3 && g[0] == 3 && g[1] == 7 && g[2] == 9);
      Assert::IsTrue(cg.Recall(2).empty() && cg.Recall(99).empty());
      cg.Forget(7);
      const auto& g2 = cg.Recall(1);
      Assert::IsTrue(g2.size() == 2 && g2[0] == 3 && g2[1] == 9);
    }
  };

  TEST_CLASS(OverviewTests)
  {
  public:
    TEST_METHOD(SortsByDistanceAndClassifiesIff)
    {
      ReplicaSet rs;
      auto add = [&](uint32_t id, EntityKind k, uint32_t owner, float x) {
        ReplicaEntity& e = rs.entities[rs.count++];
        e.networkId = id; e.entityType = static_cast<uint8_t>(k); e.ownerPlayer = owner;
        e.x = x; e.y = 0; e.z = 0; e.valid = true;
      };
      const uint32_t self = 100;
      add(1, EntityKind::Ship, 999, 3000.0f);
      add(2, EntityKind::Ship, 100, 500.0f);
      add(3, EntityKind::NpcUnit, 0, 1500.0f);
      add(4, EntityKind::ResourceNode, 0, 800.0f);
      auto ov = BuildOverview(rs, self, 0, 0, 0);
      Assert::IsTrue(ov.size() == 4 && ov[0].netId == 2 && ov[3].netId == 1);
      Assert::IsTrue(ov[0].iff == SmartTarget::Ally && ov[2].iff == SmartTarget::Enemy);
      const uint64_t shipMask = 1ull << static_cast<uint8_t>(EntityKind::Ship);
      Assert::IsTrue(BuildOverview(rs, self, 0, 0, 0, shipMask).size() == 2);
    }
  };

  TEST_CLASS(StarmapTests)
  {
  public:
    TEST_METHOD(ShortestRoute)
    {
      const auto uni = MakeGraph();
      auto route = SolveBeaconRoute(uni, "HUB", "C");
      Assert::IsTrue(route.size() == 2 && route.front() == "HUB" && route.back() == "C");
      auto longer = SolveBeaconRoute(uni, "A", "C");
      Assert::IsTrue(longer.size() == 3 && longer[1] == "HUB");
      Assert::IsTrue(SolveBeaconRoute(uni, "HUB", "HUB").size() == 1);
      Assert::IsTrue(SolveBeaconRoute(uni, "HUB", "ISL").empty());
      Assert::IsTrue(SolveBeaconRoute(uni, "HUB", "nope").empty());
    }

    TEST_METHOD(ReachableSet)
    {
      const auto uni = MakeGraph();
      auto reach = ReachableBeacons(uni, "HUB");
      Assert::IsTrue(reach.size() == 4 && reach.count("C") == 1 && reach.count("ISL") == 0);
    }
  };
}
