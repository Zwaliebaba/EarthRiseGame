// Client fleet-command logic tests (masterplan §22/§23; M3 area G) — the
// platform-independent client helpers: the smart context-action resolver, control
// groups, the overview contact list, and the starmap route solver. These mirror
// the NeuronClientTest cases the plan lists; the EarthRise UWP app's pointer/key
// wiring + rendering are Windows-only and out of the Linux runner's reach.
//
// Component ids are bound once per binary in ShapeCatalogTests.cpp.

#include "../datacook/UniverseSource.h" // ParseUniverseSource
#include "FleetControl.h"               // NeuronClient (smart action, control groups, overview)
#include "Starmap.h"                    // NeuronClient (beacon route solver)
#include "TestRunner.h"

#include <string>

using namespace Neuron::Client;
using namespace ertest;
using Neuron::Sim::EntityKind;
using Neuron::Sim::IntentType;

// --- smart context action ---------------------------------------------------

ER_TEST(ClientFleet, SmartActionResolvesByTargetType)
{
    // Classify then resolve, per §23.1 (empty=move, enemy=attack, node=harvest,
    // beacon=jump, ally=guard). selfPlayer = 100.
    ER_CHECK(ResolveSmartAction(ClassifyTarget(EntityKind::ResourceNode, 0, 100)) == IntentType::Harvest);
    ER_CHECK(ResolveSmartAction(ClassifyTarget(EntityKind::Structure, 0, 100))    == IntentType::Jump);
    ER_CHECK(ResolveSmartAction(ClassifyTarget(EntityKind::NpcUnit, 0, 100))      == IntentType::Attack);
    ER_CHECK(ResolveSmartAction(ClassifyTarget(EntityKind::Ship, 999, 100))       == IntentType::Attack); // enemy ship
    ER_CHECK(ResolveSmartAction(ClassifyTarget(EntityKind::Ship, 100, 100))       == IntentType::Guard);  // own ship
    ER_CHECK(ResolveSmartAction(ClassifyTarget(EntityKind::LootContainer, 0, 100)) == IntentType::ClaimLoot); // kill loot
    ER_CHECK(ResolveSmartAction(ClassifyTarget(EntityKind::Decoration, 0, 100))   == IntentType::Move);   // empty/scenery
}

ER_TEST(ClientFleet, MakeSmartCommandFillsTargetByType)
{
    const std::vector<uint32_t> sel{ 5, 6 };
    auto atk = MakeSmartCommand(SmartTarget::Enemy, sel, 42, { 0, 0, 0 });
    ER_CHECK(atk.intent == IntentType::Attack);
    ER_CHECK(atk.targetNetId == 42);
    ER_CHECK(atk.units.size() == 2);

    auto mv = MakeSmartCommand(SmartTarget::EmptySpace, sel, 0, { 10, 20, 30 });
    ER_CHECK(mv.intent == IntentType::Move);
    ER_CHECK(mv.targetNetId == 0);
    ER_CHECK(mv.targetPoint == Neuron::Universe::UniversePos({ 10, 20, 30 }));

    // Loot is an entity target → fills targetNetId (the container), like Attack.
    auto loot = MakeSmartCommand(SmartTarget::Loot, sel, 77, { 0, 0, 0 });
    ER_CHECK(loot.intent == IntentType::ClaimLoot);
    ER_CHECK(loot.targetNetId == 77);
}

// --- right-click context menu ----------------------------------------------

ER_TEST(ClientFleet, ContextMenuListsActionsByTargetType)
{
    using I = IntentType;
    // No selection → no menu (nothing to command).
    ER_CHECK(BuildContextMenu(SmartTarget::Enemy, false).empty());

    auto enemy = BuildContextMenu(SmartTarget::Enemy, true);
    ER_CHECK_EQ(enemy.size(), size_t{ 3 });
    ER_CHECK(enemy[0].intent == I::Attack);     // primary
    ER_CHECK(enemy[1].intent == I::Orbit);
    ER_CHECK(enemy[2].intent == I::KeepRange);

    ER_CHECK(BuildContextMenu(SmartTarget::ResourceNode, true).front().intent == I::Harvest);
    ER_CHECK(BuildContextMenu(SmartTarget::Loot, true).front().intent        == I::ClaimLoot);
    ER_CHECK(BuildContextMenu(SmartTarget::Ally, true).front().intent        == I::Guard);
    ER_CHECK(BuildContextMenu(SmartTarget::Beacon, true).front().intent      == I::Jump);
    ER_CHECK(BuildContextMenu(SmartTarget::EmptySpace, true).front().intent  == I::Move);
}

ER_TEST(ClientFleet, ContextMenuPrimaryMatchesSmartActionAndFlagsDeferred)
{
    for (auto t : { SmartTarget::Enemy, SmartTarget::ResourceNode, SmartTarget::Loot,
                    SmartTarget::Ally, SmartTarget::Beacon, SmartTarget::EmptySpace }) {
        const auto m = BuildContextMenu(t, true);
        ER_CHECK(!m.empty());
        ER_CHECK(m.front().intent == ResolveSmartAction(t)); // primary == smart action
    }
    // The two intents that need extra data are flagged so the UI can defer them.
    ER_CHECK(BuildContextMenu(SmartTarget::EmptySpace, true).front().needsPoint);  // Move
    ER_CHECK(BuildContextMenu(SmartTarget::Beacon, true).front().needsBeacon);     // Jump
}

// --- control groups ---------------------------------------------------------

ER_TEST(ClientFleet, ControlGroupSetRecall)
{
    ControlGroups cg;
    cg.Set(1, { 7, 7, 3, 9 }); // dedup + sort
    const auto& g = cg.Recall(1);
    ER_CHECK_EQ(g.size(), size_t(3));
    ER_CHECK(g[0] == 3 && g[1] == 7 && g[2] == 9);
    ER_CHECK(cg.Recall(2).empty());   // unset group
    ER_CHECK(cg.Recall(99).empty());  // out of range

    cg.Forget(7); // a unit died → drop it from groups
    const auto& g2 = cg.Recall(1);
    ER_CHECK_EQ(g2.size(), size_t(2));
    ER_CHECK(g2[0] == 3 && g2[1] == 9);
}

// --- overview list ----------------------------------------------------------

ER_TEST(ClientFleet, OverviewSortsByDistanceAndClassifiesIff)
{
    ReplicaSet rs;
    auto add = [&](uint32_t id, EntityKind k, uint32_t owner, float x) {
        ReplicaEntity& e = rs.entities[rs.count++];
        e.networkId = id; e.entityType = static_cast<uint8_t>(k);
        e.ownerPlayer = owner; e.x = x; e.y = 0; e.z = 0; e.valid = true;
    };
    const uint32_t self = 100;
    add(1, EntityKind::Ship,      999,  3000.0f); // enemy, far
    add(2, EntityKind::Ship,      100,  500.0f);  // ally, near
    add(3, EntityKind::NpcUnit,   0,    1500.0f); // enemy npc, mid
    add(4, EntityKind::ResourceNode, 0, 800.0f);  // node

    auto ov = BuildOverview(rs, self, 0, 0, 0);
    ER_CHECK_EQ(ov.size(), size_t(4));
    ER_CHECK(ov[0].netId == 2); // nearest
    ER_CHECK(ov[1].netId == 4);
    ER_CHECK(ov[2].netId == 3);
    ER_CHECK(ov[3].netId == 1); // farthest
    ER_CHECK(ov[0].iff == SmartTarget::Ally);
    ER_CHECK(ov[2].iff == SmartTarget::Enemy);
    ER_CHECK(ov[3].iff == SmartTarget::Enemy);

    // Filter to ships only (bit for EntityKind::Ship).
    const uint64_t shipMask = 1ull << static_cast<uint8_t>(EntityKind::Ship);
    auto ships = BuildOverview(rs, self, 0, 0, 0, shipMask);
    ER_CHECK_EQ(ships.size(), size_t(2));
    for (const auto& c : ships) ER_CHECK(c.kind == EntityKind::Ship);
}

// --- starmap route solver ---------------------------------------------------

namespace
{
    // HUB ↔ A ↔ B ↔ C  plus a HUB ↔ C shortcut → two routes of different length.
    const char* kGraph =
        "region R { security = high bounds = -64 64 -64 64 -64 64 yield_mult = 1 }\n"
        "beacon HUB { region = R pos = 0 0 0      links = A C   kind = public }\n"
        "beacon A   { region = R pos = 100 0 0    links = HUB B kind = public }\n"
        "beacon B   { region = R pos = 200 0 0    links = A C   kind = public }\n"
        "beacon C   { region = R pos = 300 0 0    links = B HUB kind = public }\n"
        "beacon ISL { region = R pos = 900 0 0    links =       kind = public }\n";

    Neuron::Sim::UniverseDataset Graph()
    {
        Neuron::Sim::UniverseDataset ds;
        std::vector<std::string> errs;
        const bool ok = Neuron::Tools::ParseUniverseSource(kGraph, ds, errs);
        ER_CHECK(ok && errs.empty());
        return ds;
    }
}

ER_TEST(ClientFleet, StarmapShortestRoute)
{
    const auto uni = Graph();
    auto route = SolveBeaconRoute(uni, "HUB", "C");
    // HUB→C direct shortcut (2 nodes) beats HUB→A→B→C (4).
    ER_CHECK_EQ(route.size(), size_t(2));
    ER_CHECK(route.front() == "HUB" && route.back() == "C");

    auto longer = SolveBeaconRoute(uni, "A", "C");
    // A→HUB→C and A→B→C are both 3 hops; BFS tie-breaks by link order (A lists HUB
    // first), so the deterministic result routes via HUB.
    ER_CHECK_EQ(longer.size(), size_t(3));
    ER_CHECK(longer[0] == "A" && longer[1] == "HUB" && longer[2] == "C");

    ER_CHECK(SolveBeaconRoute(uni, "HUB", "HUB").size() == 1); // self
    ER_CHECK(SolveBeaconRoute(uni, "HUB", "ISL").empty());     // island unreachable
    ER_CHECK(SolveBeaconRoute(uni, "HUB", "nope").empty());    // unknown
}

ER_TEST(ClientFleet, StarmapReachableSet)
{
    const auto uni = Graph();
    auto reach = ReachableBeacons(uni, "HUB");
    ER_CHECK(reach.count("HUB") && reach.count("A") && reach.count("B") && reach.count("C"));
    ER_CHECK(reach.count("ISL") == 0); // the island is not reachable
    ER_CHECK_EQ(reach.size(), size_t(4));
}
