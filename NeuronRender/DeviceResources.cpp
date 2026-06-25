// DeviceResources.cpp — D3D12 device + swap chain implementation.

#include "pch.h"
#include "DeviceResources.h"
#include "PixMarkers.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace Neuron::Render
{

bool DeviceResources::Initialize(HWND hwnd, UINT width, UINT height)
{
    m_width  = width;
    m_height = height;

    // -----------------------------------------------------------------------
    // Debug layer (debug builds only)
    // -----------------------------------------------------------------------
#ifdef _DEBUG
    winrt::com_ptr<ID3D12Debug> debugCtrl;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debugCtrl.put()))))
        debugCtrl->EnableDebugLayer();
#endif

    // -----------------------------------------------------------------------
    // DXGI factory
    // -----------------------------------------------------------------------
    UINT dxgiFlags = 0;
#ifdef _DEBUG
    dxgiFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
    winrt::com_ptr<IDXGIFactory4> factory;
    winrt::check_hresult(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(factory.put())));

    // -----------------------------------------------------------------------
    // Hardware adapter → D3D12 device — require DirectX 12.1 (FL 12_1) per §11.1.
    // (TODO §11.1: also CheckFeatureSupport for Resource Binding Tier 2, SM 6.7 and
    //  Root Signature 1.1 once the renderer relies on them.)
    // -----------------------------------------------------------------------
    winrt::com_ptr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter = nullptr; continue; }
        if (SUCCEEDED(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_1,
                                        IID_PPV_ARGS(m_device.put()))))
            break;
        adapter = nullptr;
    }
    if (!m_device) {
        // WARP fallback — supports FL 12_1 (Win10 1709+); slower than hardware.
        winrt::com_ptr<IDXGIAdapter> warp;
        winrt::check_hresult(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.put())));
        winrt::check_hresult(D3D12CreateDevice(warp.get(), D3D_FEATURE_LEVEL_12_1,
                                                IID_PPV_ARGS(m_device.put())));
    }

    // -----------------------------------------------------------------------
    // Direct command queue
    // -----------------------------------------------------------------------
    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    winrt::check_hresult(m_device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(m_cmdQueue.put())));

    // -----------------------------------------------------------------------
    // GPU timestamp queries (perf gate). Best-effort: on failure m_tsOk stays
    // false and GpuFrameMs() just reports 0.
    // -----------------------------------------------------------------------
    if (SUCCEEDED(m_cmdQueue->GetTimestampFrequency(&m_tsFreq)) && m_tsFreq != 0)
    {
        D3D12_QUERY_HEAP_DESC qhd{};
        qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qhd.Count = FRAME_COUNT * 2; // begin + end per in-flight frame
        if (SUCCEEDED(m_device->CreateQueryHeap(&qhd, IID_PPV_ARGS(m_tsHeap.put()))))
        {
            D3D12_HEAP_PROPERTIES hpReadback{};
            hpReadback.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rb{};
            rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rb.Width = static_cast<UINT64>(FRAME_COUNT) * 2 * sizeof(UINT64);
            rb.Height = rb.DepthOrArraySize = rb.MipLevels = 1;
            rb.SampleDesc.Count = 1;
            rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(m_device->CreateCommittedResource(
                    &hpReadback, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(m_tsReadback.put()))) &&
                SUCCEEDED(m_tsReadback->Map(0, nullptr, reinterpret_cast<void**>(&m_tsMapped))))
            {
                for (UINT i = 0; i < FRAME_COUNT * 2; ++i) m_tsMapped[i] = 0; // avoid first-frame garbage
                m_tsOk = true;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Swap chain for HWND (Win32 flip-discard)
    // -----------------------------------------------------------------------
    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width       = width;
    scDesc.Height      = height;
    scDesc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.SampleDesc  = { 1, 0 };
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = FRAME_COUNT;
    scDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    scDesc.Scaling     = DXGI_SCALING_NONE;

    winrt::com_ptr<IDXGISwapChain1> sc1;
    winrt::check_hresult(factory->CreateSwapChainForHwnd(
        m_cmdQueue.get(), hwnd, &scDesc, nullptr, nullptr, sc1.put()));
    // We present windowed/borderless and handle resizing ourselves; stop DXGI from
    // grabbing Alt+Enter for its (deprecated) exclusive-fullscreen transition.
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    m_swapChain = sc1.as<IDXGISwapChain3>();

    // -----------------------------------------------------------------------
    // Descriptor heaps
    // -----------------------------------------------------------------------
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = FRAME_COUNT;
    rtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    winrt::check_hresult(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(m_rtvHeap.put())));
    m_rtvDescSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.NumDescriptors = 1;
    dsvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    winrt::check_hresult(m_device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(m_dsvHeap.put())));

    // -----------------------------------------------------------------------
    // Per-frame command allocators + single command list
    // -----------------------------------------------------------------------
    for (UINT i = 0; i < FRAME_COUNT; ++i)
        winrt::check_hresult(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(m_cmdAllocators[i].put())));

    winrt::check_hresult(m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_cmdAllocators[0].get(), nullptr, IID_PPV_ARGS(m_cmdList.put())));
    winrt::check_hresult(m_cmdList->Close()); // start closed

    // -----------------------------------------------------------------------
    // Frame fence
    // -----------------------------------------------------------------------
    winrt::check_hresult(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                IID_PPV_ARGS(m_fence.put())));
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) winrt::throw_last_error();

    // -----------------------------------------------------------------------
    // Render targets + depth buffer
    // -----------------------------------------------------------------------
    CreateRenderTargetViews();
    CreateDepthStencil();

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    return true;
}

void DeviceResources::CreateRenderTargetViews()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FRAME_COUNT; ++i) {
        winrt::check_hresult(m_swapChain->GetBuffer(i, IID_PPV_ARGS(m_renderTargets[i].put())));
        m_device->CreateRenderTargetView(m_renderTargets[i].get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescSize;
        m_fenceValues[i] = 0;
    }
}

void DeviceResources::CreateDepthStencil()
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width              = m_width;
    rd.Height             = m_height;
    rd.DepthOrArraySize   = 1;
    rd.MipLevels          = 1;
    rd.Format             = DXGI_FORMAT_D32_FLOAT;
    rd.SampleDesc.Count   = 1;
    rd.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv{};
    cv.Format               = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth   = 1.0f;

    winrt::check_hresult(m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
        IID_PPV_ARGS(m_depthStencil.put())));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_device->CreateDepthStencilView(
        m_depthStencil.get(), &dsvDesc,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void DeviceResources::BeginFrame()
{
    // Wait until the GPU is done with this frame's previous resources.
    const UINT64 completedValue = m_fence->GetCompletedValue();
    if (completedValue < m_fenceValues[m_frameIndex]) {
        winrt::check_hresult(m_fence->SetEventOnCompletion(
            m_fenceValues[m_frameIndex], m_fenceEvent));
        WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    }

    winrt::check_hresult(m_cmdAllocators[m_frameIndex]->Reset());
    winrt::check_hresult(m_cmdList->Reset(m_cmdAllocators[m_frameIndex].get(), nullptr));

    // GPU timing: this slot's previous frame is now complete (fence waited
    // above), so its resolved timestamps are readable. Then open this frame's
    // begin timestamp.
    if (m_tsOk)
    {
        const UINT base = m_frameIndex * 2;
        const UINT64 t0 = m_tsMapped[base], t1 = m_tsMapped[base + 1];
        if (t1 > t0) m_gpuMs = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(m_tsFreq);
        m_cmdList->EndQuery(m_tsHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, base);
    }

    // Open the frame-wide PIX event; the matching End is in EndFrame so the whole
    // recorded command list (clear, scene, canvas, present) nests under one "Frame".
    NEURON_PIX_BEGIN(m_cmdList.get(), PixColors::Frame, "Frame %u", m_frameIndex);
    NEURON_PIX_BEGIN(m_cmdList.get(), PixColors::Clear, "Clear + Setup");

    // Transition back buffer: PRESENT → RENDER_TARGET
    D3D12_RESOURCE_BARRIER rb{};
    rb.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource   = m_renderTargets[m_frameIndex].get();
    rb.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    rb.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &rb);

    // Clear render target and depth.
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = CurrentRtv();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    constexpr float SKY_COLOR[] = { 0.018f, 0.014f, 0.013f, 1.0f }; // near-black warm void
    m_cmdList->ClearRenderTargetView(rtv, SKY_COLOR, 0, nullptr);
    m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    D3D12_VIEWPORT vp{ 0.f, 0.f, static_cast<float>(m_width), static_cast<float>(m_height), 0.f, 1.f };
    D3D12_RECT     sr{ 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    m_cmdList->RSSetViewports(1, &vp);
    m_cmdList->RSSetScissorRects(1, &sr);

    NEURON_PIX_END(m_cmdList.get()); // "Clear + Setup"
}

void DeviceResources::EndFrame()
{
    NEURON_PIX_BEGIN(m_cmdList.get(), PixColors::Present, "Present transition");

    // Transition back buffer: RENDER_TARGET → PRESENT
    D3D12_RESOURCE_BARRIER rb{};
    rb.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource   = m_renderTargets[m_frameIndex].get();
    rb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rb.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &rb);

    NEURON_PIX_END(m_cmdList.get()); // "Present transition"
    NEURON_PIX_END(m_cmdList.get()); // "Frame" (opened in BeginFrame)

    // GPU timing: end timestamp + resolve both into this slot's readback range.
    if (m_tsOk)
    {
        const UINT base = m_frameIndex * 2;
        m_cmdList->EndQuery(m_tsHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, base + 1);
        m_cmdList->ResolveQueryData(m_tsHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, base, 2,
                                    m_tsReadback.get(), static_cast<UINT64>(base) * sizeof(UINT64));
    }

    winrt::check_hresult(m_cmdList->Close());

    ID3D12CommandList* lists[] = { m_cmdList.get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);

    winrt::check_hresult(m_swapChain->Present(m_vsync ? 1u : 0u, 0));

    // Signal fence for this frame and advance to next back buffer.
    m_fenceValues[m_frameIndex] = m_nextFenceValue;
    winrt::check_hresult(m_cmdQueue->Signal(m_fence.get(), m_nextFenceValue++));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void DeviceResources::Resize(UINT width, UINT height)
{
    if (width == m_width && height == m_height) return;
    m_width = width; m_height = height;
    WaitForGpu();
    for (auto& rt : m_renderTargets) rt = nullptr;
    m_depthStencil = nullptr;
    winrt::check_hresult(m_swapChain->ResizeBuffers(
        FRAME_COUNT, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    CreateRenderTargetViews();
    CreateDepthStencil();
}

void DeviceResources::Uninitialize()
{
    WaitForGpu();
    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
}

void DeviceResources::WaitForGpu()
{
    winrt::check_hresult(m_cmdQueue->Signal(m_fence.get(), m_nextFenceValue));
    winrt::check_hresult(m_fence->SetEventOnCompletion(m_nextFenceValue, m_fenceEvent));
    WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    m_fenceValues[m_frameIndex] = m_nextFenceValue++;
}

D3D12_CPU_DESCRIPTOR_HANDLE DeviceResources::CurrentRtv() const noexcept
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescSize;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE DeviceResources::DsvHandle() const noexcept
{
    return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
}

} // namespace Neuron::Render
