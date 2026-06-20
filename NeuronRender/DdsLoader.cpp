// DdsLoader.cpp — DDS → DEFAULT-heap texture upload (D3D12).

#include "pch.h"
#include "DdsLoader.h"

#include "DdsParse.h"

#include <cstring>
#include <vector>

namespace Neuron::Render
{
  namespace
  {
    // Record-execute-wait on a throwaway DIRECT queue (load-time only). Mirrors
    // the one-shot upload idiom already used by SceneRenderer::Initialize.
    void executeAndWait(ID3D12Device* device, ID3D12GraphicsCommandList* cl,
                        ID3D12CommandQueue* queue)
    {
      winrt::check_hresult(cl->Close());
      ID3D12CommandList* lists[] = {cl};
      queue->ExecuteCommandLists(1, lists);

      winrt::com_ptr<ID3D12Fence> fence;
      winrt::check_hresult(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())));
      HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      winrt::check_hresult(queue->Signal(fence.get(), 1));
      winrt::check_hresult(fence->SetEventOnCompletion(1, ev));
      WaitForSingleObjectEx(ev, INFINITE, FALSE);
      CloseHandle(ev);
    }
  } // namespace

  TextureGpu DdsLoader::Load(ID3D12Device* device, std::span<const std::byte> dds)
  {
    using namespace er::format;

    DdsImage img{};
    if (parseDds(dds, img) != DdsStatus::Ok)
      return {};

    std::vector<DdsSubresource> subs;
    if (!enumerateDdsSubresources(img, dds.size(), subs) || subs.empty())
      return {};

    const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(static_cast<uint32_t>(img.format));

    // --- Destination texture (DEFAULT heap, COPY_DEST) ---
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = img.width;
    texDesc.Height = img.height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = static_cast<UINT16>(img.mipCount);
    texDesc.Format = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES hpDefault{};
    hpDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    winrt::com_ptr<ID3D12Resource> texture;
    winrt::check_hresult(device->CreateCommittedResource(&hpDefault, D3D12_HEAP_FLAG_NONE, &texDesc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(texture.put())));

    // --- Copyable footprints for the whole mip chain ---
    const UINT subCount = static_cast<UINT>(subs.size());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subCount);
    std::vector<UINT> numRows(subCount);
    std::vector<UINT64> rowSizes(subCount);
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&texDesc, 0, subCount, 0, layouts.data(), numRows.data(),
                                  rowSizes.data(), &uploadSize);

    // --- Upload (staging) buffer ---
    D3D12_HEAP_PROPERTIES hpUpload{};
    hpUpload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = uploadSize;
    bufDesc.Height = bufDesc.DepthOrArraySize = bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    winrt::com_ptr<ID3D12Resource> upload;
    winrt::check_hresult(device->CreateCommittedResource(&hpUpload, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(upload.put())));

    std::byte* mapped = nullptr;
    D3D12_RANGE noRead{};
    winrt::check_hresult(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
    for (UINT i = 0; i < subCount; ++i)
    {
      const std::byte* src = dds.data() + subs[i].offset;
      std::byte* dstBase = mapped + layouts[i].Offset;
      const size_t copyRowBytes = static_cast<size_t>(rowSizes[i]); // tight source pitch
      for (UINT row = 0; row < numRows[i]; ++row)
      {
        std::memcpy(dstBase + static_cast<size_t>(row) * layouts[i].Footprint.RowPitch,
                    src + static_cast<size_t>(row) * subs[i].rowPitch, copyRowBytes);
      }
    }
    upload->Unmap(0, nullptr);

    // --- Command list: copy each mip, then transition to SRV ---
    winrt::com_ptr<ID3D12CommandAllocator> alloc;
    winrt::check_hresult(
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.put())));
    winrt::com_ptr<ID3D12GraphicsCommandList> cl;
    winrt::check_hresult(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.get(),
                                                   nullptr, IID_PPV_ARGS(cl.put())));

    for (UINT i = 0; i < subCount; ++i)
    {
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = texture.get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = i;

      D3D12_TEXTURE_COPY_LOCATION srcLoc{};
      srcLoc.pResource = upload.get();
      srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      srcLoc.PlacedFootprint = layouts[i];

      cl->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &barrier);

    winrt::com_ptr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    winrt::check_hresult(device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.put())));
    executeAndWait(device, cl.get(), queue.get());

    TextureGpu out;
    out.resource = texture;
    out.format = format;
    out.width = img.width;
    out.height = img.height;
    out.mipCount = img.mipCount;
    return out;
  }
} // namespace Neuron::Render
