#pragma once
// Tests_ClientInterp.h — InterpBuffer unit tests (M1b, Linux-compatible).
//
// Tests snap-on-ack interpolation for new entities and linear blend for
// known entities. No DirectXMath or Windows dependency.

#include "TestRunner.h"
#include "interp/Interpolator.h"
#include "replica/Replica.h"

namespace
{

inline Neuron::Client::ReplicaSet MakeReplicaSet(
    std::initializer_list<std::tuple<uint32_t, float, float, float, uint8_t>> ents)
{
    Neuron::Client::ReplicaSet rs;
    for (auto& [id, x, y, z, type] : ents) {
        if (rs.count >= Neuron::Client::ReplicaSet::kMaxEntities) break;
        auto& e = rs.entities[rs.count++];
        e.networkId  = id;
        e.x = x; e.y = y; e.z = z;
        e.entityType = type;
        e.valid      = true;
    }
    return rs;
}

} // anonymous namespace

TEST_SUITE(ClientInterp)
{
    // Entity seen for the first time (prev empty): should snap to curr.
    TEST_CASE(SnapOnFirstAppearance) {
        Neuron::Client::InterpBuffer buf;
        buf.curr  = MakeReplicaSet({{ 1u, 10.f, 20.f, 30.f, 0u }});
        buf.alpha = 0.5f;

        float x = 0.f, y = 0.f, z = 0.f;
        CHECK(buf.GetInterpolatedPos(1, x, y, z));
        CHECK_EQ(x, 10.f);
        CHECK_EQ(y, 20.f);
        CHECK_EQ(z, 30.f);
    });

    // Entity in both prev and curr: lerp(prev, curr, alpha).
    TEST_CASE(LinearBlend) {
        Neuron::Client::InterpBuffer buf;
        buf.prev  = MakeReplicaSet({{ 1u,  0.f, 0.f,  0.f, 0u }});
        buf.curr  = MakeReplicaSet({{ 1u, 10.f, 20.f, 30.f, 0u }});
        buf.alpha = 0.5f;

        float x = 0.f, y = 0.f, z = 0.f;
        CHECK(buf.GetInterpolatedPos(1, x, y, z));
        CHECK_EQ(x,  5.f);
        CHECK_EQ(y, 10.f);
        CHECK_EQ(z, 15.f);
    });

    // alpha = 0 → result equals prev.
    TEST_CASE(AlphaZeroReturnsPrev) {
        Neuron::Client::InterpBuffer buf;
        buf.prev  = MakeReplicaSet({{ 2u, 5.f,  6.f,  7.f, 1u }});
        buf.curr  = MakeReplicaSet({{ 2u, 50.f, 60.f, 70.f, 1u }});
        buf.alpha = 0.f;

        float x = 0.f, y = 0.f, z = 0.f;
        CHECK(buf.GetInterpolatedPos(2, x, y, z));
        CHECK_EQ(x, 5.f);
        CHECK_EQ(y, 6.f);
        CHECK_EQ(z, 7.f);
    });

    // alpha = 1 → result equals curr.
    TEST_CASE(AlphaOneReturnsCurr) {
        Neuron::Client::InterpBuffer buf;
        buf.prev  = MakeReplicaSet({{ 3u, 1.f,  2.f,  3.f,  0u }});
        buf.curr  = MakeReplicaSet({{ 3u, 10.f, 20.f, 30.f, 0u }});
        buf.alpha = 1.f;

        float x = 0.f, y = 0.f, z = 0.f;
        CHECK(buf.GetInterpolatedPos(3, x, y, z));
        CHECK_EQ(x, 10.f);
        CHECK_EQ(y, 20.f);
        CHECK_EQ(z, 30.f);
    });

    // Query for an entity absent from curr → returns false.
    TEST_CASE(MissingEntityReturnsFalse) {
        Neuron::Client::InterpBuffer buf;
        buf.curr  = MakeReplicaSet({{ 10u, 1.f, 2.f, 3.f, 0u }});
        buf.alpha = 0.5f;

        float x = 0.f, y = 0.f, z = 0.f;
        CHECK(!buf.GetInterpolatedPos(99, x, y, z));
    });

    // After Advance: old curr → prev, new snapshot → curr, alpha resets to 0.
    TEST_CASE(Advance) {
        Neuron::Client::InterpBuffer buf;
        buf.curr  = MakeReplicaSet({{ 1u, 0.f, 0.f, 0.f, 0u }});
        buf.alpha = 0.8f;

        const auto next = MakeReplicaSet({{ 1u, 100.f, 0.f, 0.f, 0u }});
        buf.Advance(next);

        CHECK_EQ(buf.alpha, 0.f);

        // Old curr should now be prev.
        const auto* prevEnt = buf.prev.FindById(1);
        CHECK(prevEnt != nullptr);
        if (prevEnt) CHECK_EQ(prevEnt->x, 0.f);

        // New snapshot should be curr.
        const auto* currEnt = buf.curr.FindById(1);
        CHECK(currEnt != nullptr);
        if (currEnt) CHECK_EQ(currEnt->x, 100.f);
    });

    // Two entities interpolate independently.
    TEST_CASE(MultipleEntities) {
        Neuron::Client::InterpBuffer buf;
        buf.prev  = MakeReplicaSet({{ 1u,   0.f, 0.f, 0.f, 0u },
                                     { 2u, 100.f, 0.f, 0.f, 1u }});
        buf.curr  = MakeReplicaSet({{ 1u,  10.f, 0.f, 0.f, 0u },
                                     { 2u, 200.f, 0.f, 0.f, 1u }});
        buf.alpha = 0.5f;

        float x = 0.f, y = 0.f, z = 0.f;
        CHECK(buf.GetInterpolatedPos(1, x, y, z));
        CHECK_EQ(x, 5.f);

        CHECK(buf.GetInterpolatedPos(2, x, y, z));
        CHECK_EQ(x, 150.f);
    });

    // ReplicaSet::Clear resets count to 0.
    TEST_CASE(ReplicaSetClear) {
        Neuron::Client::ReplicaSet rs = MakeReplicaSet({{ 1u, 0.f, 0.f, 0.f, 0u }});
        CHECK_EQ(rs.count, 1u);
        rs.Clear();
        CHECK_EQ(rs.count, 0u);
        CHECK(rs.FindById(1) == nullptr);
    });
}
