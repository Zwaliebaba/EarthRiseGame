// Token-indexed connection routing tests (masterplan §9; M4 area G). Datagrams
// route by their 64-bit connectionToken into a generation-tagged slot table — no
// per-datagram string hash — and a recycled slot with a stale generation is
// rejected (the ECS-handle pattern). Pure; mirrored on the Linux runner (§16.2).
// The IOCP per-connection lane dispatch is the Win32 ERServer side.

#include "ConnectionTable.h"
#include "TestRunner.h"

using namespace ertest;
using Neuron::Net::ConnectionTable;
using Neuron::Net::ConnHandle;

ER_TEST(ConnectionTable, RoutesByTokenToTheRightSlot)
{
    ConnectionTable t;
    const ConnHandle a = t.Open(0xAAAA'0000'1111'2222ull);
    const ConnHandle b = t.Open(0xBBBB'3333'4444'5555ull);
    ER_CHECK(a.valid && b.valid);
    ER_CHECK(a.index != b.index);
    ER_CHECK_EQ(t.ActiveCount(), size_t{ 2 });

    // A datagram carrying a token routes to its slot; an unknown token does not.
    const ConnHandle found = t.Find(0xBBBB'3333'4444'5555ull);
    ER_CHECK(found.valid);
    ER_CHECK_EQ(found.index, b.index);
    ER_CHECK(!t.Find(0xDEAD'BEEFull).valid);
}

ER_TEST(ConnectionTable, OpeningSameTokenIsIdempotent)
{
    ConnectionTable t;
    const ConnHandle a1 = t.Open(0x1234ull);
    const ConnHandle a2 = t.Open(0x1234ull);
    ER_CHECK_EQ(a1.index, a2.index);
    ER_CHECK_EQ(t.ActiveCount(), size_t{ 1 });
}

ER_TEST(ConnectionTable, RecycledSlotRejectsStaleGeneration)
{
    ConnectionTable t;
    const ConnHandle a = t.Open(0xA11ull);
    ER_CHECK(t.Validate(a));

    t.Close(0xA11ull);
    ER_CHECK(!t.Validate(a));       // closed → the old handle is stale
    ER_CHECK(!t.Find(0xA11ull).valid);

    // A new connection reuses the freed slot with a bumped generation.
    const ConnHandle b = t.Open(0xB22ull);
    ER_CHECK_EQ(b.index, a.index);  // same physical slot recycled
    ER_CHECK(t.Validate(b));
    ER_CHECK(!t.Validate(a));       // a's generation no longer matches → rejected
    ER_CHECK(a.generation != b.generation);
}

ER_TEST(ConnectionTable, StateAccessorFollowsValidity)
{
    ConnectionTable t;
    const ConnHandle a = t.Open(0xC0FFEEull);
    ER_CHECK(t.Get(a) != nullptr);
    ER_CHECK_EQ(t.Get(a)->token, uint64_t{ 0xC0FFEEull });
    t.Close(0xC0FFEEull);
    ER_CHECK(t.Get(a) == nullptr); // stale handle yields no state
}

ER_TEST(ConnectionTable, ConnectionIsPinnedToOneLane)
{
    ConnectionTable t;
    const ConnHandle a = t.Open(1);
    const ConnHandle b = t.Open(2);
    // A connection's lane is stable and derived from its slot (per-conn affinity).
    ER_CHECK_EQ(ConnectionTable::Lane(a, 4), a.index % 4);
    ER_CHECK_EQ(ConnectionTable::Lane(a, 4), ConnectionTable::Lane(t.Find(1), 4));
    ER_CHECK_EQ(ConnectionTable::Lane(b, 1), uint32_t{ 0 }); // single lane → everything on 0
}

ER_TEST(ConnectionTable, FreedSlotsAreReusedNotGrown)
{
    ConnectionTable t;
    for (uint64_t i = 1; i <= 8; ++i) t.Open(i);
    ER_CHECK_EQ(t.SlotCapacity(), size_t{ 8 });
    for (uint64_t i = 1; i <= 8; ++i) t.Close(i);
    for (uint64_t i = 100; i < 104; ++i) t.Open(i);
    ER_CHECK_EQ(t.SlotCapacity(), size_t{ 8 }); // recycled the freed slots, no growth
    ER_CHECK_EQ(t.ActiveCount(), size_t{ 4 });
}
