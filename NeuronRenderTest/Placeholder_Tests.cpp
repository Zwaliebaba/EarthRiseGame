#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// NeuronRender unit tests — to be expanded in M2 (Darwinia look milestone).
// DeviceResources, SceneRenderer, and CanvasRenderer tests require a D3D12
// device, which will be exercised via WARP (software adapter) in CI.

TEST_CLASS(NeuronRenderPlaceholderTests)
{
public:
    TEST_METHOD(Placeholder)
    {
        Assert::IsTrue(true);
    }
};
