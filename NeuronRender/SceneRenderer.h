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
#include <unordered_map>

#include "DeviceResources.h"
#include "MeshGpu.h"
#include "TextureGpu.h"

namespace Neuron::Render
{

// Public description of one renderable entity (sector-relative coordinates).
struct SceneEntity
{
    float    x, y, z;   // render-space position (floating-origin-relative metres)
    float    yaw;        // rotation around Y (radians)
    float    scale;      // uniform world scale applied to the mesh
    float    r, g, b;   // emissive color override (< 0 = use kind default)
    uint8_t  kind;       // EntityKind (color default when no override)
    uint16_t shapeId;    // ShapeCatalog index → which registered mesh to draw
};

class SceneRenderer
{
public:
    // sceneColorFormat is the render-target format the scene draws into. With
    // post-processing it is the HDR buffer (R16G16B16A16_FLOAT); without it, the
    // LDR swap-chain format. Both PSOs are created against this format, so it
    // must match the bound render target at draw time.
    bool Initialize(DeviceResources* dr,
                    DXGI_FORMAT sceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT);
    void Uninitialize();

    // Record draw commands for 'count' entities into 'cl'.
    // viewProjT: 16 floats, column-major (i.e. XMMatrixTranspose of the camera matrix).
    // Called between DeviceResources::BeginFrame and EndFrame.
    void Render(ID3D12GraphicsCommandList* cl,
                const float viewProjT[16],
                const SceneEntity* entities, uint32_t count);

    // Camera-relative three-point lighting, uploaded to the pixel shaders as
    // root constants b1 (matches Lighting.hlsli's cbuffer layout). The caller
    // (App) recomputes this each frame from the camera basis.
    struct Lighting
    {
        float keyDir[3];   float keyIntensity;   // dir surface->key (world)
        float fillDir[3];  float fillIntensity;  // dir surface->fill (world)
        float viewDir[3];  float ambient;        // dir surface->camera (rim)
        float rimColor[3]; float rimPower;       // rim tint + Fresnel exponent
    };
    static_assert(sizeof(Lighting) == 16 * sizeof(float), "Lighting cbuffer layout");
    void SetLighting(const Lighting& l) { m_light = l; }

    // Register a catalog shape: the mesh drawn for entities whose SceneEntity::
    // shapeId == id, plus an optional diffuse texture (creates its SRV; when
    // present the textured pipeline is used for that shape). Invalid meshes are
    // ignored. Entities referencing an unregistered shape fall back to the
    // placeholder cube, so a missing asset never blanks the scene.
    void SetShape(uint16_t id, MeshGpu mesh, TextureGpu diffuse);

    static constexpr UINT kMaxEntities = 512;
    static constexpr UINT kMaxShapes   = 128; // SRV heap capacity (diffuse per shape)

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

    // Registered catalog shape: its GPU mesh, optional diffuse, and the SRV-heap
    // slot the diffuse was placed in (only meaningful when diffuse.valid()).
    struct Shape
    {
        MeshGpu    mesh;
        TextureGpu diffuse;
        UINT       srvIndex{ 0 };
    };
    std::unordered_map<uint16_t, Shape> m_shapes;

    // Draw one contiguous run of instances [startInstance, startInstance+count)
    // sharing shapeId 'sid', picking the textured/untextured pipeline and mesh.
    void DrawRun(ID3D12GraphicsCommandList* cl, const float viewProjT[16],
                 uint16_t sid, UINT startInstance, UINT count, UINT fi);

    // Textured pipeline (separate root sig/PSO so the untextured geometry path is
    // untouched). The SRV heap holds one diffuse per registered shape.
    winrt::com_ptr<ID3D12RootSignature> m_rootSigTex;
    winrt::com_ptr<ID3D12PipelineState> m_psoTex;
    winrt::com_ptr<ID3D12DescriptorHeap> m_srvHeap; // shader-visible, kMaxShapes SRVs
    UINT m_srvDescSize{ 0 };
    UINT m_nextSrv{ 0 };

    // Current frame's lighting (b1). Default is a sane forward-key rig so the
    // scene is lit even if SetLighting is never called.
    Lighting m_light{
        { 0.4f, 0.6f, -0.7f }, 0.9f,   // key:  up-right, toward camera
        { -0.5f, 0.1f, -0.3f }, 0.35f, // fill: opposite side, near level
        { 0.0f, 0.0f, -1.0f }, 0.16f,  // view dir (rim), ambient floor
        { 0.35f, 0.45f, 0.65f }, 3.0f, // cool rim tint, Fresnel power
    };
};

} // namespace Neuron::Render
