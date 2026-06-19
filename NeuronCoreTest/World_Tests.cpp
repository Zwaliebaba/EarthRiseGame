#include "CppUnitTest.h"
#include "world/WorldPos.h"

#include <cmath>
#include <cstdint>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

TEST_CLASS(WorldTests)
{
public:
    TEST_METHOD(SectorShift)
    {
        Assert::AreEqual(int64_t(16384), Neuron::World::kSectorSize);
    }

    TEST_METHOD(WorldToSector_positive)
    {
        Neuron::World::WorldPos p{ 16384, 0, 0 };
        auto s = Neuron::World::WorldToSector(p);
        Assert::AreEqual(int64_t(1), s.x);
        Assert::AreEqual(int64_t(0), s.y);
        Assert::AreEqual(int64_t(0), s.z);
    }

    TEST_METHOD(WorldToSector_origin)
    {
        Neuron::World::WorldPos p{ 0, 0, 0 };
        auto s = Neuron::World::WorldToSector(p);
        Assert::AreEqual(int64_t(0), s.x);
        Assert::AreEqual(int64_t(0), s.y);
        Assert::AreEqual(int64_t(0), s.z);
    }

    TEST_METHOD(WorldToSector_negative)
    {
        Neuron::World::WorldPos p{ -1, -1, -1 };
        auto s = Neuron::World::WorldToSector(p);
        Assert::AreEqual(int64_t(-1), s.x);
        Assert::AreEqual(int64_t(-1), s.y);
        Assert::AreEqual(int64_t(-1), s.z);
    }

    TEST_METHOD(SectorToOrigin_roundtrip)
    {
        Neuron::World::WorldPos p{ 32768, -16384, 49152 };
        auto s    = Neuron::World::WorldToSector(p);
        auto orig = Neuron::World::SectorToOrigin(s);
        Assert::IsTrue(orig.x <= p.x);
        Assert::IsTrue(orig.y <= p.y);
        Assert::IsTrue(orig.z <= p.z);
        Assert::IsTrue(p.x - orig.x < Neuron::World::kSectorSize);
    }

    TEST_METHOD(LocalOffset)
    {
        Neuron::World::WorldPos p{ 1000, 2000, 3000 };
        XMFLOAT3 local = Neuron::World::WorldToLocalOffset(p);
        Assert::AreEqual(1000.0f, local.x);
        Assert::AreEqual(2000.0f, local.y);
        Assert::AreEqual(3000.0f, local.z);
    }

    TEST_METHOD(AxisDelta)
    {
        Assert::AreEqual(int64_t(60),  Neuron::World::AxisDelta(100, 40));
        Assert::AreEqual(int64_t(-20), Neuron::World::AxisDelta(-10, 10));
    }

    TEST_METHOD(RelativeVec3)
    {
        Neuron::World::WorldPos a{ 0, 0, 0 };
        Neuron::World::WorldPos b{ 100, 200, 300 };
        XMFLOAT3 rel = Neuron::World::RelativeVec3(a, b);
        Assert::AreEqual(100.0f, rel.x);
        Assert::AreEqual(200.0f, rel.y);
        Assert::AreEqual(300.0f, rel.z);
    }

    TEST_METHOD(SectorHash_distinct)
    {
        Neuron::World::SectorHash hasher;
        Neuron::World::SectorId s1{ 0, 0, 0 };
        Neuron::World::SectorId s2{ 1, 0, 0 };
        Neuron::World::SectorId s3{ 0, 1, 0 };
        Assert::IsTrue(hasher(s1) != hasher(s2));
        Assert::IsTrue(hasher(s1) != hasher(s3));
        Assert::IsTrue(hasher(s2) != hasher(s3));
    }

    TEST_METHOD(FloatingOrigin_RenderSpace)
    {
        Neuron::World::FloatingOriginHelper fo;
        fo.origin = { 1000, 2000, 3000 };
        Neuron::World::WorldPos entity{ 1100, 2100, 3100 };
        XMFLOAT3 rel = fo.ToRenderSpace(entity);
        Assert::AreEqual(100.0f, rel.x);
        Assert::AreEqual(100.0f, rel.y);
        Assert::AreEqual(100.0f, rel.z);
    }

    TEST_METHOD(FloatingOrigin_NeedsRebase)
    {
        Neuron::World::FloatingOriginHelper fo;
        fo.origin = { 0, 0, 0 };
        Neuron::World::WorldPos near{ 1000, 0, 0 };
        Assert::IsFalse(fo.NeedsRebase(near));
        Neuron::World::WorldPos far{ Neuron::World::kSectorSize, 0, 0 };
        Assert::IsTrue(fo.NeedsRebase(far));
    }
};
