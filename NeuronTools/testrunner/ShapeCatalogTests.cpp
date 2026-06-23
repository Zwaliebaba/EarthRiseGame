// ShapeCatalog + ECS/snapshot integration tests (ShapeCatalog.h, Components.h,
// Snapshot.h, ServerUniverse.h). Platform-independent: the catalog data, the ECS
// spawn path, and the snapshot wire format all run identically on server and
// client, so the Linux runner can verify them (a tiny DirectXMath shim supplies
// XMFLOAT3 — see dxmath_shim/). Mirrors the wire contract the Windows client
// relies on to pick a mesh per entity.

#include "ShapeCatalog.h"
#include "ServerUniverse.h"
#include "Snapshot.h"
#include "TestRunner.h"

using namespace Neuron::Sim;
using namespace ertest;

// Bind the sim ECS component ids in this TU (as SimComponents.cpp does for the
// server). Exactly one definition site per linked binary.
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
// M6 combat model (areas B–G).
NEURON_DEFINE_COMPONENT(Neuron::Sim::DefenseLayers, Neuron::Sim::Slot_DefenseLayers);
NEURON_DEFINE_COMPONENT(Neuron::Sim::ResistProfile, Neuron::Sim::Slot_ResistProfile);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Fitting, Neuron::Sim::Slot_Fitting);
NEURON_DEFINE_COMPONENT(Neuron::Sim::EwarStatus, Neuron::Sim::Slot_EwarStatus);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Projectile, Neuron::Sim::Slot_Projectile);
NEURON_DEFINE_COMPONENT(Neuron::Sim::LootContainer, Neuron::Sim::Slot_LootContainer);
NEURON_DEFINE_COMPONENT(Neuron::Sim::BaseCombat, Neuron::Sim::Slot_BaseCombat);
NEURON_DEFINE_COMPONENT(Neuron::Sim::HullInfo, Neuron::Sim::Slot_HullInfo);

ER_TEST(ShapeCatalog, CountAndSequentialIds)
{
  ER_CHECK_EQ(kShapeCount, 70u);
  for (uint16_t i = 0; i < kShapeCount; ++i)
  {
    ER_CHECK_EQ(kShapes[i].id, i);
    const auto p = kShapes[i].cmoPath;
    ER_CHECK(p.size() > 4 && p.substr(p.size() - 4) == ".cmo");
  }
}

ER_TEST(ShapeCatalog, NameLookup)
{
  ER_CHECK(ShapeIdByName("Jumpgate01") != kInvalidShapeId);
  ER_CHECK(ShapeIdByName("Outpost01") != kInvalidShapeId);
  ER_CHECK(ShapeIdByName("HullAurora") != kInvalidShapeId);
  ER_CHECK(ShapeIdByName("DoesNotExist") == kInvalidShapeId);
  // Round-trip: id -> def -> name.
  const uint16_t id = ShapeIdByName("HullAurora");
  ER_CHECK(ShapeById(id) != nullptr);
  ER_CHECK(ShapeById(id)->name == "HullAurora");
  ER_CHECK(ShapeById(kInvalidShapeId) == nullptr);
}

ER_TEST(ShapeCatalog, CategoryToKind)
{
  ER_CHECK(KindForCategory(ShapeCategory::Hull) == EntityKind::Ship);
  ER_CHECK(KindForCategory(ShapeCategory::Asteroid) == EntityKind::Asteroid);
  ER_CHECK(KindForCategory(ShapeCategory::Station) == EntityKind::Station);
  ER_CHECK(KindForCategory(ShapeCategory::Jumpgate) == EntityKind::Structure);
  ER_CHECK(KindForCategory(ShapeCategory::Crate) == EntityKind::LootContainer);
}

ER_TEST(ShapeCatalog, EveryCategoryHasAtLeastOneShape)
{
  for (uint8_t c = 0; c <= static_cast<uint8_t>(ShapeCategory::Station); ++c)
    ER_CHECK(FirstShapeOfCategory(static_cast<ShapeCategory>(c)) != kInvalidShapeId);
}

ER_TEST(ServerUniverse, ScenerySpawnedWithShapeAndKind)
{
  ServerUniverse w;
  const Snapshot snap = w.BuildSnapshot();
  ER_CHECK(!snap.entities.empty());

  // The jumpgate landmark is present and classified as a Structure.
  bool sawGate = false;
  const uint16_t gate = ShapeIdByName("Jumpgate01");
  for (const auto& e : snap.entities)
  {
    ER_CHECK(e.shapeId != kInvalidShapeId);
    if (e.shapeId == gate)
    {
      sawGate = true;
      ER_CHECK(e.kind == EntityKind::Structure);
    }
  }
  ER_CHECK(sawGate);
}

ER_TEST(ServerUniverse, SpawnedBaseCarriesBaseKind)
{
  ServerUniverse w;
  const uint32_t net = w.SpawnBase({ 0, 0, 0 }, { 0, 0, 0 });
  const Snapshot snap = w.BuildSnapshot();
  bool sawBase = false;
  for (const auto& e : snap.entities)
    if (e.netId == net)
    {
      sawBase = true;
      ER_CHECK(e.kind == EntityKind::Base);
      ER_CHECK(e.shapeId == ServerUniverse::BaseShapeId());
    }
  ER_CHECK(sawBase);
}

ER_TEST(Snapshot, RoundTripPreservesShapeIdAndKind)
{
  ServerUniverse w;
  w.SpawnBase({ 100, 0, 0 }, { 0, 0, 0 });
  const Snapshot snap = w.BuildSnapshot();

  const std::vector<uint8_t> bytes = EncodeSnapshot(snap);
  Snapshot decoded;
  ER_CHECK(DecodeSnapshot(bytes, decoded));
  ER_CHECK_EQ(decoded.entities.size(), snap.entities.size());
  for (size_t i = 0; i < decoded.entities.size(); ++i)
  {
    ER_CHECK_EQ(decoded.entities[i].shapeId, snap.entities[i].shapeId);
    ER_CHECK(decoded.entities[i].kind == snap.entities[i].kind);
    ER_CHECK_EQ(decoded.entities[i].netId, snap.entities[i].netId);
  }
}
