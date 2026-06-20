#pragma once
// SceneRenderer — 3D scene pass: instanced unit-cube geometry for bases and ships.
//
// Each entity is drawn as a scaled/rotated/translated box:
//   Bases  — 100m cube, neon-blue emissive
//   Ships  — 20m cube, orange emissive
//
// World transform arrives via per-instance vertex-buffer stream 1 (§11).
// viewProj is set via root constants (no CBV heap descriptor needed).
//
// M2 replaces the cube with real CMO mesh assets and adds bloom.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>

#include "DeviceResources.h"

namespace Neuron::Render
{

// Public description of one renderable entity (sector-relative coordinates).
struct SceneEntity
{
    float   x, y, z;   // render-space position (floating-origin-relative metres)
    float   yaw;        // rotation around Y (radians)
    float   scale;      // half-extent in metres (100 for base, 20 for ship)
    float   r, g, b;   // emissive color override (< 0 = use kind default)
    uint8_t kind;       // 0 = Base, 1 = Ship, others use orange
};

class SceneRenderer
{
public:
    bool Initialize(DeviceResources* dr);
    void Uninitialize();

    // Record draw commands for 'count' entities into 'cl'.
    // viewProjT: 16 floats, column-major (i.e. XMMatrixTranspose of the camera matrix).
    // Called between DeviceResources::BeginFrame and EndFrame.
    void Render(ID3D12GraphicsCommandList* cl,
                const float viewProjT[16],
                const SceneEntity* entities, uint32_t count);

    static constexpr UINT kMaxEntities = 512;

private:
    void BuildUnitCube();

    // Per-instance data layout in stream 1 (80 bytes, matches SceneVS.hlsl input):
    //   float4 w0, w1, w2, w3  — world matrix rows (64 bytes)
    //   float4 color            — RGBA emissive   (16 bytes)
    struct InstanceData
    {
        float world[4][4]; // row-major; fills w0-w3
        float color[4];    // RGBA
    };
    static_assert(sizeof(InstanceData) == 80, "InstanceData size mismatch");

    struct Vertex { float px, py, pz, nx, ny, nz; };

    DeviceResources*                    m_dr{ nullptr };
    ID3D12Device*                       m_device{ nullptr };
    winrt::com_ptr<ID3D12RootSignature> m_rootSig;
    winrt::com_ptr<ID3D12PipelineState> m_pso;
    winrt::com_ptr<ID3D12Resource>      m_vb;
    winrt::com_ptr<ID3D12Resource>      m_ib;

    // One CPU-mapped instance buffer PER in-flight frame — writing a single
    // shared buffer races the GPU still reading the previous frame (flicker).
    std::array<winrt::com_ptr<ID3D12Resource>, DeviceResources::kFrameCount> m_instBuf;
    std::array<InstanceData*, DeviceResources::kFrameCount>                  m_instPtr{};
    std::array<D3D12_VERTEX_BUFFER_VIEW, DeviceResources::kFrameCount>       m_instView{};

    D3D12_VERTEX_BUFFER_VIEW m_vbView{};
    D3D12_INDEX_BUFFER_VIEW  m_ibView{};
    UINT m_indexCount{ 0 };
};

} // namespace Neuron::Render
