#include "CppUnitTest.h"
#include "ecs/Ecs.h"

#include <cstdint>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Named namespace required in DLL context to avoid internal-linkage ODR issues.
namespace NeuronCoreTestComponents {
    struct Position { float x, y, z; };
    struct Velocity { float dx, dy, dz; };
    struct Tag      { uint8_t kind; };
}

// Bind component IDs — use slots 10-12 to avoid collision with Sim slots 0-5.
NEURON_DEFINE_COMPONENT(NeuronCoreTestComponents::Position, 10);
NEURON_DEFINE_COMPONENT(NeuronCoreTestComponents::Velocity, 11);
NEURON_DEFINE_COMPONENT(NeuronCoreTestComponents::Tag,      12);

using namespace NeuronCoreTestComponents;

TEST_CLASS(ECSTests)
{
public:
    TEST_METHOD(NullHandle)
    {
        auto h = Neuron::ECS::EntityHandle::Null();
        Assert::IsTrue(h.IsNull());
        Assert::AreEqual(0u, h.Index());
        Assert::AreEqual(0u, h.Generation());
    }

    TEST_METHOD(MakeHandle)
    {
        auto h = Neuron::ECS::EntityHandle::Make(42, 7);
        Assert::AreEqual(42u, h.Index());
        Assert::AreEqual(7u, h.Generation());
        Assert::IsFalse(h.IsNull());
    }

    TEST_METHOD(CreateAndDestroy)
    {
        Neuron::ECS::World w;
        w.RegisterComponent<Position>();

        auto e = w.CreateEntity();
        Assert::IsFalse(e.IsNull());
        Assert::IsTrue(w.IsAlive(e));

        w.DestroyEntity(e);
        Assert::IsFalse(w.IsAlive(e));
    }

    TEST_METHOD(StaleHandle)
    {
        Neuron::ECS::World w;
        w.RegisterComponent<Position>();

        auto e1 = w.CreateEntity();
        w.DestroyEntity(e1);

        auto e2 = w.CreateEntity();
        Assert::IsFalse(w.IsAlive(e1));
        Assert::IsTrue(w.IsAlive(e2));
    }

    TEST_METHOD(AddGetComponent)
    {
        Neuron::ECS::World w;
        w.RegisterComponent<Position>();

        auto e = w.CreateEntity();
        auto& pos = w.AddComponent<Position>(e);
        pos = { 1.0f, 2.0f, 3.0f };

        auto* p = w.GetComponent<Position>(e);
        Assert::IsNotNull(p);
        Assert::AreEqual(1.0f, p->x);
        Assert::AreEqual(2.0f, p->y);
        Assert::AreEqual(3.0f, p->z);
    }

    TEST_METHOD(HasComponent)
    {
        Neuron::ECS::World w;
        w.RegisterComponent<Position>();
        w.RegisterComponent<Velocity>();

        auto e = w.CreateEntity();
        Assert::IsFalse(w.HasComponent<Position>(e));

        w.AddComponent<Position>(e);
        Assert::IsTrue(w.HasComponent<Position>(e));
        Assert::IsFalse(w.HasComponent<Velocity>(e));
    }

    TEST_METHOD(RemoveComponent)
    {
        Neuron::ECS::World w;
        w.RegisterComponent<Position>();

        auto e = w.CreateEntity();
        w.AddComponent<Position>(e).x = 5.0f;
        Assert::IsTrue(w.HasComponent<Position>(e));

        w.RemoveComponent<Position>(e);
        Assert::IsFalse(w.HasComponent<Position>(e));
        Assert::IsNull(w.GetComponent<Position>(e));
    }

    TEST_METHOD(ForEachDeterministicOrder)
    {
        Neuron::ECS::World w;
        w.RegisterComponent<Position>();
        w.RegisterComponent<Velocity>();
        w.RegisterComponent<Tag>();

        auto e1 = w.CreateEntity();
        auto e2 = w.CreateEntity();
        auto e3 = w.CreateEntity();

        w.AddComponent<Position>(e1).x = 1.0f;
        w.AddComponent<Velocity>(e1).dx = 10.0f;

        w.AddComponent<Tag>(e2).kind = 42;

        w.AddComponent<Position>(e3).x = 3.0f;
        w.AddComponent<Velocity>(e3).dx = 30.0f;

        uint32_t visited = 0;
        float    lastX   = -1.0f;
        w.ForEach<Position, Velocity>([&](Position& p, Velocity& v) {
            Assert::IsTrue(p.x > lastX);
            lastX = p.x;
            ++visited;
        });
        Assert::AreEqual(2u, visited);
    }

    TEST_METHOD(EntityCount)
    {
        Neuron::ECS::World w;
        w.RegisterComponent<Position>();

        Assert::AreEqual(0u, w.EntityCount());
        auto e1 = w.CreateEntity();
        auto e2 = w.CreateEntity();
        Assert::AreEqual(2u, w.EntityCount());
        w.DestroyEntity(e1);
        Assert::AreEqual(1u, w.EntityCount());
        (void)e2;
    }
};
