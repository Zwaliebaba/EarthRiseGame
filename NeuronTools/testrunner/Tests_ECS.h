#pragma once
// M0 unit tests — ECS (entity handle, create/destroy, component add/get/remove, ForEach).

#include "TestRunner.h"
#include "ecs/Ecs.h"

#include <cstdint>

namespace
{

// Sample components for testing.
struct Position { float x, y, z; };
struct Velocity { float dx, dy, dz; };
struct Tag      { uint8_t kind; };

} // anonymous

// Register component IDs (one definition per TU).
NEURON_DEFINE_COMPONENT(Position, 0);
NEURON_DEFINE_COMPONENT(Velocity, 1);
NEURON_DEFINE_COMPONENT(Tag,      2);

TEST_SUITE(ECS)
{
    TEST_CASE(NullHandle) {
        auto h = Neuron::ECS::EntityHandle::Null();
        CHECK(h.IsNull());
        CHECK_EQ(h.Index(), 0u);
        CHECK_EQ(h.Generation(), 0u);
    });

    TEST_CASE(MakeHandle) {
        auto h = Neuron::ECS::EntityHandle::Make(42, 7);
        CHECK_EQ(h.Index(), 42u);
        CHECK_EQ(h.Generation(), 7u);
        CHECK(!h.IsNull());
    });

    TEST_CASE(CreateAndDestroy) {
        Neuron::ECS::World w;
        w.RegisterComponent<Position>();

        auto e = w.CreateEntity();
        CHECK(!e.IsNull());
        CHECK(w.IsAlive(e));

        w.DestroyEntity(e);
        CHECK(!w.IsAlive(e));
    });

    TEST_CASE(StaleHandle) {
        // After destroy+recreate the old handle reports !IsAlive (generation mismatch).
        Neuron::ECS::World w;
        w.RegisterComponent<Position>();

        auto e1 = w.CreateEntity();
        w.DestroyEntity(e1);

        auto e2 = w.CreateEntity(); // may reuse same slot
        // e1 is now stale: its generation was incremented on destroy.
        CHECK(!w.IsAlive(e1));
        CHECK(w.IsAlive(e2));
    });

    TEST_CASE(AddGetComponent) {
        Neuron::ECS::World w;
        w.RegisterComponent<Position>();

        auto e = w.CreateEntity();
        auto& pos = w.AddComponent<Position>(e);
        pos = { 1.0f, 2.0f, 3.0f };

        auto* p = w.GetComponent<Position>(e);
        REQUIRE(p != nullptr);
        CHECK_EQ(p->x, 1.0f);
        CHECK_EQ(p->y, 2.0f);
        CHECK_EQ(p->z, 3.0f);
    });

    TEST_CASE(HasComponent) {
        Neuron::ECS::World w;
        w.RegisterComponent<Position>();
        w.RegisterComponent<Velocity>();

        auto e = w.CreateEntity();
        CHECK(!w.HasComponent<Position>(e));

        w.AddComponent<Position>(e);
        CHECK(w.HasComponent<Position>(e));
        CHECK(!w.HasComponent<Velocity>(e));
    });

    TEST_CASE(RemoveComponent) {
        Neuron::ECS::World w;
        w.RegisterComponent<Position>();

        auto e = w.CreateEntity();
        w.AddComponent<Position>(e).x = 5.0f;
        CHECK(w.HasComponent<Position>(e));

        w.RemoveComponent<Position>(e);
        CHECK(!w.HasComponent<Position>(e));
        CHECK(w.GetComponent<Position>(e) == nullptr);
    });

    TEST_CASE(ForEachDeterministicOrder) {
        // Create 3 entities, add Position+Velocity to 2, Tag only to 1.
        // ForEach<Position,Velocity> must visit only the 2 matching entities
        // in ascending index order.
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
            CHECK(p.x > lastX); // ascending order (e1.x=1 < e3.x=3)
            lastX = p.x;
            ++visited;
        });
        CHECK_EQ(visited, 2u);
    });

    TEST_CASE(EntityCount) {
        Neuron::ECS::World w;
        w.RegisterComponent<Position>();

        CHECK_EQ(w.EntityCount(), 0u);
        auto e1 = w.CreateEntity();
        auto e2 = w.CreateEntity();
        CHECK_EQ(w.EntityCount(), 2u);
        w.DestroyEntity(e1);
        CHECK_EQ(w.EntityCount(), 1u);
        (void)e2;
    });
}
