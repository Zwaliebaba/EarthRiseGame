#pragma once
// PostProcess — HDR scene buffer + bloom + composite into the LDR back buffer
// (masterplan M2 area C: the Darwinia "everything glows" look).
//
// Flow per frame (driven by App):
//   BeginScene(cl) — bind the full-res HDR target (+ shared depth), clear it.
//   [SceneRenderer records its draws into the HDR target]
//   Resolve(cl)    — bright-pass + downsample -> half-res, separable Gaussian
//                    blur (H then V, ping-pong), then composite HDR + bloom into
//                    the swap-chain back buffer (additive glow).
//   [CanvasRenderer draws the HUD over the composited back buffer]
//
// All targets are R16G16B16A16_FLOAT (HDR) so emissive/over-bright pixels carry
// energy >1 into the bloom threshold. Self-contained: owns its RTV/SRV heaps,
// PSOs and a static linear-clamp sampler. Initialize() returns false on any
// failure so the caller can fall back to rendering straight to the back buffer.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>

namespace Neuron::Render
{
  class DeviceResources;

  class PostProcess
  {
  public:
    // HDR format the scene must render into (pass to SceneRenderer::Initialize).
    static constexpr DXGI_FORMAT HDR_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // Creates size-dependent targets for the current back-buffer size. Returns
    // false on failure (caller renders without post-processing).
    bool Initialize(DeviceResources* dr);
    void Uninitialize();

    // Recreate the size-dependent targets after a swap-chain resize.
    void Resize();

    // Bind the HDR target (+ shared depth) and clear it. Call right after
    // DeviceResources::BeginFrame and before SceneRenderer::Render.
    void BeginScene(ID3D12GraphicsCommandList* cl);

    // Run the bloom chain and composite into the back buffer. Leaves the back
    // buffer (+ depth) bound so the HUD can draw over it. Call after the scene.
    void Resolve(ID3D12GraphicsCommandList* cl);

    // Settings (area G). Bloom add strength (0 = off); pixel effect toggles the
    // composite vignette + scanlines.
    void SetBloomIntensity(float i) noexcept { m_bloomIntensity = i; }
    void SetPixelEffect(bool on) noexcept { m_pixelEffect = on; }

    [[nodiscard]] bool ready() const noexcept { return m_ready; }

  private:
    struct Target
    {
      winrt::com_ptr<ID3D12Resource> res;
      D3D12_RESOURCE_STATES          state{ D3D12_RESOURCE_STATE_COMMON };
      D3D12_CPU_DESCRIPTOR_HANDLE    rtv{};   // in m_rtvHeap
      UINT                           srvIndex{ 0 }; // slot in m_srvHeap
    };

    bool CreateTargets();
    // Render a full-screen pass into 'dst' (transitioned to RENDER_TARGET here),
    // sampling srcSrvIndex as t0 and bloomSrvIndex as t1 (the second is a dummy
    // for single-texture passes). The two SRVs are bound via independent
    // single-descriptor tables, so neither can alias the current render target.
    void DrawFullscreen(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso,
                        Target& dst, UINT srcSrvIndex, UINT bloomSrvIndex,
                        const float params[4], UINT vpW, UINT vpH);
    void Barrier(ID3D12GraphicsCommandList* cl, Target& t, D3D12_RESOURCE_STATES to);
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(UINT index) const;

    DeviceResources* m_dr{ nullptr };
    ID3D12Device*    m_device{ nullptr };
    bool             m_ready{ false };

    winrt::com_ptr<ID3D12RootSignature> m_rootSig;
    winrt::com_ptr<ID3D12PipelineState> m_psoBright;
    winrt::com_ptr<ID3D12PipelineState> m_psoBlur;
    winrt::com_ptr<ID3D12PipelineState> m_psoComposite;

    winrt::com_ptr<ID3D12DescriptorHeap> m_rtvHeap; // 3 RTVs (hdr, bloomA, bloomB)
    winrt::com_ptr<ID3D12DescriptorHeap> m_srvHeap; // 3 SRVs, shader-visible
    UINT m_rtvSize{ 0 };
    UINT m_srvSize{ 0 };

    // Index layout in m_srvHeap chosen so the composite reads HDR(0)+bloomA(1)
    // as a contiguous t0/t1 table: 0=HDR, 1=bloomA, 2=bloomB.
    Target m_hdr;    // srvIndex 0
    Target m_bloomA; // srvIndex 1
    Target m_bloomB; // srvIndex 2

    UINT m_width{ 0 };
    UINT m_height{ 0 };
    UINT m_halfW{ 0 };
    UINT m_halfH{ 0 };

    float m_bloomIntensity{ 1.10f }; // settings: composite bloom add strength
    bool  m_pixelEffect{ true };     // settings: vignette + scanlines
  };
} // namespace Neuron::Render
