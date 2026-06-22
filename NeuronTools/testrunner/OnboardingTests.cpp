// Onboarding objective-chain tests (playable slice). The chain advances purely on
// client-observable state (replica set + selection), so the live universe reads as
// a game with goals. Mirrors the platform-independent logic the EarthRise app drives
// (§16.2 Linux mirror). Component ids are bound once per binary in ShapeCatalogTests.

#include "Onboarding.h"
#include "TestRunner.h"

using namespace ertest;
using Neuron::Client::Onboarding;
using Neuron::Client::ObservedState;
using Neuron::Client::ObserveWorld;
using Neuron::Client::ReplicaSet;
using Step = Neuron::Client::Onboarding::Step;
using K = Neuron::Sim::EntityKind;

namespace
{
    void Add(ReplicaSet& s, uint32_t id, K kind, uint32_t owner)
    {
        auto& e = s.entities[s.count++];
        e.networkId = id; e.entityType = static_cast<uint8_t>(kind);
        e.ownerPlayer = owner; e.valid = true;
    }
}

ER_TEST(Onboarding, WalksTheChainOnObservableMilestones)
{
    Onboarding ob;
    ER_CHECK(ob.Current() == Step::Welcome);

    ObservedState s;
    s.hasOwnBase = true;            // base shows up → past the welcome
    ob.Observe(s);
    ER_CHECK(ob.Current() == Step::Select);

    s.selectionCount = 2;          // player selects a fleet
    ob.Observe(s);
    ER_CHECK(ob.Current() == Step::Engage);

    s.npcVisible = 3;              // hostiles come into sensor range
    ob.Observe(s);
    ER_CHECK(ob.Current() == Step::Clear);

    s.npcVisible = 0;              // guardians destroyed
    ob.Observe(s);
    ER_CHECK(ob.Current() == Step::Done);
    ER_CHECK(ob.Complete());
}

ER_TEST(Onboarding, ClearNeedsTheSiteToHaveBeenSeenFirst)
{
    Onboarding ob;
    ObservedState s; s.hasOwnBase = true; s.selectionCount = 1;
    ob.Observe(s); // → Select
    ob.Observe(s); // → Engage
    // npcVisible never went above zero, so "Clear" must not fire on an empty world.
    s.npcVisible = 0;
    ob.Observe(s);
    ER_CHECK(ob.Current() == Step::Engage);
}

ER_TEST(Onboarding, ObserveWorldCountsOwnedShipsBaseAndNpcs)
{
    ReplicaSet set;
    const uint32_t me = 7;
    Add(set, 1, K::Base, me);       // my base
    Add(set, 2, K::Ship, me);       // my ship
    Add(set, 3, K::Ship, me);       // my ship
    Add(set, 4, K::Ship, 9);        // someone else's ship
    Add(set, 5, K::NpcUnit, 0);     // a guardian
    Add(set, 6, K::NpcUnit, 0);     // a guardian

    const ObservedState o = ObserveWorld(set, me, /*selectionCount*/ 2);
    ER_CHECK(o.hasOwnBase);
    ER_CHECK_EQ(o.ownedShips, uint32_t{ 2 });    // only mine
    ER_CHECK_EQ(o.npcVisible, uint32_t{ 2 });
    ER_CHECK_EQ(o.selectionCount, uint32_t{ 2 });
}

ER_TEST(Onboarding, TextIsNonEmptyAtEveryStep)
{
    Onboarding ob;
    ObservedState s;
    for (int i = 0; i < 6; ++i)
    {
        ER_CHECK(ob.CurrentText() != nullptr && ob.CurrentText()[0] != '\0');
        // nudge through the chain
        s.hasOwnBase = true; s.selectionCount = 1;
        if (i >= 2) s.npcVisible = (i == 3) ? 0u : 2u;
        ob.Observe(s);
    }
}
