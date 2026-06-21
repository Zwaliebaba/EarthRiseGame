#pragma once
// DeviceResources — D3D12 device, DXGI swap chain, RTV/DSV descriptor heaps,
// per-frame command infrastructure, and CPU/GPU sync fence (§11).
//
// Usage pattern:
//   Initialize(coreWindow, w, h)        ← called from IFrameworkView::SetWindow
//   foreach frame:
//     BeginFrame()                       ← resets allocator, clears, sets RT
//     [renderers record into CmdList()]
//     EndFrame()                         ← executes, presents, advances frame
//   Uninitialize()                       ← drains GPU, releases resources

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>

namespace Neuron::Render
{

class DeviceResources
{
public:
    static constexpr UINT kFrameCount = 2; // double-buffered

    bool Initialize(IUnknown* coreWindow, UINT width, UINT height);
    void Resize(UINT width, UINT height);
    void Uninitialize();

    // Frame lifecycle — called by App::Run each iteration.
    void BeginFrame(); // wait fence, reset allocator, clear RT/DS, set viewport
    void EndFrame();   // execute cmdlist, present, signal fence, advance index

    // Accessors for renderers.
    [[nodiscard]] ID3D12Device*              Device()  const noexcept { return m_device.get(); }
    [[nodiscard]] ID3D12GraphicsCommandList* CmdList() const noexcept { return m_cmdList.get(); }

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE DsvHandle() const noexcept;

    // RTV of the back buffer for the in-progress frame (valid between
    // BeginFrame and EndFrame). Used by the post-process composite to write the
    // tone-mapped result into the swap-chain buffer.
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle() const noexcept { return CurrentRtv(); }

    [[nodiscard]] UINT Width()  const noexcept { return m_width; }
    [[nodiscard]] UINT Height() const noexcept { return m_height; }

    // Index of the back buffer / per-frame resource slot for the in-progress
    // frame (valid between BeginFrame and EndFrame). Renderers key per-frame
    // upload buffers off this so the CPU never overwrites data the GPU may still
    // be reading from an in-flight frame.
    [[nodiscard]] UINT FrameIndex() const noexcept { return m_frameIndex; }

    // VSync toggle (settings). On = present synced to vblank; off = uncapped.
    void SetVSync(bool on) noexcept { m_vsync = on; }

private:
    void CreateRenderTargetViews();
    void CreateDepthStencil();
    void WaitForGpu();

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE CurrentRtv() const noexcept;

    winrt::com_ptr<ID3D12Device>           m_device;
    winrt::com_ptr<IDXGISwapChain3>        m_swapChain;
    winrt::com_ptr<ID3D12CommandQueue>     m_cmdQueue;

    winrt::com_ptr<ID3D12DescriptorHeap>   m_rtvHeap;
    winrt::com_ptr<ID3D12DescriptorHeap>   m_dsvHeap;
    UINT                                   m_rtvDescSize{ 0 };

    std::array<winrt::com_ptr<ID3D12Resource>,          kFrameCount> m_renderTargets;
    std::array<winrt::com_ptr<ID3D12CommandAllocator>,  kFrameCount> m_cmdAllocators;
    std::array<UINT64,                                  kFrameCount> m_fenceValues{};

    winrt::com_ptr<ID3D12GraphicsCommandList> m_cmdList;
    winrt::com_ptr<ID3D12Resource>            m_depthStencil;
    winrt::com_ptr<ID3D12Fence>               m_fence;

    HANDLE  m_fenceEvent{ nullptr };
    UINT64  m_nextFenceValue{ 1 };
    UINT    m_frameIndex{ 0 };
    UINT    m_width{ 1280 };
    UINT    m_height{ 720 };
    bool    m_vsync{ true };
};

} // namespace Neuron::Render
