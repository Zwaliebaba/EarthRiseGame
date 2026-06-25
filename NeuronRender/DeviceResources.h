#pragma once
// DeviceResources — D3D12 device, DXGI swap chain, RTV/DSV descriptor heaps,
// per-frame command infrastructure, and CPU/GPU sync fence (§11).
//
// Usage pattern:
//   Initialize(hwnd, w, h)              ← called once the Win32 window exists
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
    static constexpr UINT FRAME_COUNT = 3; // triple-buffered (frames in flight = 3, §11.1)

    bool Initialize(HWND hwnd, UINT width, UINT height);
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

    // Measured GPU time for the last completed frame, in milliseconds (0 until
    // the first frame resolves, or if timestamp queries are unavailable).
    [[nodiscard]] double GpuFrameMs() const noexcept { return m_gpuMs; }

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

    std::array<winrt::com_ptr<ID3D12Resource>,          FRAME_COUNT> m_renderTargets;
    std::array<winrt::com_ptr<ID3D12CommandAllocator>,  FRAME_COUNT> m_cmdAllocators;
    std::array<UINT64,                                  FRAME_COUNT> m_fenceValues{};

    winrt::com_ptr<ID3D12GraphicsCommandList> m_cmdList;
    winrt::com_ptr<ID3D12Resource>            m_depthStencil;
    winrt::com_ptr<ID3D12Fence>               m_fence;

    HANDLE  m_fenceEvent{ nullptr };
    UINT64  m_nextFenceValue{ 1 };
    UINT    m_frameIndex{ 0 };
    UINT    m_width{ 1280 };
    UINT    m_height{ 720 };
    bool    m_vsync{ true };

    // GPU timestamp queries (perf gate): 2 timestamps (begin/end) per in-flight
    // frame, resolved into a READBACK buffer and read back one frame later.
    winrt::com_ptr<ID3D12QueryHeap> m_tsHeap;
    winrt::com_ptr<ID3D12Resource>  m_tsReadback;
    UINT64*                         m_tsMapped{ nullptr };
    UINT64                          m_tsFreq{ 0 };
    double                          m_gpuMs{ 0.0 };
    bool                            m_tsOk{ false };
};

} // namespace Neuron::Render
