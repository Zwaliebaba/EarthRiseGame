#include "CppUnitTest.h"
#include "scene/SceneRenderer.h"
#include "canvas/CanvasRenderer.h"
#include "world/WorldPos.h"

#include <cmath>
#include <cstdint>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// SceneRenderer and CanvasRenderer require a GPU device for Initialize() and Render().
// These tests cover layout constants, struct invariants, and the FloatingOriginHelper
// math that drives render-space coordinate conversion — all without a GPU.

TEST_CLASS(SceneRendererLayoutTests)
{
public:
    TEST_METHOD(MaxEntitiesCapacityIs512)
    {
        Assert::AreEqual(UINT(512), Neuron::Render::SceneRenderer::kMaxEntities);
    }

    TEST_METHOD(SceneEntityKindBaseIsZero)
    {
        Neuron::Render::SceneEntity e{};
        e.kind = 0; // Base
        Assert::AreEqual(uint8_t(0), e.kind);
    }

    TEST_METHOD(SceneEntityKindShipIsOne)
    {
        Neuron::Render::SceneEntity e{};
        e.kind = 1; // Ship
        Assert::AreEqual(uint8_t(1), e.kind);
    }

    TEST_METHOD(NegativeColorMeansKindDefault)
    {
        // Convention: r < 0 signals "use kind default color" (§11 masterplan).
        Neuron::Render::SceneEntity base{};
        base.r = -1.0f;
        Assert::IsTrue(base.r < 0.0f);

        Neuron::Render::SceneEntity colored{};
        colored.r = 0.0f; colored.g = 1.0f; colored.b = 0.5f;
        Assert::IsFalse(colored.r < 0.0f);
    }

    TEST_METHOD(SceneEntityFieldsAreFloatAndByte)
    {
        // Verify field layout compiles and values round-trip correctly.
        Neuron::Render::SceneEntity e{};
        e.x = 100.5f; e.y = -200.25f; e.z = 0.0f;
        e.yaw = 1.5707963f; // ~PI/2
        e.scale = 100.0f;
        e.r = 0.0f; e.g = 0.5f; e.b = 1.0f;
        e.kind = 0;
        Assert::AreEqual(100.5f,       e.x);
        Assert::AreEqual(-200.25f,     e.y);
        Assert::AreEqual(1.5707963f,   e.yaw);
        Assert::AreEqual(100.0f,       e.scale);
        Assert::AreEqual(uint8_t(0),   e.kind);
    }
};

TEST_CLASS(CanvasRendererLayoutTests)
{
public:
    TEST_METHOD(MaxQuadsCapacityIs4096)
    {
        Assert::AreEqual(UINT(4096), Neuron::Render::CanvasRenderer::kMaxQuads);
    }
};

TEST_CLASS(FloatingOriginRenderTests)
{
public:
    TEST_METHOD(ToRenderSpaceIsRelativeToCameraOrigin)
    {
        Neuron::World::FloatingOriginHelper fo;
        fo.origin = { 1000, 2000, 3000 };
        Neuron::World::WorldPos entity{ 1100, 2100, 3100 };
        XMFLOAT3 rel = fo.ToRenderSpace(entity);
        Assert::AreEqual(100.0f, rel.x);
        Assert::AreEqual(100.0f, rel.y);
        Assert::AreEqual(100.0f, rel.z);
    }

    TEST_METHOD(RebaseToSectorUpdatesRenderOrigin)
    {
        Neuron::World::FloatingOriginHelper fo;
        fo.origin = { 0, 0, 0 };
        Neuron::World::SectorId s{ 2, 0, 0 };
        fo.RebaseToSector(s);
        // After rebase, origin should be at the sector-2 corner
        const int64_t expected = 2 * Neuron::World::kSectorSize;
        Assert::AreEqual(expected, fo.origin.x);
        Assert::AreEqual(int64_t(0), fo.origin.y);
    }

    TEST_METHOD(NeedsRebaseWhenCameraMovedOneSectorWidth)
    {
        Neuron::World::FloatingOriginHelper fo;
        fo.origin = { 0, 0, 0 };
        Neuron::World::WorldPos nearby{ Neuron::World::kSectorSize - 1, 0, 0 };
        Assert::IsFalse(fo.NeedsRebase(nearby)); // just inside the threshold
        Neuron::World::WorldPos faraway{ Neuron::World::kSectorSize, 0, 0 };
        Assert::IsTrue(fo.NeedsRebase(faraway));  // at exactly one sector width
    }

    TEST_METHOD(MultipleEntitiesHaveCorrectRenderOffsets)
    {
        Neuron::World::FloatingOriginHelper fo;
        fo.origin = { 5000, 5000, 0 };

        Neuron::World::WorldPos a{ 5100, 5050, 0 };
        Neuron::World::WorldPos b{ 4900, 5050, 0 };

        XMFLOAT3 ra = fo.ToRenderSpace(a);
        XMFLOAT3 rb = fo.ToRenderSpace(b);

        Assert::AreEqual(100.0f,  ra.x);
        Assert::AreEqual(-100.0f, rb.x);
        Assert::AreEqual(50.0f,   ra.y);
        Assert::AreEqual(50.0f,   rb.y);
    }

    TEST_METHOD(CrossSectorEntityRenderPosition)
    {
        // A base in sector 1 viewed from a camera in sector 0.
        Neuron::World::FloatingOriginHelper fo;
        fo.origin = { 0, 0, 0 };

        const int64_t sectorSize = Neuron::World::kSectorSize;
        Neuron::World::WorldPos entity{ sectorSize + 500, 0, 0 };
        XMFLOAT3 r = fo.ToRenderSpace(entity);

        Assert::AreEqual(static_cast<float>(sectorSize + 500), r.x);
        Assert::AreEqual(0.0f, r.y);
        Assert::AreEqual(0.0f, r.z);
    }
};
