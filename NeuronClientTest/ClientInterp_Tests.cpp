#include "CppUnitTest.h"
#include "interp/Interpolator.h"
#include "replica/Replica.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ClientInterpTestHelpers {

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

} // namespace ClientInterpTestHelpers

using namespace ClientInterpTestHelpers;

TEST_CLASS(ClientInterpTests)
{
public:
    TEST_METHOD(SnapOnFirstAppearance)
    {
        Neuron::Client::InterpBuffer buf;
        buf.curr  = MakeReplicaSet({{ 1u, 10.f, 20.f, 30.f, 0u }});
        buf.alpha = 0.5f;

        float x = 0.f, y = 0.f, z = 0.f;
        Assert::IsTrue(buf.GetInterpolatedPos(1, x, y, z));
        Assert::AreEqual(10.f, x);
        Assert::AreEqual(20.f, y);
        Assert::AreEqual(30.f, z);
    }

    TEST_METHOD(LinearBlend)
    {
        Neuron::Client::InterpBuffer buf;
        buf.prev  = MakeReplicaSet({{ 1u,  0.f,  0.f,  0.f, 0u }});
        buf.curr  = MakeReplicaSet({{ 1u, 10.f, 20.f, 30.f, 0u }});
        buf.alpha = 0.5f;

        float x = 0.f, y = 0.f, z = 0.f;
        Assert::IsTrue(buf.GetInterpolatedPos(1, x, y, z));
        Assert::AreEqual( 5.f, x);
        Assert::AreEqual(10.f, y);
        Assert::AreEqual(15.f, z);
    }

    TEST_METHOD(AlphaZeroReturnsPrev)
    {
        Neuron::Client::InterpBuffer buf;
        buf.prev  = MakeReplicaSet({{ 2u,  5.f,  6.f,  7.f, 1u }});
        buf.curr  = MakeReplicaSet({{ 2u, 50.f, 60.f, 70.f, 1u }});
        buf.alpha = 0.f;

        float x = 0.f, y = 0.f, z = 0.f;
        Assert::IsTrue(buf.GetInterpolatedPos(2, x, y, z));
        Assert::AreEqual(5.f, x);
        Assert::AreEqual(6.f, y);
        Assert::AreEqual(7.f, z);
    }

    TEST_METHOD(AlphaOneReturnsCurr)
    {
        Neuron::Client::InterpBuffer buf;
        buf.prev  = MakeReplicaSet({{ 3u,  1.f,  2.f,  3.f, 0u }});
        buf.curr  = MakeReplicaSet({{ 3u, 10.f, 20.f, 30.f, 0u }});
        buf.alpha = 1.f;

        float x = 0.f, y = 0.f, z = 0.f;
        Assert::IsTrue(buf.GetInterpolatedPos(3, x, y, z));
        Assert::AreEqual(10.f, x);
        Assert::AreEqual(20.f, y);
        Assert::AreEqual(30.f, z);
    }

    TEST_METHOD(MissingEntityReturnsFalse)
    {
        Neuron::Client::InterpBuffer buf;
        buf.curr  = MakeReplicaSet({{ 10u, 1.f, 2.f, 3.f, 0u }});
        buf.alpha = 0.5f;

        float x = 0.f, y = 0.f, z = 0.f;
        Assert::IsFalse(buf.GetInterpolatedPos(99, x, y, z));
    }

    TEST_METHOD(Advance)
    {
        Neuron::Client::InterpBuffer buf;
        buf.curr  = MakeReplicaSet({{ 1u, 0.f, 0.f, 0.f, 0u }});
        buf.alpha = 0.8f;

        const auto next = MakeReplicaSet({{ 1u, 100.f, 0.f, 0.f, 0u }});
        buf.Advance(next);

        Assert::AreEqual(0.f, buf.alpha);

        const auto* prevEnt = buf.prev.FindById(1);
        Assert::IsNotNull(prevEnt);
        if (prevEnt) Assert::AreEqual(0.f, prevEnt->x);

        const auto* currEnt = buf.curr.FindById(1);
        Assert::IsNotNull(currEnt);
        if (currEnt) Assert::AreEqual(100.f, currEnt->x);
    }

    TEST_METHOD(MultipleEntities)
    {
        Neuron::Client::InterpBuffer buf;
        buf.prev  = MakeReplicaSet({{ 1u,   0.f, 0.f, 0.f, 0u },
                                     { 2u, 100.f, 0.f, 0.f, 1u }});
        buf.curr  = MakeReplicaSet({{ 1u,  10.f, 0.f, 0.f, 0u },
                                     { 2u, 200.f, 0.f, 0.f, 1u }});
        buf.alpha = 0.5f;

        float x = 0.f, y = 0.f, z = 0.f;
        Assert::IsTrue(buf.GetInterpolatedPos(1, x, y, z));
        Assert::AreEqual(5.f, x);

        Assert::IsTrue(buf.GetInterpolatedPos(2, x, y, z));
        Assert::AreEqual(150.f, x);
    }

    TEST_METHOD(ReplicaSetClear)
    {
        Neuron::Client::ReplicaSet rs = MakeReplicaSet({{ 1u, 0.f, 0.f, 0.f, 0u }});
        Assert::AreEqual(1u, rs.count);
        rs.Clear();
        Assert::AreEqual(0u, rs.count);
        Assert::IsNull(rs.FindById(1));
    }
};
