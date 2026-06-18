#pragma once
// M0 unit tests — World coordinates (WorldPos, SectorId, sector math, RelativeVec3).

#include "TestRunner.h"
#include "world/WorldPos.h"

#include <cmath>
#include <cstdint>

TEST_SUITE(World)
{
    TEST_CASE(SectorShift) {
        // kSectorSize = 2^14 = 16384
        CHECK_EQ(Neuron::World::kSectorSize, int64_t(16384));
    });

    TEST_CASE(WorldToSector_positive) {
        // Position at exact sector boundary
        Neuron::World::WorldPos p{ 16384, 0, 0 };
        auto s = Neuron::World::WorldToSector(p);
        CHECK_EQ(s.x, int64_t(1));
        CHECK_EQ(s.y, int64_t(0));
        CHECK_EQ(s.z, int64_t(0));
    });

    TEST_CASE(WorldToSector_origin) {
        Neuron::World::WorldPos p{ 0, 0, 0 };
        auto s = Neuron::World::WorldToSector(p);
        CHECK_EQ(s.x, int64_t(0));
        CHECK_EQ(s.y, int64_t(0));
        CHECK_EQ(s.z, int64_t(0));
    });

    TEST_CASE(WorldToSector_negative) {
        // -1 is in sector -1 (arithmetic right-shift)
        Neuron::World::WorldPos p{ -1, -1, -1 };
        auto s = Neuron::World::WorldToSector(p);
        CHECK_EQ(s.x, int64_t(-1));
        CHECK_EQ(s.y, int64_t(-1));
        CHECK_EQ(s.z, int64_t(-1));
    });

    TEST_CASE(SectorToOrigin_roundtrip) {
        Neuron::World::WorldPos p{ 32768, -16384, 49152 };
        auto s     = Neuron::World::WorldToSector(p);
        auto orig  = Neuron::World::SectorToOrigin(s);
        // Origin should be <= p and within kSectorSize
        CHECK_LE(orig.x, p.x);
        CHECK_LE(orig.y, p.y);
        CHECK_LE(orig.z, p.z);
        CHECK_LT(p.x - orig.x, Neuron::World::kSectorSize);
    });

    TEST_CASE(LocalOffset) {
        // Position at (1000, 2000, 3000) → sector 0,0,0; local offset = (1000,2000,3000)
        Neuron::World::WorldPos p{ 1000, 2000, 3000 };
        XMFLOAT3 local = Neuron::World::WorldToLocalOffset(p);
        CHECK_EQ(local.x, 1000.0f);
        CHECK_EQ(local.y, 2000.0f);
        CHECK_EQ(local.z, 3000.0f);
    });

    TEST_CASE(AxisDelta) {
        CHECK_EQ(Neuron::World::AxisDelta(100, 40),  int64_t(60));
        CHECK_EQ(Neuron::World::AxisDelta(-10, 10),  int64_t(-20));
    });

    TEST_CASE(RelativeVec3) {
        Neuron::World::WorldPos a{ 0, 0, 0 };
        Neuron::World::WorldPos b{ 100, 200, 300 };
        XMFLOAT3 rel = Neuron::World::RelativeVec3(a, b);
        CHECK_EQ(rel.x, 100.0f);
        CHECK_EQ(rel.y, 200.0f);
        CHECK_EQ(rel.z, 300.0f);
    });

    TEST_CASE(SectorHash_distinct) {
        Neuron::World::SectorHash hasher;
        Neuron::World::SectorId s1{ 0, 0, 0 };
        Neuron::World::SectorId s2{ 1, 0, 0 };
        Neuron::World::SectorId s3{ 0, 1, 0 };
        // Hash values must differ (not a strict requirement, but they should for these trivial cases)
        CHECK(hasher(s1) != hasher(s2));
        CHECK(hasher(s1) != hasher(s3));
        CHECK(hasher(s2) != hasher(s3));
    });

    TEST_CASE(FloatingOrigin_RenderSpace) {
        Neuron::World::FloatingOriginHelper fo;
        fo.origin = { 1000, 2000, 3000 };

        Neuron::World::WorldPos entity{ 1100, 2100, 3100 };
        XMFLOAT3 rel = fo.ToRenderSpace(entity);
        CHECK_EQ(rel.x, 100.0f);
        CHECK_EQ(rel.y, 100.0f);
        CHECK_EQ(rel.z, 100.0f);
    });

    TEST_CASE(FloatingOrigin_NeedsRebase) {
        Neuron::World::FloatingOriginHelper fo;
        fo.origin = { 0, 0, 0 };

        // Within one sector — no rebase needed
        Neuron::World::WorldPos near{ 1000, 0, 0 };
        CHECK(!fo.NeedsRebase(near));

        // One full sector away — rebase needed
        Neuron::World::WorldPos far{ Neuron::World::kSectorSize, 0, 0 };
        CHECK(fo.NeedsRebase(far));
    });
}
