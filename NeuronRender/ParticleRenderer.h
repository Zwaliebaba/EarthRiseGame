#pragma once
// ParticleRenderer — additive billboard particles into the HDR scene target
// (masterplan §11.2 area D). M2 increment: a CPU-simulated drifting "space dust"
// field that follows the camera focus, rendered as camera-facing additive quads
// sampling Particle.dds so the motes glow through the bloom pass.
//
// (The masterplan's end state is GPU-compute simulation; this CPU sim + GPU
// instanced billboards is the low-risk first step with the same look.)
//
// Per frame, inside the HDR scene pass (after SceneRenderer, before Resolve):
//   particles.Update(dt, focusX, focusY, focusZ);
//   particles.Render(cl, viewProjT, right, up);

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <vector>

#include "DeviceResources.h"
#include "TextureGpu.h"

namespace Neuron::Render
{
  class ParticleRenderer
  {
  public:
    // sceneColorFormat must match the bound render target (HDR with bloom, else
    // the swap-chain format).
    bool Initialize(DeviceResources* dr,
                    DXGI_FORMAT sceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT);
    void Uninitialize();

    // Glow sprite (Particle.dds). Without it Render is a no-op (fail-safe).
    void SetTexture(TextureGpu tex);

    // Advance the drifting field; it wraps within a box centred on the focus.
    void Update(float dt, float focusX, float focusY, float focusZ);

    // Record the additive billboard draw. viewProjT: 16 floats (un-transposed
    // view*proj, as SceneVS expects). right/up: camera basis (world space).
    void Render(ID3D12GraphicsCommandList* cl, const float viewProjT[16],
                float rightX, float rightY, float rightZ,
                float upX, float upY, float upZ);

    static constexpr UINT kMaxParticles = 1500;

  private:
    struct Particle
    {
      float ox, oy, oz; // offset from focus (drifts; wrapped into the field box)
      float vx, vy, vz; // velocity (m/s)
      float size;       // half-extent (m)
      float r, g, b, a; // additive colour (premultiplied feel via a)
    };
    struct Vertex { float x, y, z, u, v, r, g, b, a; };
    static_assert(sizeof(Vertex) == 36, "Particle vertex size");

    DeviceResources*                    m_dr{ nullptr };
    ID3D12Device*                       m_device{ nullptr };
    winrt::com_ptr<ID3D12RootSignature> m_rootSig;
    winrt::com_ptr<ID3D12PipelineState> m_pso;
    winrt::com_ptr<ID3D12DescriptorHeap> m_srvHeap; // shader-visible, 1 SRV
    TextureGpu                          m_tex;

    // One CPU-mapped vertex buffer per in-flight frame (avoid GPU races).
    std::array<winrt::com_ptr<ID3D12Resource>, DeviceResources::kFrameCount> m_vb;
    std::array<Vertex*, DeviceResources::kFrameCount>                        m_vbPtr{};

    std::vector<Particle> m_particles;
    float m_fx{ 0.f }, m_fy{ 0.f }, m_fz{ 0.f }; // field centre (camera focus)

    static constexpr float kFieldHalf = 600.f; // field box half-extent (m)
  };
} // namespace Neuron::Render
