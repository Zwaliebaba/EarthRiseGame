#include "pch.h"
#include "CppUnitTest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "Command.h"
#include "Economy.h"
#include "Fleet.h"
#include "Interest.h"
#include "Navigation.h"
#include "ServerUniverse.h"
#include "ShapeCatalog.h"
#include "Snapshot.h"
#include "SnapshotScheduler.h"
#include "TimeDilation.h"
#include "UniverseData.h"
#include "UniverseSource.h" // local copy of the build-time text parser (see header)

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron::Sim;
using namespace Neuron::Tools;
using Neuron::Universe::UniversePos;

// NeuronCoreTest — server-authoritative, platform-independent sim logic mirrored
// from NeuronTools/testrunner (§16.2): the ShapeCatalog + ECS/snapshot wire
// format, navigation (warp/jump/interdiction), the eXploit economy, the harvest
// micro-loop, and the universe cook/check pipeline. All of it is header-only, so
// nothing here needs to link NeuronCore — the same reason the Linux runner can
// exercise it with a bare compiler.

// Bind the sim ECS component ids exactly once in this binary (as SimComponents.cpp
// does for the server, and ShapeCatalogTests.cpp does for the Linux runner).
NEURON_DEFINE_COMPONENT(Neuron::Sim::Transform, Neuron::Sim::Slot_Transform);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Velocity, Neuron::Sim::Slot_Velocity);
NEURON_DEFINE_COMPONENT(Neuron::Sim::BaseTag, Neuron::Sim::Slot_BaseTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::ShipTag, Neuron::Sim::Slot_ShipTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::NetId, Neuron::Sim::Slot_NetId);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Health, Neuron::Sim::Slot_Health);
NEURON_DEFINE_COMPONENT(Neuron::Sim::ShapeId, Neuron::Sim::Slot_ShapeId);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Fuel, Neuron::Sim::Slot_Fuel);
NEURON_DEFINE_COMPONENT(Neuron::Sim::NavState, Neuron::Sim::Slot_NavState);
NEURON_DEFINE_COMPONENT(Neuron::Sim::BeaconTag, Neuron::Sim::Slot_BeaconTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::OwnerId, Neuron::Sim::Slot_OwnerId);
NEURON_DEFINE_COMPONENT(Neuron::Sim::ResourceNodeTag, Neuron::Sim::Slot_ResourceNodeTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Cargo, Neuron::Sim::Slot_Cargo);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Storage, Neuron::Sim::Slot_Storage);
NEURON_DEFINE_COMPONENT(Neuron::Sim::BuildQueue, Neuron::Sim::Slot_BuildQueue);
NEURON_DEFINE_COMPONENT(Neuron::Sim::FleetMember, Neuron::Sim::Slot_FleetMember);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Sensor, Neuron::Sim::Slot_Sensor);
NEURON_DEFINE_COMPONENT(Neuron::Sim::HarvestOrder, Neuron::Sim::Slot_HarvestOrder);
NEURON_DEFINE_COMPONENT(Neuron::Sim::FleetOrder, Neuron::Sim::Slot_FleetOrder);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Weapon, Neuron::Sim::Slot_Weapon);
NEURON_DEFINE_COMPONENT(Neuron::Sim::NpcAi, Neuron::Sim::Slot_NpcAi);

namespace
{
  constexpr int kOre = static_cast<int>(ResourceType::Ore);
  constexpr int kIce = static_cast<int>(ResourceType::Ice);

  // Small connected graph + fast tuning for the navigation integration tests.
  const char* kNavSrc =
      "region R { security = high bounds = -64 64 -64 64 -64 64 yield_mult = 1 }\n"
      "beacon HUB { region = R pos = 0 0 0        links = RIM     kind = public }\n"
      "beacon RIM { region = R pos = 100000 0 0   links = HUB FAR kind = public }\n"
      "beacon FAR { region = R pos = 500000 0 0   links = RIM     kind = public }\n"
      "tuning { warp_align = 0  warp_speed_base = 10000  jump_fuel_base = 20\n"
      "         jump_spool_base = 1  jump_cooldown = 1  beacon_range = 2000  base_fuel_max = 100 }\n";

  // Tiny economy for fast build/cap tests.
  const char* kEconSrc =
      "region R { security = high bounds = -64 64 -64 64 -64 64 yield_mult = 1 }\n"
      "economy { fleet_cap = 2  cargo_capacity = 500  storage_capacity = 2000  harvest_rate = 100\n"
      "          sensor_range_ship = 4000  sensor_range_base = 9000  build_ore = 100  build_ice = 50\n"
      "          build_seconds = 1  build_ship_type = 1 }\n";

  // Fast economy: small cargo, instant travel, cheap ore-only ship, quick build.
  const char* kHarvestEcon =
      "region R { security = high bounds = -64 64 -64 64 -64 64 yield_mult = 1 }\n"
      "economy { fleet_cap = 8  cargo_capacity = 200  storage_capacity = 10000  harvest_rate = 1000\n"
      "          build_ore = 300  build_ice = 0  build_seconds = 0.1  build_ship_type = 1\n"
      "          harvester_speed = 100000  harvest_range = 600 }\n";

  // A minimal, fully valid universe dataset (2 regions, a connected public
  // beacon chain, one resource field) for the cook/check pipeline tests.
  const char* kGoodUniverse =
      "region HOME { security = high  bounds = -16 16 -16 16 -4 4  yield_mult = 0.6 }\n"
      "region EDGE { security = low   bounds = 17 64 -16 16 -4 4   yield_mult = 1.0 }\n"
      "beacon A { region = HOME  pos = 0 0 0        links = B          kind = public }\n"
      "beacon B { region = HOME  pos = 327680 0 0   links = A C        kind = public }\n"
      "beacon C { region = EDGE  pos = 1310720 0 0  links = B          kind = public }\n"
      "field BELT { region = HOME  center = 1000 0 0  radius = 1500\n"
      "            nodes = Ore:0.6 Ice:0.4  count = 4 10  yield = 3000 6000  respawn = 600 }\n";

  UniverseDataset LoadFrom(ServerUniverse& su, const char* src)
  {
    UniverseDataset ds;
    std::vector<std::string> errs;
    const bool ok = ParseUniverseSource(src, ds, errs);
    Assert::IsTrue(ok && errs.empty());
    su.LoadUniverse(ds);
    return ds;
  }

  UniverseDataset ParseUniverseOk(const char* src)
  {
    UniverseDataset ds;
    std::vector<std::string> errs;
    const bool ok = ParseUniverseSource(src, ds, errs);
    Assert::IsTrue(ok && errs.empty());
    return ds;
  }
} // namespace

namespace NeuronCoreTest
{
  // --- ShapeCatalog + ECS/snapshot --------------------------------------------

  TEST_CLASS(ShapeCatalogTests)
  {
  public:
    TEST_METHOD(CountAndSequentialIds)
    {
      Assert::IsTrue(kShapeCount == 70u);
      for (uint16_t i = 0; i < kShapeCount; ++i)
      {
        Assert::IsTrue(kShapes[i].id == i);
        const auto p = kShapes[i].cmoPath;
        Assert::IsTrue(p.size() > 4 && p.substr(p.size() - 4) == ".cmo");
      }
    }

    TEST_METHOD(NameLookup)
    {
      Assert::IsTrue(ShapeIdByName("Jumpgate01") != kInvalidShapeId);
      Assert::IsTrue(ShapeIdByName("Outpost01") != kInvalidShapeId);
      Assert::IsTrue(ShapeIdByName("HullAurora") != kInvalidShapeId);
      Assert::IsTrue(ShapeIdByName("DoesNotExist") == kInvalidShapeId);
      // Round-trip: id -> def -> name.
      const uint16_t id = ShapeIdByName("HullAurora");
      Assert::IsTrue(ShapeById(id) != nullptr);
      Assert::IsTrue(ShapeById(id)->name == "HullAurora");
      Assert::IsTrue(ShapeById(kInvalidShapeId) == nullptr);
    }

    TEST_METHOD(CategoryToKind)
    {
      Assert::IsTrue(KindForCategory(ShapeCategory::Hull) == EntityKind::Ship);
      Assert::IsTrue(KindForCategory(ShapeCategory::Asteroid) == EntityKind::Asteroid);
      Assert::IsTrue(KindForCategory(ShapeCategory::Station) == EntityKind::Station);
      Assert::IsTrue(KindForCategory(ShapeCategory::Jumpgate) == EntityKind::Structure);
      Assert::IsTrue(KindForCategory(ShapeCategory::Crate) == EntityKind::LootContainer);
    }

    TEST_METHOD(EveryCategoryHasAtLeastOneShape)
    {
      for (uint8_t c = 0; c <= static_cast<uint8_t>(ShapeCategory::Station); ++c)
        Assert::IsTrue(FirstShapeOfCategory(static_cast<ShapeCategory>(c)) != kInvalidShapeId);
    }
  };

  TEST_CLASS(ServerUniverseTests)
  {
  public:
    TEST_METHOD(ScenerySpawnedWithShapeAndKind)
    {
      ServerUniverse w;
      const Snapshot snap = w.BuildSnapshot();
      Assert::IsTrue(!snap.entities.empty());

      // The jumpgate landmark is present and classified as a Structure.
      bool sawGate = false;
      const uint16_t gate = ShapeIdByName("Jumpgate01");
      for (const auto& e : snap.entities)
      {
        Assert::IsTrue(e.shapeId != kInvalidShapeId);
        if (e.shapeId == gate)
        {
          sawGate = true;
          Assert::IsTrue(e.kind == EntityKind::Structure);
        }
      }
      Assert::IsTrue(sawGate);
    }

    TEST_METHOD(SpawnedBaseCarriesBaseKind)
    {
      ServerUniverse w;
      const uint32_t net = w.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      const Snapshot snap = w.BuildSnapshot();
      bool sawBase = false;
      for (const auto& e : snap.entities)
        if (e.netId == net)
        {
          sawBase = true;
          Assert::IsTrue(e.kind == EntityKind::Base);
          Assert::IsTrue(e.shapeId == ServerUniverse::BaseShapeId());
        }
      Assert::IsTrue(sawBase);
    }
  };

  TEST_CLASS(SnapshotTests)
  {
  public:
    TEST_METHOD(RoundTripPreservesShapeIdAndKind)
    {
      ServerUniverse w;
      w.SpawnBase({ 100, 0, 0 }, { 0, 0, 0 });
      const Snapshot snap = w.BuildSnapshot();

      const std::vector<uint8_t> bytes = EncodeSnapshot(snap);
      Snapshot decoded;
      Assert::IsTrue(DecodeSnapshot(bytes, decoded));
      Assert::IsTrue(decoded.entities.size() == snap.entities.size());
      for (size_t i = 0; i < decoded.entities.size(); ++i)
      {
        Assert::IsTrue(decoded.entities[i].shapeId == snap.entities[i].shapeId);
        Assert::IsTrue(decoded.entities[i].kind == snap.entities[i].kind);
        Assert::IsTrue(decoded.entities[i].netId == snap.entities[i].netId);
      }
    }
  };

  // --- Quantized sector-local delta codec (M4 area C; §8.4, App. A) -----------

  TEST_CLASS(SnapshotDeltaTests)
  {
    static SnapshotEntity Ent(uint32_t netId, UniversePos pos, DirectX::XMFLOAT3 local,
                              int32_t hp, uint16_t shape, EntityKind kind, uint32_t owner)
    {
      SnapshotEntity e;
      e.netId = netId; e.pos = pos; e.localOffset = local; e.hp = hp;
      e.shapeId = shape; e.kind = kind; e.ownerPlayer = owner;
      return e;
    }
    static double Abs(int64_t pos, float local) { return static_cast<double>(pos) + local; }
    static double Step()
    {
      return static_cast<double>(Neuron::Universe::kSectorSize) /
             static_cast<double>(uint64_t(1) << kPosQuantBitsPerAxis);
    }

  public:
    TEST_METHOD(FirstSightRoundTripWithinQuantBound)
    {
      const SnapshotEntity src = Ent(7, { 1000, 2000, 3000 }, { 0.25f, 0.5f, 0.75f },
                                     640, 5, EntityKind::Ship, 42);
      DeltaSnapshot snap; snap.tick = 11;
      snap.records.push_back(MakeDeltaRecord(src, nullptr));
      DeltaDecodeState dec;
      Assert::IsTrue(dec.Apply(EncodeDeltaSnapshot(snap)));
      const SnapshotEntity* got = dec.Find(7);
      Assert::IsTrue(got != nullptr);
      Assert::IsTrue(std::fabs(Abs(got->pos.x, got->localOffset.x) - 1000.25) <= Step());
      Assert::IsTrue(std::fabs(Abs(got->pos.z, got->localOffset.z) - 3000.75) <= Step());
      Assert::AreEqual(int32_t{ 640 }, got->hp);
      Assert::AreEqual(uint16_t{ 5 }, got->shapeId);
      Assert::AreEqual(uint32_t{ 42 }, got->ownerPlayer);
    }

    TEST_METHOD(MaskOmitsUnchangedFields)
    {
      const SnapshotEntity base = Ent(7, { 1000, 0, 0 }, { 0, 0, 0 }, 640, 5, EntityKind::Ship, 42);
      SnapshotEntity cur = base; cur.hp = 600;
      const DeltaRecord r = MakeDeltaRecord(cur, &base);
      Assert::AreEqual(uint8_t{ DeltaHp }, r.mask);
      Assert::IsTrue(DeltaRecordBits(r) < DeltaRecordBits(MakeDeltaRecord(cur, nullptr)));
    }

    TEST_METHOD(StationaryEntityCostsNothing)
    {
      const SnapshotEntity base = Ent(7, { 1234, 5678, 9012 }, { 0.1f, 0.2f, 0.3f },
                                      640, 5, EntityKind::Ship, 42);
      Assert::AreEqual(uint8_t{ 0 }, MakeDeltaRecord(base, &base).mask);
    }

    TEST_METHOD(BudgetedSnapshotNeverExceedsAndSpillsTheRest)
    {
      std::vector<DeltaRecord> recs;
      for (uint32_t i = 1; i <= 20; ++i)
        recs.push_back(MakeDeltaRecord(
            Ent(i, { 1000 * i, 0, 0 }, { 0, 0, 0 }, 100, 5, EntityKind::Ship, 1), nullptr));
      std::vector<uint32_t> overflow;
      const size_t budget = 60;
      const DeltaSnapshot snap = BuildBudgetedSnapshot(1, recs, budget, overflow);
      Assert::IsTrue(EncodeDeltaSnapshot(snap).size() <= budget);
      Assert::IsTrue(!overflow.empty());
      Assert::AreEqual(size_t{ 20 }, snap.records.size() + overflow.size());
    }

    TEST_METHOD(ReorderedAndDuplicateSnapshotsConvergeByTick)
    {
      const SnapshotEntity a = Ent(1, { 0, 0, 0 }, { 0, 0, 0 }, 100, 5, EntityKind::Ship, 7);
      DeltaDecodeState dec;
      DeltaSnapshot s5; s5.tick = 5; s5.records.push_back(MakeDeltaRecord(a, nullptr));
      dec.Apply(EncodeDeltaSnapshot(s5));
      Assert::AreEqual(int32_t{ 100 }, dec.Find(1)->hp);

      SnapshotEntity a4 = a; a4.hp = 50;
      DeltaSnapshot s4; s4.tick = 4; s4.records.push_back(MakeDeltaRecord(a4, &a));
      dec.Apply(EncodeDeltaSnapshot(s4));
      Assert::AreEqual(int32_t{ 100 }, dec.Find(1)->hp); // stale ignored

      SnapshotEntity a6 = a; a6.hp = 80;
      DeltaSnapshot s6; s6.tick = 6; s6.records.push_back(MakeDeltaRecord(a6, &a));
      dec.Apply(EncodeDeltaSnapshot(s6));
      Assert::AreEqual(int32_t{ 80 }, dec.Find(1)->hp);

      SnapshotEntity a6b = a; a6b.hp = 10;
      DeltaSnapshot s6b; s6b.tick = 6; s6b.records.push_back(MakeDeltaRecord(a6b, &a));
      dec.Apply(EncodeDeltaSnapshot(s6b));
      Assert::AreEqual(int32_t{ 80 }, dec.Find(1)->hp); // duplicate tick ignored
    }
  };

  // --- Priority / quota snapshot scheduler (M4 area E; §8.4) ------------------

  TEST_CLASS(SchedulerTests)
  {
    // Spawn a base and 'n' static scenery props in its sector; returns the base id.
    static uint32_t SeedScene(ServerUniverse& su, int n)
    {
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      for (int i = 0; i < n; ++i) su.SpawnProp(0, { 100 + 10 * i, 0, 0 });
      return base;
    }
    static size_t VisibleCount(ServerUniverse& su, uint32_t base)
    {
      std::vector<uint32_t> vis; su.Interest().VisibleTo(base, vis); return vis.size();
    }

  public:
    TEST_METHOD(PriorityRanksRelevantAndNearAboveDistantNeutral)
    {
      RelevanceWeights w;
      const SchedCandidate ownBase{ 1, 0.0, Iff::Own, true, false, 0 };
      const SchedCandidate target{ 2, 16000.0, Iff::Enemy, false, true, 0 };
      const SchedCandidate neutralFar{ 3, 320000.0, Iff::Neutral, false, false, 0 };
      Assert::IsTrue(SnapshotPriority(ownBase, w) > SnapshotPriority(target, w));
      Assert::IsTrue(SnapshotPriority(target, w) > SnapshotPriority(neutralFar, w));
    }

    TEST_METHOD(StalenessRaisesAnAgedEntity)
    {
      RelevanceWeights w;
      SchedCandidate fresh{ 1, 16000.0, Iff::Neutral, false, false, 0 };
      SchedCandidate aged = fresh; aged.netId = 2; aged.staleness = 100;
      Assert::IsTrue(SnapshotPriority(aged, w) > SnapshotPriority(fresh, w));
    }

    TEST_METHOD(ScheduleAppliesVisibleCapAndIsDeterministic)
    {
      RelevanceWeights w;
      std::vector<SchedCandidate> cands;
      for (uint32_t i = 1; i <= 10; ++i)
        cands.push_back({ i, 1000.0 * i, Iff::Neutral, false, false, 0 });
      const ScheduleResult r = ScheduleClient(cands, w, 3);
      Assert::AreEqual(size_t{ 3 }, r.ordered.size());
      Assert::AreEqual(size_t{ 7 }, r.capped);
      Assert::AreEqual(uint32_t{ 1 }, r.ordered[0]);
      const ScheduleResult r2 = ScheduleClient(cands, w, 3);
      Assert::IsTrue(r.ordered == r2.ordered);
    }

    TEST_METHOD(KnownStateAdvancesOnlyOnAck)
    {
      ClientKnownState known;
      SnapshotEntity e; e.netId = 7; e.hp = 100;
      Assert::IsTrue(known.Base(7) == nullptr);
      known.RecordSent(5, { e });
      Assert::IsTrue(known.Base(7) == nullptr); // sent, not acked
      known.Ack(5);
      Assert::IsTrue(known.Base(7) != nullptr);
      Assert::AreEqual(int32_t{ 100 }, known.Base(7)->hp);
    }

    TEST_METHOD(ColdStartConvergesUnderTinyBudget)
    {
      ServerUniverse su(false);
      const uint32_t base = SeedScene(su, 10);
      su.Step(0.1f);
      const size_t want = VisibleCount(su, base);
      Assert::IsTrue(want >= 11);

      DeltaDecodeState client;
      bool firstTick = true;
      for (int t = 0; t < 60 && client.Size() < want; ++t) {
        const DeltaSnapshot snap = su.BuildClientSnapshot(base, 64);
        if (firstTick) {
          Assert::IsTrue(!snap.records.empty());
          Assert::AreEqual(base, snap.records[0].netId); // own base is top priority
          firstTick = false;
        }
        Assert::IsTrue(EncodeDeltaSnapshot(snap).size() <= 64);
        client.Apply(EncodeDeltaSnapshot(snap));
        su.RecordClientSnapshotSent(base, snap);
        su.AckClient(base, su.Tick());
        su.Step(0.1f);
      }
      Assert::AreEqual(want, client.Size());
    }

    TEST_METHOD(VisibleCapBindsButStillConverges)
    {
      ServerUniverse su(false);
      const uint32_t base = SeedScene(su, 10);
      su.SetVisibleCap(3);
      su.Step(0.1f);
      const size_t want = VisibleCount(su, base);

      DeltaDecodeState client;
      bool sawCapBind = false;
      for (int t = 0; t < 80 && client.Size() < want; ++t) {
        size_t capped = 0;
        const DeltaSnapshot snap = su.BuildClientSnapshot(base, 4096, &capped);
        Assert::IsTrue(snap.records.size() <= 3);
        if (capped > 0) sawCapBind = true;
        client.Apply(EncodeDeltaSnapshot(snap));
        su.RecordClientSnapshotSent(base, snap);
        su.AckClient(base, su.Tick());
        su.Step(0.1f);
      }
      Assert::IsTrue(sawCapBind);
      Assert::AreEqual(want, client.Size());
    }
  };

  // --- Time dilation (M4 area H; §7.2 / §9) -----------------------------------

  TEST_CLASS(DilationTests)
  {
    static constexpr double kBudget = 1.0 / 30.0;
    static double Settle(DilationController& c, const DilationConfig& cfg, double cost, int n)
    {
      double f = c.Factor();
      for (int i = 0; i < n; ++i) f = c.Update(cost, kBudget, cfg);
      return f;
    }

  public:
    TEST_METHOD(FullSpeedWhenUnderBudget)
    {
      DilationController c; DilationConfig cfg;
      Assert::IsTrue(Settle(c, cfg, kBudget * 0.5, 50) >= 0.999);
      Assert::IsTrue(!c.IsDilated());
    }

    TEST_METHOD(DilatesTowardOnsetOverLoad)
    {
      DilationController c; DilationConfig cfg;
      const double f = Settle(c, cfg, kBudget * 3.0, 200);
      Assert::IsTrue(f > 0.30 && f < 0.37); // ~ onset/load = 1/3
      Assert::IsTrue(c.IsDilated());
    }

    TEST_METHOD(ClampsAtFloorUnderExtremeOverload)
    {
      DilationController c; DilationConfig cfg;
      const double f = Settle(c, cfg, kBudget * 50.0, 300);
      Assert::IsTrue(f >= cfg.floor - 1e-9 && f <= cfg.floor + 1e-6);
    }

    TEST_METHOD(RecoversToFullSpeedWhenLoadDrops)
    {
      DilationController c; DilationConfig cfg;
      Settle(c, cfg, kBudget * 4.0, 200);
      Assert::IsTrue(c.IsDilated());
      Assert::IsTrue(Settle(c, cfg, kBudget * 0.2, 500) >= 0.999);
      Assert::IsTrue(!c.IsDilated());
    }

    TEST_METHOD(FactorStaysWithinBounds)
    {
      DilationController c; DilationConfig cfg;
      for (int i = 0; i < 500; ++i) {
        const double cost = (i % 2 == 0) ? kBudget * 8.0 : kBudget * 0.1;
        const double f = c.Update(cost, kBudget, cfg);
        Assert::IsTrue(f >= cfg.floor - 1e-9 && f <= 1.0 + 1e-9);
      }
    }
  };

  // --- Interest (cell pub/sub, M4 area A; §8.4, §6.3) -------------------------

  TEST_CLASS(InterestGridTests)
  {
  public:
    TEST_METHOD(CrossingEmitsOneLeaveAndOneEnter)
    {
      InterestGrid g;
      const auto first = g.UpdateResidency(7, { 0, 0, 0 });
      Assert::IsTrue(first.changed && !first.hadPrevious); // enter only
      Assert::IsTrue(!g.UpdateResidency(7, { 0, 0, 0 }).changed); // same sector = no-op

      const auto ev = g.UpdateResidency(7, { 1, 0, 0 });
      Assert::IsTrue(ev.changed && ev.hadPrevious);
      Assert::IsTrue(ev.from == SectorId{ 0, 0, 0 });
      Assert::IsTrue(ev.to == SectorId{ 1, 0, 0 });
      Assert::IsTrue(g.Residents({ 0, 0, 0 }).empty());        // exactly one leave
      Assert::AreEqual(size_t{ 1 }, g.Residents({ 1, 0, 0 }).size()); // exactly one enter
    }

    TEST_METHOD(NeighbourhoodSubscriptionMatchesRange)
    {
      InterestGrid g;
      std::vector<SectorId> cells;
      CollectNeighbourhood({ 0, 0, 0 }, 2, cells);
      g.SetSubscription(1, cells);
      Assert::IsTrue(g.IsSubscribed(1, { 2, 0, 0 }));
      Assert::IsTrue(g.IsSubscribed(1, { -2, -2, -2 }));
      Assert::IsTrue(!g.IsSubscribed(1, { 3, 0, 0 }));     // past the radius
      Assert::AreEqual(size_t{ 5 * 5 * 5 }, g.Subscriptions(1).size());
    }

    TEST_METHOD(MutationEnqueuedToCellSubscribersOnly)
    {
      InterestGrid g;
      const SectorId cellC{ 5, 0, 0 };
      g.UpdateResidency(100, cellC);
      g.Subscribe(1, cellC);
      g.Subscribe(2, { 9, 0, 0 }); // subscribes elsewhere

      Assert::AreEqual(size_t{ 1 }, g.Subscribers(cellC).size());
      Assert::AreEqual(uint32_t{ 1 }, g.Subscribers(cellC)[0]);
      std::vector<uint32_t> vis;
      g.VisibleTo(1, vis);
      Assert::AreEqual(size_t{ 1 }, vis.size());
      Assert::AreEqual(uint32_t{ 100 }, vis[0]);
      g.VisibleTo(2, vis);
      Assert::IsTrue(vis.empty()); // the non-subscriber never sees the mutation
    }

    TEST_METHOD(WarpPrefetchSubscribesDestinationBeforeArrival)
    {
      ServerUniverse su(false);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      const UniversePos dest{ int64_t(40) * Neuron::Universe::kSectorSize, 0, 0 };
      const SectorId destSec = Neuron::Universe::UniverseToSector(dest);
      Assert::IsTrue(su.BeginWarpTo(base, dest));            // R21 prefetch fires
      Assert::IsTrue(su.Interest().IsSubscribed(base, destSec)); // before arrival
      su.Step(0.1f);
      Assert::IsTrue(su.Interest().IsSubscribed(base, destSec)); // pinned in flight
    }
  };

  // --- Replication versions + per-client baselines (M4 area B; §8.4/§8.3) ------

  TEST_CLASS(ReplicationTests)
  {
  public:
    TEST_METHOD(VersionBumpsOnlyWhenAReplicatedFieldChanges)
    {
      ServerUniverse su(false);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      su.Step(0.1f);
      const uint32_t v1 = su.ReplVersion(base);
      Assert::IsTrue(v1 >= 1);
      su.Step(0.1f);
      su.Step(0.1f);
      Assert::AreEqual(v1, su.ReplVersion(base)); // idle holds its version
      su.SetBaseVelocity(base, { 50, 0, 0 });
      su.Step(0.1f);
      Assert::IsTrue(su.ReplVersion(base) > v1);  // position changed → bump
    }

    TEST_METHOD(StampBumpsOnlyOnChange)
    {
      ReplicationStamps stamps;
      ReplFields f;
      f.hp = 100;
      Assert::AreEqual(uint32_t{ 1 }, stamps.Stamp(42, f));
      Assert::AreEqual(uint32_t{ 1 }, stamps.Stamp(42, f)); // unchanged
      f.hp = 90;
      Assert::AreEqual(uint32_t{ 2 }, stamps.Stamp(42, f)); // changed field
    }

    TEST_METHOD(BaselineDiffSelectsChangedAndAckShrinksToEmpty)
    {
      ClientBaseline base;
      Assert::IsTrue(base.Needs(10, 3));
      base.RecordSent(5, { { 10, 3 } });
      Assert::IsTrue(base.Needs(10, 3)); // from acked, not last-sent
      base.Ack(5);
      Assert::IsTrue(!base.Needs(10, 3)); // re-acked → ∅
      Assert::IsTrue(base.Needs(10, 4));
      Assert::AreEqual(uint32_t{ 3 }, base.Acked(10));
    }

    TEST_METHOD(DroppedSnapshotReDeltasFromAckedBaseline)
    {
      ServerUniverse su(false);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      su.Step(0.1f);
      const auto changed1 = su.ChangedFor(base);
      Assert::IsTrue(std::find(changed1.begin(), changed1.end(), base) != changed1.end());

      su.RecordSent(base, su.Tick(), changed1); // sent but ack lost
      su.Step(0.1f);
      const auto changed2 = su.ChangedFor(base);
      Assert::IsTrue(std::find(changed2.begin(), changed2.end(), base) != changed2.end());

      su.RecordSent(base, su.Tick(), changed2);
      su.AckBaseline(base, su.Tick());          // now acked
      su.Step(0.1f);
      const auto changed3 = su.ChangedFor(base);
      Assert::IsTrue(std::find(changed3.begin(), changed3.end(), base) == changed3.end());
    }
  };

  // --- Tombstone eviction (M4 area D; §8.4 / App. A) --------------------------

  TEST_CLASS(TombstoneTests)
  {
  public:
    TEST_METHOD(ReEmitsUntilAckedThenClears)
    {
      ClientBaseline base;
      base.RecordSent(1, { { 10, 1 } });
      base.Ack(1);
      Assert::AreEqual(size_t{ 1 }, base.AckedCount());

      base.Tombstone(10); // entity 10 leaves interest
      Assert::IsTrue(base.IsTombstoned(10));
      Assert::AreEqual(size_t{ 0 }, base.AckedCount());

      std::vector<uint32_t> tombs;
      base.CollectTombstones(tombs);
      Assert::AreEqual(size_t{ 1 }, tombs.size());
      Assert::AreEqual(uint32_t{ 10 }, tombs[0]);
      base.RecordTombstonesSent(5, tombs); // snapshot 5 SENT
      Assert::IsTrue(base.IsTombstoned(10)); // but not acked → still pending

      tombs.clear();
      base.CollectTombstones(tombs);
      base.RecordTombstonesSent(6, tombs);
      base.Ack(6);
      Assert::IsTrue(!base.IsTombstoned(10)); // ack of a carrying tick clears it
      Assert::AreEqual(size_t{ 0 }, base.TombstoneCount());
    }

    TEST_METHOD(StaleAckBeforeCarryingTickDoesNotClear)
    {
      ClientBaseline base;
      base.Tombstone(10);
      base.RecordTombstonesSent(5, { 10 });
      base.Ack(4);
      Assert::IsTrue(base.IsTombstoned(10)); // tick 4 predates the leave
      base.Ack(5);
      Assert::IsTrue(!base.IsTombstoned(10));
    }

    TEST_METHOD(ReEnteredEntityIsUntombstoned)
    {
      ClientBaseline base;
      base.Tombstone(10);
      Assert::IsTrue(base.IsTombstoned(10));
      base.Untombstone(10);
      Assert::IsTrue(!base.IsTombstoned(10));
    }

    TEST_METHOD(ServerUniverseEvictsEntityThatLeftInterest)
    {
      ServerUniverse su(false);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      const uint32_t prop = su.SpawnProp(0, { 100, 0, 0 });
      su.Step(0.1f);

      auto changed = su.ChangedFor(base);
      Assert::IsTrue(std::find(changed.begin(), changed.end(), prop) != changed.end());
      su.RecordSent(base, su.Tick(), changed);
      su.AckBaseline(base, su.Tick());
      Assert::IsTrue(su.TombstonesFor(base).empty());

      Assert::IsTrue(su.DespawnBase(prop)); // leaves the grid → leaves interest
      su.Step(0.1f);
      const auto tombs1 = su.TombstonesFor(base);
      Assert::IsTrue(std::find(tombs1.begin(), tombs1.end(), prop) != tombs1.end());
      su.RecordTombstonesSent(base, su.Tick(), tombs1); // sent but ack lost
      su.Step(0.1f);
      const auto tombs2 = su.TombstonesFor(base);
      Assert::IsTrue(std::find(tombs2.begin(), tombs2.end(), prop) != tombs2.end());

      su.RecordTombstonesSent(base, su.Tick(), tombs2);
      su.AckBaseline(base, su.Tick());
      su.Step(0.1f);
      Assert::IsTrue(su.TombstonesFor(base).empty());
    }
  };

  // --- Navigation -------------------------------------------------------------

  TEST_CLASS(NavigationTests)
  {
  public:
    TEST_METHOD(WarpTravelTimeProportionalToDistance)
    {
      // align is fixed; the travel part scales linearly with distance.
      const float a1 = WarpArrivalSeconds(1000.0, 100.0f, 2.0f); // 2 + 10
      const float a2 = WarpArrivalSeconds(2000.0, 100.0f, 2.0f); // 2 + 20
      Assert::IsTrue((a1 - 2.0f) == 10.0f);
      Assert::IsTrue((a2 - 2.0f) == 2.0f * (a1 - 2.0f)); // doubling distance doubles travel time
    }

    TEST_METHOD(WarpArrivesAtTargetExactly)
    {
      NavTuning t; // unused here (StepNav reads cooldown from it)
      NavState nav; Transform tr; tr.pos = { 0, 0, 0 };
      BeginWarp(nav, { 10000, 0, 0 }, 1000.0f, 0.0f); // speed 1000 m/s, no align

      NavEvent ev = NavEvent::None;
      int guard = 0;
      while (nav.phase != NavPhase::Idle && guard++ < 1000)
        ev = StepNav(nav, tr, t, 1.0f);

      Assert::IsTrue(ev == NavEvent::Arrived);
      const UniversePos dest{ 10000, 0, 0 };
      Assert::IsTrue(tr.pos == dest);
    }

    TEST_METHOD(CheckJumpReadyFuelAndBusy)
    {
      NavState nav; Fuel f{ 100.0f, 100.0f };
      Assert::IsTrue(CheckJumpReady(nav, f, 20.0f) == JumpReject::Accepted);

      f.current = 10.0f; // below cost
      Assert::IsTrue(CheckJumpReady(nav, f, 20.0f) == JumpReject::NoFuel);

      f.current = 100.0f; nav.phase = NavPhase::Cooldown; // mid-cycle
      Assert::IsTrue(CheckJumpReady(nav, f, 20.0f) == JumpReject::Busy);
    }

    TEST_METHOD(LoadUniverseSpawnsBeacons)
    {
      ServerUniverse su(false);
      LoadFrom(su, kNavSrc);
      Assert::IsTrue(su.Universe().beacons.size() == size_t(3));
      Assert::IsTrue(su.BeaconNetId("HUB") != 0);
      Assert::IsTrue(su.BeaconNetId("RIM") != 0);
      Assert::IsTrue(su.BeaconNetId("nope") == 0);
    }

    TEST_METHOD(JumpAcrossLinkedBeaconsConsumesFuelAndArrives)
    {
      ServerUniverse su(false);
      LoadFrom(su, kNavSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 }); // sitting on HUB
      Assert::IsTrue(su.FuelOf(base)->current == 100.0f);

      const JumpReject r = su.BeginJumpTo(base, "RIM"); // HUB <-> RIM linked
      Assert::IsTrue(r == JumpReject::Accepted);
      Assert::IsTrue(su.FuelOf(base)->current == 80.0f);      // 100 - 20
      Assert::IsTrue(su.NavOf(base)->phase == NavPhase::Spool);

      const UniversePos hub{ 0, 0, 0 };
      const UniversePos rim{ 100000, 0, 0 };
      su.Step(0.5f); // spool 1.0 -> 0.5
      UniversePos p; Assert::IsTrue(su.GetBasePos(base, p));
      Assert::IsTrue(p == hub);                               // still at source during spool
      su.Step(0.5f); // spool fires -> teleport to RIM, enter cooldown

      Assert::IsTrue(su.GetBasePos(base, p));
      Assert::IsTrue(p == rim);
      Assert::IsTrue(su.NavOf(base)->phase == NavPhase::Cooldown);

      su.Step(0.5f); su.Step(0.5f); // cooldown 1.0 elapses
      Assert::IsTrue(su.NavOf(base)->phase == NavPhase::Idle);
    }

    TEST_METHOD(JumpRejectedWhenNotLinked)
    {
      ServerUniverse su(false);
      LoadFrom(su, kNavSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 }); // on HUB
      // HUB links only RIM; FAR is not reachable in one jump from HUB.
      Assert::IsTrue(su.BeginJumpTo(base, "FAR") == JumpReject::NotLinked);
    }

    TEST_METHOD(JumpRejectedWhenNotAtBeacon)
    {
      ServerUniverse su(false);
      LoadFrom(su, kNavSrc);
      const uint32_t base = su.SpawnBase({ 50000, 0, 0 }, { 0, 0, 0 }); // far from every beacon
      Assert::IsTrue(su.BeginJumpTo(base, "RIM") == JumpReject::NotAtBeacon);
    }

    TEST_METHOD(JumpRejectedWhenOutOfFuel)
    {
      ServerUniverse su(false);
      LoadFrom(su, kNavSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      su.FuelOf(base)->current = 5.0f; // below the 20 cost
      Assert::IsTrue(su.BeginJumpTo(base, "RIM") == JumpReject::NoFuel);
      Assert::IsTrue(su.FuelOf(base)->current == 5.0f); // not charged on rejection
    }

    TEST_METHOD(InterdictionDropsBaseOutOfWarp)
    {
      ServerUniverse su(false);
      LoadFrom(su, kNavSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      Assert::IsTrue(su.BeginWarpTo(base, { 300000, 0, 0 })); // align 0, base speed 10000 m/s

      su.Step(0.1f); // Align -> Warp (no move yet)
      su.Step(0.1f); // Warp: ~1000 m toward target
      UniversePos mid; Assert::IsTrue(su.GetBasePos(base, mid));
      Assert::IsTrue(mid.x > 0 && mid.x < 300000);

      su.Interdict(base);
      su.Step(0.1f); // interdicted -> dropped out of warp
      Assert::IsTrue(su.NavOf(base)->phase == NavPhase::Idle);

      UniversePos after; Assert::IsTrue(su.GetBasePos(base, after));
      Assert::IsTrue(after.x < 300000); // never reached the destination
    }

    TEST_METHOD(BusyUnitCannotStartASecondTravel)
    {
      ServerUniverse su(false);
      LoadFrom(su, kNavSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      Assert::IsTrue(su.BeginWarpTo(base, { 300000, 0, 0 }));
      // already aligning/warping -> a jump must be refused
      Assert::IsTrue(su.BeginJumpTo(base, "RIM") == JumpReject::Busy);
      // and a second warp too
      Assert::IsTrue(!su.BeginWarpTo(base, { 100000, 0, 0 }));
    }

    TEST_METHOD(InterestPrefetchRecordsDestinationSector)
    {
      ServerUniverse su(false);
      LoadFrom(su, kNavSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      su.BeginWarpTo(base, { 1310720, 0, 0 }); // sector x = 1310720 >> 14 = 80
      Assert::IsTrue(su.LastTravelSector().x == 80);
    }
  };

  // --- Economy ----------------------------------------------------------------

  TEST_CLASS(EconomyTests)
  {
  public:
    TEST_METHOD(HarvestDepletesNodeAndFillsCargo)
    {
      ResourceNodeTag node; node.type = static_cast<uint8_t>(ResourceType::Ore); node.remaining = 100.0f;
      Cargo cargo; cargo.capacity = 1000.0f;

      Assert::IsTrue(HarvestStep(node, cargo, 50.0f, 1.0f) == 50.0f); // rate 50 x dt 1
      Assert::IsTrue(node.remaining == 50.0f);
      Assert::IsTrue(cargo.amount[kOre] == 50.0f);

      Assert::IsTrue(HarvestStep(node, cargo, 1000.0f, 1.0f) == 50.0f); // clamps to remaining
      Assert::IsTrue(node.remaining == 0.0f);
      Assert::IsTrue(HarvestStep(node, cargo, 50.0f, 1.0f) == 0.0f);    // empty node
    }

    TEST_METHOD(HarvestClampsToCargoCapacity)
    {
      ResourceNodeTag node; node.type = 0; node.remaining = 1000.0f;
      Cargo cargo; cargo.capacity = 30.0f;
      Assert::IsTrue(HarvestStep(node, cargo, 100.0f, 1.0f) == 30.0f); // wants 100, only 30 fits
      Assert::IsTrue(CargoFree(cargo) == 0.0f);
    }

    TEST_METHOD(DepositMovesCargoToStorageClamped)
    {
      Cargo c; c.capacity = 1000.0f; c.amount[kOre] = 100.0f; c.amount[kIce] = 50.0f;
      Storage s; s.capacity = 1000.0f;
      Assert::IsTrue(DepositAll(c, s) == 150.0f);
      Assert::IsTrue(s.amount[kOre] == 100.0f && s.amount[kIce] == 50.0f);
      Assert::IsTrue(c.amount[kOre] == 0.0f && c.amount[kIce] == 0.0f);

      Cargo c2; c2.capacity = 1000.0f; c2.amount[kOre] = 100.0f;
      Storage s2; s2.capacity = 40.0f; // only 40 room
      Assert::IsTrue(DepositAll(c2, s2) == 40.0f);
      Assert::IsTrue(s2.amount[kOre] == 40.0f && c2.amount[kOre] == 60.0f);
    }

    TEST_METHOD(BuildPaysOnceThenCompletes)
    {
      EconomyTuning e; e.buildOreCost = 100.0f; e.buildIceCost = 50.0f; e.buildSeconds = 2.0f;
      BuildQueue q; q.active = true;
      Storage s; s.capacity = 1000.0f; s.amount[kOre] = 200.0f; s.amount[kIce] = 100.0f;

      Assert::IsTrue(BuildStep(q, s, e, 1.0f) == BuildResult::InProgress);
      Assert::IsTrue(q.paid);
      Assert::IsTrue(s.amount[kOre] == 100.0f && s.amount[kIce] == 50.0f); // charged once
      Assert::IsTrue(BuildStep(q, s, e, 1.0f) == BuildResult::Completed);  // progress 2 >= 2
      Assert::IsTrue(!q.active);
    }

    TEST_METHOD(BuildRejectedWhenInsufficient)
    {
      EconomyTuning e; e.buildOreCost = 100.0f; e.buildIceCost = 50.0f; e.buildSeconds = 2.0f;
      BuildQueue q; q.active = true;
      Storage s; s.capacity = 1000.0f; s.amount[kOre] = 50.0f; // not enough ore
      Assert::IsTrue(BuildStep(q, s, e, 1.0f) == BuildResult::Insufficient);
      Assert::IsTrue(!q.active);
      Assert::IsTrue(s.amount[kOre] == 50.0f); // not charged
    }

    TEST_METHOD(FuelAndSensorRules)
    {
      Fuel f{ 100.0f, 100.0f };
      Assert::IsTrue(ConsumeFuel(f, 30.0f));
      Assert::IsTrue(f.current == 70.0f);
      Assert::IsTrue(!ConsumeFuel(f, 1000.0f)); // insufficient -> unchanged
      Assert::IsTrue(f.current == 70.0f);

      const UniversePos a{ 0, 0, 0 }, nearby{ 1000, 0, 0 }, farPos{ 10000, 0, 0 };
      Assert::IsTrue(SensorDetect(a, nearby, 5000.0f));
      Assert::IsTrue(!SensorDetect(a, farPos, 5000.0f));
    }

    TEST_METHOD(BaseHasStorageAndSensorFromTuning)
    {
      ServerUniverse su(false);
      LoadFrom(su, kEconSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      Assert::IsTrue(su.StorageOf(base) != nullptr);
      Assert::IsTrue(su.StorageOf(base)->capacity == 2000.0f);
      Assert::IsTrue(su.BuildQueueOf(base) != nullptr);
      Assert::IsTrue(su.Economy().fleetCap == 2);
    }

    TEST_METHOD(FleetCapIsEnforced)
    {
      ServerUniverse su(false);
      LoadFrom(su, kEconSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      const uint32_t player = base; // a player ~ their base net id
      Assert::IsTrue(su.SpawnFleetShip(player, ServerUniverse::ShipShapeId(), { 0, 0, 0 }) != 0);
      Assert::IsTrue(su.SpawnFleetShip(player, ServerUniverse::ShipShapeId(), { 0, 0, 0 }) != 0);
      Assert::IsTrue(su.SpawnFleetShip(player, ServerUniverse::ShipShapeId(), { 0, 0, 0 }) == 0); // cap 2
      Assert::IsTrue(su.OwnedShipCount(player) == 2);
    }

    TEST_METHOD(BuildQueueSpawnsAShip)
    {
      ServerUniverse su(false);
      LoadFrom(su, kEconSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      // stock the base storage with enough to build (needs 100 ore, 50 ice)
      su.StorageOf(base)->amount[kOre] = 200.0f;
      su.StorageOf(base)->amount[kIce] = 100.0f;

      Assert::IsTrue(su.EnqueueBuild(base));
      Assert::IsTrue(su.OwnedShipCount(base) == 0);

      su.Step(0.5f); // pays + progresses (build_seconds = 1)
      Assert::IsTrue(su.DrainBuildCompleted().empty());
      su.Step(0.5f); // completes -> ship spawns

      auto completed = su.DrainBuildCompleted();
      Assert::IsTrue(completed.size() == size_t(1));
      Assert::IsTrue(su.OwnedShipCount(base) == 1);
      Assert::IsTrue(su.StorageOf(base)->amount[kOre] == 100.0f); // charged once
      Assert::IsTrue(!su.BuildQueueOf(base)->active);
    }

    TEST_METHOD(BuildWithEmptyStorageSpawnsNothing)
    {
      ServerUniverse su(false);
      LoadFrom(su, kEconSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 }); // storage empty
      Assert::IsTrue(su.EnqueueBuild(base));
      su.Step(0.5f);
      Assert::IsTrue(su.DrainBuildCompleted().empty());
      Assert::IsTrue(su.OwnedShipCount(base) == 0);
      Assert::IsTrue(!su.BuildQueueOf(base)->active); // cancelled - insufficient
    }

    TEST_METHOD(DefaultConstructorSeedsVisibleDemoContent)
    {
      ServerUniverse su; // default -> live seed (scenery + an autonomous demo economy)
      Assert::IsTrue(su.BeaconNetId("DEMO_GATE_W") != 0); // working, jump-linked beacons
      Assert::IsTrue(su.BeaconNetId("DEMO_GATE_E") != 0);

      const Snapshot snap = su.BuildSnapshot();
      int nodes = 0, bases = 0, structures = 0;
      for (const auto& e : snap.entities) {
        if      (e.kind == EntityKind::ResourceNode) ++nodes;
        else if (e.kind == EntityKind::Base)         ++bases;
        else if (e.kind == EntityKind::Structure)    ++structures;
      }
      Assert::IsTrue(nodes >= 1);      // the harvestable asteroid
      Assert::IsTrue(bases >= 1);      // the autonomous demo station
      Assert::IsTrue(structures >= 2); // the two demo beacons (+ scenery jumpgate)
    }
  };

  // --- Harvest loop -----------------------------------------------------------

  TEST_CLASS(HarvestTests)
  {
  public:
    TEST_METHOD(FullLoopNodeToCargoToStorageToShip)
    {
      ServerUniverse su(false);
      LoadFrom(su, kHarvestEcon);

      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      const uint32_t harv = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 500, 0, 0 });
      const uint32_t node = su.SpawnResourceNode(ResourceType::Ore, 2000.0f, { 5000, 0, 0 });
      Assert::IsTrue(harv != 0 && node != 0);
      Assert::IsTrue(su.OwnedShipCount(base) == 1); // the harvester

      Assert::IsTrue(su.OrderHarvest(harv, node));

      // Run the loop: the harvester shuttles node<->base, ore flows node -> cargo -> storage.
      for (int i = 0; i < 80 && su.StorageOf(base)->amount[kOre] < 400.0f; ++i)
        su.Step(0.1f);

      Assert::IsTrue(su.StorageOf(base)->amount[kOre] >= 400.0f); // returned + deposited
      Assert::IsTrue(su.ResourceNodeOf(node)->remaining < 2000.0f); // node depleted

      // Enqueue a build off the deposited ore -> a ship is born.
      Assert::IsTrue(su.EnqueueBuild(base));
      for (int i = 0; i < 20 && su.OwnedShipCount(base) < 2; ++i)
        su.Step(0.1f);

      Assert::IsTrue(su.OwnedShipCount(base) == 2); // harvester + the built ship
      Assert::IsTrue(!su.DrainBuildCompleted().empty());
    }

    TEST_METHOD(HarvesterDepositsThenIdlesWhenNodeEmpty)
    {
      ServerUniverse su(false);
      LoadFrom(su, kHarvestEcon);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      const uint32_t harv = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 100, 0, 0 });
      const uint32_t node = su.SpawnResourceNode(ResourceType::Ore, 150.0f, { 2000, 0, 0 }); // less than one cargo
      Assert::IsTrue(su.OrderHarvest(harv, node));

      for (int i = 0; i < 40 && su.HarvestOrderOf(harv)->phase != HarvestPhase::Idle; ++i)
        su.Step(0.1f);

      Assert::IsTrue(su.HarvestOrderOf(harv)->phase == HarvestPhase::Idle); // finished (node drained)
      Assert::IsTrue(su.ResourceNodeOf(node)->remaining == 0.0f);
      Assert::IsTrue(su.StorageOf(base)->amount[kOre] == 150.0f);          // all of it banked
      Assert::IsTrue(su.CargoOf(harv)->amount[kOre] == 0.0f);              // cargo emptied on deposit
    }

    TEST_METHOD(OrderHarvestValidates)
    {
      ServerUniverse su(false);
      LoadFrom(su, kHarvestEcon);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      const uint32_t harv = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 0, 0, 0 });
      const uint32_t node = su.SpawnResourceNode(ResourceType::Ore, 100.0f, { 1000, 0, 0 });

      Assert::IsTrue(su.OrderHarvest(harv, node));      // ok
      Assert::IsTrue(!su.OrderHarvest(harv, base));     // base is not a resource node
      Assert::IsTrue(!su.OrderHarvest(node, node));     // a node can't harvest (no Cargo/Owner)
      Assert::IsTrue(!su.OrderHarvest(99999, node));    // unknown ship
    }

    TEST_METHOD(FieldsSpawnResourceNodes)
    {
      ServerUniverse su(false);
      LoadFrom(su,
          "region R { security = high bounds = -64 64 -64 64 -64 64 yield_mult = 1 }\n"
          "field BELT { region = R center = 0 0 0 radius = 1000 nodes = Ore:0.6 Ice:0.4\n"
          "            count = 4 4 yield = 100 200 respawn = 60 }\n");

      const Snapshot snap = su.BuildSnapshot();
      int nodes = 0;
      for (const auto& e : snap.entities)
        if (e.kind == EntityKind::ResourceNode) ++nodes;
      Assert::IsTrue(nodes == 4); // clamp(countMin=4, 1, 8)
    }
  };

  // --- Universe cook/check pipeline -------------------------------------------

  TEST_CLASS(UniverseDataTests)
  {
  public:
    TEST_METHOD(ParsesCountsAndFields)
    {
      UniverseDataset ds = ParseUniverseOk(kGoodUniverse);
      Assert::IsTrue(ds.regions.size() == size_t(2));
      Assert::IsTrue(ds.beacons.size() == size_t(3));
      Assert::IsTrue(ds.fields.size() == size_t(1));

      const RegionDef* home = ds.FindRegion("HOME");
      Assert::IsTrue(home != nullptr);
      Assert::IsTrue(home->security == SecurityTier::High);
      Assert::IsTrue(home->bounds.x0 == -16 && home->bounds.x1 == 16);

      const BeaconDef* b = ds.FindBeacon("B");
      Assert::IsTrue(b != nullptr);
      Assert::IsTrue(b->links.size() == size_t(2));
      Assert::IsTrue(b->pos.x == 327680);

      Assert::IsTrue(ds.fields[0].nodes.size() == 2);
      Assert::IsTrue(ds.fields[0].countMin == 4 && ds.fields[0].countMax == 10);
    }

    TEST_METHOD(ValidGraphPasses)
    {
      UniverseDataset ds = ParseUniverseOk(kGoodUniverse);
      std::vector<std::string> errs;
      Assert::IsTrue(ValidateUniverseDataset(ds, errs));
      Assert::IsTrue(errs.empty());
    }

    TEST_METHOD(BinaryRoundTrip)
    {
      UniverseDataset ds = ParseUniverseOk(kGoodUniverse);
      const auto bytes = EncodeUniverseDataset(ds);
      auto rt = DecodeUniverseDataset(bytes);
      Assert::IsTrue(rt.has_value());
      // Re-encoding the decoded copy must be byte-identical (stable round-trip).
      const auto bytes2 = EncodeUniverseDataset(*rt);
      Assert::IsTrue(bytes == bytes2);
      // Spot-check a few decoded values survived.
      Assert::IsTrue(rt->beacons.size() == size_t(3));
      Assert::IsTrue(rt->FindBeacon("C") != nullptr);
      Assert::IsTrue(rt->FindBeacon("C")->region == "EDGE");
      Assert::IsTrue(rt->beacons[1].pos.x == 327680);
    }

    TEST_METHOD(RejectsUnknownRegionRef)
    {
      auto ds = ParseUniverseOk(
          "region HOME { security = high bounds = 0 4 0 4 0 4 yield_mult = 1 }\n"
          "beacon A { region = NOPE pos = 0 0 0 links = kind = public }\n");
      std::vector<std::string> errs;
      Assert::IsTrue(!ValidateUniverseDataset(ds, errs));
      Assert::IsTrue(!errs.empty());
    }

    TEST_METHOD(RejectsNonReciprocalLink)
    {
      // A -> B, but B does not link back to A.
      auto ds = ParseUniverseOk(
          "region R { security = low bounds = 0 9 0 9 0 9 yield_mult = 1 }\n"
          "beacon A { region = R pos = 0 0 0 links = B kind = public }\n"
          "beacon B { region = R pos = 1 0 0 links =   kind = public }\n");
      std::vector<std::string> errs;
      Assert::IsTrue(!ValidateUniverseDataset(ds, errs));
    }

    TEST_METHOD(RejectsClaimableInHighSec)
    {
      auto ds = ParseUniverseOk(
          "region HOME { security = high bounds = 0 9 0 9 0 9 yield_mult = 1 }\n"
          "beacon A { region = HOME pos = 0 0 0 links = kind = claimable }\n");
      std::vector<std::string> errs;
      Assert::IsTrue(!ValidateUniverseDataset(ds, errs));
    }

    TEST_METHOD(RejectsDisconnectedPublicIsland)
    {
      // A-B connected; C is a lone public island.
      auto ds = ParseUniverseOk(
          "region R { security = low bounds = 0 99 0 9 0 9 yield_mult = 1 }\n"
          "beacon A { region = R pos = 0 0 0 links = B kind = public }\n"
          "beacon B { region = R pos = 1 0 0 links = A kind = public }\n"
          "beacon C { region = R pos = 2 0 0 links =   kind = public }\n");
      std::vector<std::string> errs;
      Assert::IsTrue(!ValidateUniverseDataset(ds, errs));
    }

    TEST_METHOD(RejectsBadNodeWeights)
    {
      // Weights sum to 0.5, not ~1.0.
      auto ds = ParseUniverseOk(
          "region R { security = low bounds = 0 9 0 9 0 9 yield_mult = 1 }\n"
          "field F { region = R center = 0 0 0 radius = 100 nodes = Ore:0.3 Ice:0.2"
          " count = 1 2 yield = 1 2 respawn = 1 }\n");
      std::vector<std::string> errs;
      Assert::IsTrue(!ValidateUniverseDataset(ds, errs));
    }

    TEST_METHOD(ReportsSyntaxErrorWithLine)
    {
      UniverseDataset ds;
      std::vector<std::string> errs;
      const bool ok = ParseUniverseSource(
          "region R { security = high bounds = 0 4 0 4 0 4 yield_mult = 1 }\n"
          "beacon A { bogus = 3 }\n", ds, errs);
      Assert::IsTrue(!ok);
      Assert::IsTrue(!errs.empty());
      // Error should mention the offending line (line 2).
      Assert::IsTrue(errs[0].find("line 2") != std::string::npos);
    }

    TEST_METHOD(ParsesTuningBlockAndKeepsDefaults)
    {
      auto ds = ParseUniverseOk(
          "region R { security = low bounds = 0 9 0 9 0 9 yield_mult = 1 }\n"
          "tuning { warp_speed_ship = 7000  jump_fuel_base = 50  base_fuel_max = 250 }\n");
      Assert::IsTrue(ds.nav.warpSpeedShip == 7000.0f);
      Assert::IsTrue(ds.nav.jumpFuelBase == 50.0f);
      Assert::IsTrue(ds.nav.baseFuelMax == 250.0f);
      Assert::IsTrue(ds.nav.warpSpeedBase == 2000.0f); // untouched key keeps its default
      std::vector<std::string> errs;
      Assert::IsTrue(ValidateUniverseDataset(ds, errs));
      // The tuning survives a binary round-trip.
      auto rt = DecodeUniverseDataset(EncodeUniverseDataset(ds));
      Assert::IsTrue(rt.has_value() && rt->nav.warpSpeedShip == 7000.0f && rt->nav.baseFuelMax == 250.0f);
    }

    TEST_METHOD(RejectsBadTuning)
    {
      auto ds = ParseUniverseOk(
          "region R { security = low bounds = 0 9 0 9 0 9 yield_mult = 1 }\n"
          "tuning { warp_speed_ship = 0 }\n"); // warp speed must be > 0
      std::vector<std::string> errs;
      Assert::IsTrue(!ValidateUniverseDataset(ds, errs));
    }
  };

  // --- fleet command / fog / PvE (M3 areas B, E, F) ---------------------------
  // Mirrors NeuronTools/testrunner/FleetTests.cpp.

  const char* kFleetSrc =
      "region R { security = high bounds = -64 64 -64 64 -64 64 yield_mult = 1 }\n"
      "economy { fleet_cap = 8  cargo_capacity = 500  storage_capacity = 2000  harvest_rate = 100\n"
      "          sensor_range_ship = 8000  sensor_range_base = 20000  build_ore = 100  build_ice = 0\n"
      "          build_seconds = 1  build_ship_type = 1  harvester_speed = 4000  harvest_range = 600 }\n";

  TEST_CLASS(FleetCommandTests)
  {
  public:
    TEST_METHOD(CommandRoundTrips)
    {
      FleetCommand c;
      c.clientTick = 42; c.intent = IntentType::Attack; c.queue = true;
      c.units = { 7, 9, 11 }; c.targetNetId = 1234; c.targetPoint = { 100, -200, 300 };
      c.range = 750.0f; c.beacon = "RIM";
      FleetCommand d;
      Assert::IsTrue(DecodeFleetCommand(EncodeFleetCommand(c), d));
      Assert::IsTrue(d.intent == IntentType::Attack && d.queue && d.units.size() == 3);
      Assert::IsTrue(d.units[2] == 11 && d.targetNetId == 1234 && d.range == 750.0f);
      Assert::IsTrue(d.targetPoint == UniversePos({ 100, -200, 300 }) && d.beacon == "RIM");
    }

    TEST_METHOD(DecodeRejectsWrongVersion)
    {
      auto bytes = EncodeFleetCommand({});
      bytes[0] = 0xEE;
      FleetCommand d;
      Assert::IsTrue(!DecodeFleetCommand(bytes, d));
    }

    TEST_METHOD(MoveIntentSteersOwnedShipToPoint)
    {
      ServerUniverse su(false); LoadFrom(su, kFleetSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      const uint32_t ship = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 0, 0, 0 });
      FleetCommand cmd; cmd.intent = IntentType::Move; cmd.units = { ship }; cmd.targetPoint = { 6000, 0, 0 };
      Assert::IsTrue(su.ApplyFleetCommand(base, cmd) == 1);
      for (int i = 0; i < 200 && su.FleetOrderOf(ship)->current.type != OrderType::Idle; ++i) su.Step(0.1f);
      UniversePos p; Assert::IsTrue(su.GetBasePos(ship, p));
      Assert::IsTrue(p == UniversePos({ 6000, 0, 0 }));
    }

    TEST_METHOD(CommandRejectedForUnownedUnit)
    {
      ServerUniverse su(false); LoadFrom(su, kFleetSrc);
      const uint32_t baseA = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      const uint32_t baseB = su.SpawnBase({ 0, 5000, 0 }, { 0, 0, 0 });
      const uint32_t shipB = su.SpawnFleetShip(baseB, ServerUniverse::ShipShapeId(), { 0, 5000, 0 });
      FleetCommand cmd; cmd.intent = IntentType::Move; cmd.units = { shipB }; cmd.targetPoint = { 9999, 0, 0 };
      Assert::IsTrue(su.ApplyFleetCommand(baseA, cmd) == 0);
      Assert::IsTrue(su.FleetOrderOf(shipB)->current.type == OrderType::Idle);
    }

    TEST_METHOD(OrdersQueuePreserveOrder)
    {
      ServerUniverse su(false); LoadFrom(su, kFleetSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      const uint32_t ship = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 0, 0, 0 });
      auto move = [&](int64_t x, bool q) {
        FleetCommand c; c.intent = IntentType::Move; c.units = { ship }; c.targetPoint = { x, 0, 0 }; c.queue = q;
        Assert::IsTrue(su.ApplyFleetCommand(base, c) == 1);
      };
      move(2000, false); move(4000, true); move(6000, true);
      FleetOrder* fo = su.FleetOrderOf(ship);
      Assert::IsTrue(fo->current.targetPoint.x == 2000 && fo->queue.size() == 2);
      Assert::IsTrue(fo->queue[0].targetPoint.x == 4000 && fo->queue[1].targetPoint.x == 6000);
    }

    TEST_METHOD(BuildIntentEnqueuesAtBase)
    {
      ServerUniverse su(false); LoadFrom(su, kFleetSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      su.StorageOf(base)->amount[kOre] = 1000.0f;
      FleetCommand c; c.intent = IntentType::Build; c.units = { base };
      Assert::IsTrue(su.ApplyFleetCommand(base, c) == 1 && su.BuildQueueOf(base)->active);
      for (int i = 0; i < 40 && su.OwnedShipCount(base) < 1; ++i) su.Step(0.1f);
      Assert::IsTrue(su.OwnedShipCount(base) == 1);
    }

    TEST_METHOD(DetectedSetAndFog)
    {
      ServerUniverse su(false); LoadFrom(su, kFleetSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      const uint32_t nearN = su.SpawnResourceNode(ResourceType::Ore, 100.0f, { 10000, 0, 0 });
      const uint32_t farN  = su.SpawnResourceNode(ResourceType::Ore, 100.0f, { 80000, 0, 0 });
      const auto seen = su.DetectedSet(base);
      Assert::IsTrue(seen.count(base) == 1 && seen.count(nearN) == 1 && seen.count(farN) == 0);
      Assert::IsTrue(su.BuildSnapshot().entities.size() > su.BuildSnapshotFor(base).entities.size());
      Assert::IsTrue(!su.OrderScan(base, farN, 2.0f));
      Assert::IsTrue(su.OrderScan(base, farN, 2.0f) && su.IsRevealedTo(base, farN));
    }

    TEST_METHOD(AiStateTransitions)
    {
      NpcAi ai; ai.aggroRange = 5000.0f; ai.fleeHpFrac = 0.2f;
      Assert::IsTrue(NextAiState(ai, false, false, 1.0f) == AiState::Defend);
      Assert::IsTrue(NextAiState(ai, true, true, 1.0f) == AiState::Aggro);
      Assert::IsTrue(NextAiState(ai, true, false, 1.0f) == AiState::Defend);
      Assert::IsTrue(NextAiState(ai, true, true, 0.1f) == AiState::Flee);
    }

    TEST_METHOD(LowDpsStillDamagesAtSimTickRate)
    {
      ServerUniverse su(false); LoadFrom(su, kFleetSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      const uint32_t ship = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 0, 0, 0 });
      su.WeaponOf(ship)->dps = 20.0f; // below the 30 Hz tick → < 1 dmg per tick
      const uint16_t site = su.SpawnNpcSite({ 200, 0, 0 }, 1, 50.0f);
      const uint32_t npc = su.NpcSiteMembers(site).front();
      FleetCommand atk; atk.intent = IntentType::Attack; atk.units = { ship }; atk.targetNetId = npc;
      Assert::IsTrue(su.ApplyFleetCommand(base, atk) == 1);
      const int32_t hp0 = su.HealthOf(npc)->hp;
      for (int i = 0; i < 90; ++i) su.Step(1.0f / 30.0f);
      Assert::IsTrue(su.HealthOf(npc)->hp < hp0);
    }

    TEST_METHOD(AttackerClosesToItsOwnWeaponRange)
    {
      ServerUniverse su(false); LoadFrom(su, kFleetSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      const uint32_t ship = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 0, 0, 0 });
      const float shipRange = su.WeaponOf(ship)->range;
      const uint16_t site = su.SpawnNpcSite({ 40000, 0, 0 }, 1, 50.0f);
      const uint32_t npc = su.NpcSiteMembers(site).front();
      su.WeaponOf(npc)->range = shipRange * 4.0f;
      su.NpcAiOf(npc)->aggroRange = 0.0f;
      FleetCommand atk; atk.intent = IntentType::Attack; atk.units = { ship }; atk.targetNetId = npc;
      Assert::IsTrue(su.ApplyFleetCommand(base, atk) == 1);
      const int32_t hp0 = su.HealthOf(npc)->hp;
      for (int i = 0; i < 200 && su.HealthOf(npc) && su.HealthOf(npc)->hp == hp0; ++i) su.Step(0.1f);
      UniversePos sp, np;
      Assert::IsTrue(su.GetBasePos(ship, sp) && su.GetBasePos(npc, np));
      Assert::IsTrue(UniverseDistance(sp, np) <= static_cast<double>(shipRange));
      Assert::IsTrue(su.HealthOf(npc)->hp < hp0);
    }

    TEST_METHOD(FleetClearsNpcSiteAndFiresOnce)
    {
      ServerUniverse su(false); LoadFrom(su, kFleetSrc);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      const uint16_t site = su.SpawnNpcSite({ 4000, 0, 0 }, 2, 400.0f);
      std::vector<uint32_t> ships;
      for (int i = 0; i < 3; ++i) ships.push_back(su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 4000, 0, 0 }));
      for (int i = 0; i < 600 && !su.IsNpcSiteCleared(site); ++i) {
        auto members = su.NpcSiteMembers(site);
        if (!members.empty()) {
          FleetCommand atk; atk.intent = IntentType::Attack; atk.units = ships; atk.targetNetId = members.front();
          su.ApplyFleetCommand(base, atk);
        }
        su.Step(0.1f);
      }
      Assert::IsTrue(su.IsNpcSiteCleared(site));
      auto cleared = su.DrainClearedSites();
      Assert::IsTrue(cleared.size() == 1 && cleared.front() == site);
      Assert::IsTrue(su.DrainClearedSites().empty());
    }
  };

  // --- record/replay determinism (M3 area H) ----------------------------------
  // Mirrors NeuronTools/testrunner/DeterminismTests.cpp (sans the NeuronClient
  // ScriptedController dep — commands are applied inline).

  const char* kDetSrc =
      "region R { security = high bounds = -64 64 -64 64 -64 64 yield_mult = 1 }\n"
      "beacon HUB { region = R pos = 0 0 0       links = RIM kind = public }\n"
      "beacon RIM { region = R pos = 200000 0 0  links = HUB kind = public }\n"
      "tuning { warp_align = 0 warp_speed_ship = 50000 jump_fuel_ship = 10 jump_spool_ship = 0.5\n"
      "         jump_cooldown = 0.5 beacon_range = 3000 ship_fuel_max = 100 base_fuel_max = 300 }\n"
      "economy { fleet_cap = 8 cargo_capacity = 200 storage_capacity = 10000 harvest_rate = 1000\n"
      "          build_ore = 300 build_ice = 0 build_seconds = 0.1 build_ship_type = 1\n"
      "          harvester_speed = 100000 harvest_range = 600 sensor_range_ship = 8000\n"
      "          sensor_range_base = 50000 }\n";

  TEST_CLASS(DeterminismTests)
  {
    static uint64_t RunScript(const char* src, uint32_t ticks, bool withCommands)
    {
      ServerUniverse su(false);
      UniverseDataset ds = ParseUniverseOk(src); su.LoadUniverse(ds);
      const uint32_t base = su.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
      const uint32_t harv = su.SpawnFleetShip(base, ServerUniverse::ShipShapeId(), { 500, 0, 0 });
      const uint32_t node = su.SpawnResourceNode(ResourceType::Ore, 5000.0f, { 4000, 0, 0 });
      for (uint32_t t = 0; t < ticks; ++t) {
        if (withCommands && t == 1) {
          FleetCommand h; h.intent = IntentType::Harvest; h.units = { harv }; h.targetNetId = node;
          su.ApplyFleetCommand(base, h);
        }
        if (withCommands && t == 60) {
          FleetCommand j; j.intent = IntentType::Jump; j.units = { harv }; j.beacon = "RIM";
          su.ApplyFleetCommand(base, j);
        }
        su.Step(0.1f);
      }
      return su.SimHash();
    }

  public:
    TEST_METHOD(SameLogReproducesSimHash)
    {
      Assert::IsTrue(RunScript(kDetSrc, 120, true) == RunScript(kDetSrc, 120, true));
    }
    TEST_METHOD(DivergentLogChangesSimHash)
    {
      Assert::IsTrue(RunScript(kDetSrc, 120, true) != RunScript(kDetSrc, 120, false));
    }
  };
} // namespace NeuronCoreTest
