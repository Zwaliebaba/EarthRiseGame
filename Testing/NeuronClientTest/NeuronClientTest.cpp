#include "pch.h"
#include "CppUnitTest.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "FleetControl.h" // NeuronClient — smart action / control groups / overview
#include "HudOverlay.h"   // NeuronClient — in-world overlay helpers (playable slice)
#include "Onboarding.h"   // NeuronClient — objective chain (playable slice)
#include "Picking.h"      // NeuronClient — selection hit-testing (playable slice)
#include "RtsCamera.h"    // NeuronClient — RTS camera (playable slice)
#include "Starmap.h"      // NeuronClient — beacon route solver
#include "ServerStatusClient.h" // NeuronClient — debug server-status poller (§21)
#include "LoopbackNetwork.h"    // NeuronCore — in-memory ISocket for the poll test
#include "ServerStatus.h"       // NeuronCore — status wire format
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

  // --- Playable slice (mirrors NeuronTools/testrunner: Camera/Onboarding/Picking/
  //     HudOverlay). All header-only client logic, no ECS binding. ----------------

  TEST_CLASS(CameraTests)
  {
    static float Dist(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
    {
      const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
      return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

  public:
    TEST_METHOD(EyeAboveFocusAtDistance)
    {
      RtsCamera cam;
      cam.SetFocus({ 0, 0, 0 });
      Assert::IsTrue(cam.Eye().y > 0.0f);
      Assert::IsTrue(std::fabs(Dist(cam.Eye(), cam.Focus()) - cam.Distance()) < 0.01f);
    }

    TEST_METHOD(OrbitKeepsDistanceConstant)
    {
      RtsCamera cam;
      cam.SetFocus({ 100, 0, 50 });
      const float d0 = cam.Distance();
      cam.Rotate(1.2f, 0.0f);
      Assert::IsTrue(std::fabs(Dist(cam.Eye(), cam.Focus()) - d0) < 0.01f);
    }

    TEST_METHOD(ZoomAndPitchClamp)
    {
      RtsCamera cam;
      cam.Zoom(0.0001f);
      Assert::IsTrue(std::fabs(cam.Distance() - RtsCamera::MIN_DISTANCE) < 0.01f);
      cam.Zoom(100000.0f);
      Assert::IsTrue(std::fabs(cam.Distance() - RtsCamera::MAX_DISTANCE) < 0.01f);
      cam.Rotate(0.0f, +10.0f);
      Assert::IsTrue(std::fabs(cam.Pitch() - RtsCamera::MAX_PITCH) < 0.001f);
      cam.Rotate(0.0f, -10.0f);
      Assert::IsTrue(std::fabs(cam.Pitch() - RtsCamera::MIN_PITCH) < 0.001f);
    }

    TEST_METHOD(PanMovesFocusAndDisablesFollow)
    {
      RtsCamera cam;
      cam.SetFollow(true);
      cam.SetFocus({ 0, 0, 0 });
      cam.PanWorld(250.0f, 0.0f);
      Assert::IsTrue(!cam.Follow());
      Assert::IsTrue(std::fabs(cam.Focus().x) + std::fabs(cam.Focus().z) > 1.0f);
      Assert::IsTrue(std::fabs(cam.Focus().y) < 0.001f);
    }
  };

  TEST_CLASS(OnboardingTests)
  {
    using Step = Onboarding::Step;
  public:
    TEST_METHOD(WalksChainOnObservableMilestones)
    {
      Onboarding ob;
      Assert::IsTrue(ob.Current() == Step::Welcome);
      ObservedState s;
      s.hasOwnBase = true;     ob.Observe(s); Assert::IsTrue(ob.Current() == Step::Select);
      s.selectionCount = 2;    ob.Observe(s); Assert::IsTrue(ob.Current() == Step::Engage);
      s.npcVisible = 3;        ob.Observe(s); Assert::IsTrue(ob.Current() == Step::Clear);
      s.npcVisible = 0;        ob.Observe(s); Assert::IsTrue(ob.Current() == Step::Done);
      Assert::IsTrue(ob.Complete());
    }

    TEST_METHOD(ClearNeedsAPriorSighting)
    {
      Onboarding ob;
      ObservedState s; s.hasOwnBase = true; s.selectionCount = 1;
      ob.Observe(s); ob.Observe(s);          // → Engage
      s.npcVisible = 0; ob.Observe(s);
      Assert::IsTrue(ob.Current() == Step::Engage); // no sighting → cannot clear
    }

    TEST_METHOD(ObserveWorldCounts)
    {
      ReplicaSet set;
      auto add = [&](uint32_t id, EntityKind k, uint32_t owner) {
        ReplicaEntity& e = set.entities[set.count++];
        e.networkId = id; e.entityType = static_cast<uint8_t>(k); e.ownerPlayer = owner; e.valid = true;
      };
      const uint32_t me = 7;
      add(1, EntityKind::Base, me); add(2, EntityKind::Ship, me); add(3, EntityKind::Ship, me);
      add(4, EntityKind::Ship, 9); add(5, EntityKind::NpcUnit, 0); add(6, EntityKind::NpcUnit, 0);
      const ObservedState o = ObserveWorld(set, me, 2);
      Assert::IsTrue(o.hasOwnBase);
      Assert::AreEqual(uint32_t{ 2 }, o.ownedShips);
      Assert::AreEqual(uint32_t{ 2 }, o.npcVisible);
    }
  };

  TEST_CLASS(PickingTests)
  {
  public:
    TEST_METHOD(NearestWithinRadiusWins)
    {
      const std::vector<ScreenPoint> pts = {
        { 10, 100.f, 100.f, true }, { 11, 130.f, 100.f, true }, { 12, 106.f, 100.f, true } };
      Assert::AreEqual(uint32_t{ 12 }, PickNearest(pts, 105.f, 100.f, 20.f));
      Assert::AreEqual(uint32_t{ 0 }, PickNearest(pts, 105.f, 400.f, 20.f)); // none in range
    }

    TEST_METHOD(InvisibleExcludedAndBoxAnyCorner)
    {
      const std::vector<ScreenPoint> pts = {
        { 1, 50.f, 50.f, true }, { 2, 150.f, 150.f, true },
        { 3, 300.f, 300.f, true }, { 4, 80.f, 90.f, false } };
      std::vector<uint32_t> box;
      PickBox(pts, 200.f, 200.f, 0.f, 0.f, box); // reversed corners
      Assert::AreEqual(size_t{ 2 }, box.size());
      Assert::IsTrue(box[0] == 1 && box[1] == 2);
    }

    TEST_METHOD(DragThreshold)
    {
      Assert::IsTrue(!IsDrag(100.f, 100.f, 102.f, 101.f));
      Assert::IsTrue(IsDrag(100.f, 100.f, 140.f, 130.f));
    }
  };

  TEST_CLASS(HudOverlayTests)
  {
  public:
    TEST_METHOD(NominalMaxAndBarVisibility)
    {
      Assert::AreEqual(1000, NominalMaxHp(EntityKind::Base));
      Assert::AreEqual(500, NominalMaxHp(EntityKind::Ship));
      Assert::AreEqual(300, NominalMaxHp(EntityKind::NpcUnit));
      Assert::AreEqual(0, NominalMaxHp(EntityKind::Asteroid));
      Assert::IsTrue(ShowsHealthBar(EntityKind::Ship));
      Assert::IsTrue(!ShowsHealthBar(EntityKind::Decoration));
    }

    TEST_METHOD(FractionClampsAndScales)
    {
      Assert::IsTrue(std::fabs(HealthFraction(EntityKind::Ship, 250) - 0.5f) < 1e-4f);
      Assert::IsTrue(std::fabs(HealthFraction(EntityKind::Ship, 900) - 1.0f) < 1e-4f);
      Assert::IsTrue(std::fabs(HealthFraction(EntityKind::Ship, -50) - 0.0f) < 1e-4f);
      Assert::IsTrue(HealthFraction(EntityKind::Asteroid, 100) < 0.0f);
    }
  };

  // --- Debug server-status poller (§21) ---------------------------------------
  // Mirrors NeuronTools/testrunner/ServerStatusTests.cpp: the overlay line
  // formatter and the full client poll path over an in-memory LoopbackSocket.
  TEST_CLASS(ServerStatusClientTests)
  {
    static Neuron::Net::ServerStatus Sample()
    {
      Neuron::Net::ServerStatus s;
      s.connectionsActive = 4;
      s.connectionsPending = 7;
      s.objectsSpawned = 1024;
      s.projectiles = 33;
      return s;
    }

  public:
    TEST_METHOD(FormatStatusLines)
    {
      const auto lines = ServerStatusClient::FormatStatusLines(Sample());
      Assert::AreEqual(size_t{ 12 }, lines.size()); // title + 11 value rows
      Assert::IsTrue(lines[0].find("SERVER STATUS") != std::string::npos);

      bool foundConns = false;
      for (const auto& ln : lines)
        if (ln.find("4 active / 7 total") != std::string::npos) foundConns = true;
      Assert::IsTrue(foundConns);
    }

    TEST_METHOD(PollOverLoopback)
    {
      Neuron::Net::LoopbackNetwork net;
      constexpr uint16_t STATUS_PORT = 7778;
      Neuron::Net::LoopbackSocket serverSock(&net);
      Neuron::Net::LoopbackSocket clientSock(&net);
      Assert::IsTrue(serverSock.Open(STATUS_PORT));
      Assert::IsTrue(clientSock.Open(0));

      ServerStatusClient client(&clientSock, "127.0.0.1", STATUS_PORT);
      Assert::IsTrue(client.Enabled());
      Assert::IsFalse(client.Valid());

      client.RequestStatus();

      uint8_t buf[Neuron::Net::STATUS_MAX_DATAGRAM_BYTES]{};
      Neuron::Net::Endpoint from;
      const int n = serverSock.RecvFrom(from, std::span<uint8_t>(buf, sizeof(buf)));
      Assert::IsTrue(n > 0);
      Assert::IsTrue(Neuron::Net::IsStatusQuery(
          std::span<const uint8_t>(buf, static_cast<size_t>(n))));
      const std::string json = Neuron::Net::EncodeStatusJson(Sample());
      serverSock.SendTo(from, std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(json.data()), json.size()));

      Assert::IsTrue(client.Poll());
      Assert::IsTrue(client.Valid());
      Assert::AreEqual(uint64_t{ 1024 }, client.Last().objectsSpawned);
      Assert::AreEqual(uint32_t{ 4 }, client.Last().connectionsActive);
    }
  };
}
