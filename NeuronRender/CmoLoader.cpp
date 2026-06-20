// CmoLoader.cpp — CMO → DEFAULT-heap vertex/index buffers (D3D12).

#include "pch.h"
#include "CmoLoader.h"

#include "CmoParse.h"

#include <cstring>

namespace Neuron::Render
{
  namespace
  {
    // Create a DEFAULT-heap buffer and stage `data` into it via an UPLOAD buffer,
    // recording the copy + state transition into `cl`. `uploadKeepAlive` must
    // outlive command-list execution.
    winrt::com_ptr<ID3D12Resource> createBuffer(ID3D12Device* device, ID3D12GraphicsCommandList* cl,
                                                const void* data, UINT64 size,
                                                D3D12_RESOURCE_STATES finalState,
                                                winrt::com_ptr<ID3D12Resource>& uploadKeepAlive)
    {
      D3D12_RESOURCE_DESC rd{};
      rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      rd.Width = size;
      rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
      rd.SampleDesc.Count = 1;
      rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

      D3D12_HEAP_PROPERTIES hpDefault{};
      hpDefault.Type = D3D12_HEAP_TYPE_DEFAULT;
      winrt::com_ptr<ID3D12Resource> buf;
      winrt::check_hresult(device->CreateCommittedResource(&hpDefault, D3D12_HEAP_FLAG_NONE, &rd,
                                                           D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                           IID_PPV_ARGS(buf.put())));

      D3D12_HEAP_PROPERTIES hpUpload{};
      hpUpload.Type = D3D12_HEAP_TYPE_UPLOAD;
      winrt::check_hresult(device->CreateCommittedResource(&hpUpload, D3D12_HEAP_FLAG_NONE, &rd,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                           IID_PPV_ARGS(uploadKeepAlive.put())));

      void* mapped = nullptr;
      D3D12_RANGE noRead{};
      winrt::check_hresult(uploadKeepAlive->Map(0, &noRead, &mapped));
      std::memcpy(mapped, data, size);
      uploadKeepAlive->Unmap(0, nullptr);

      D3D12_RESOURCE_BARRIER rb{};
      rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      rb.Transition.pResource = buf.get();
      rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      rb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
      rb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
      cl->ResourceBarrier(1, &rb);
      cl->CopyResource(buf.get(), uploadKeepAlive.get());
      rb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
      rb.Transition.StateAfter = finalState;
      cl->ResourceBarrier(1, &rb);
      return buf;
    }
  } // namespace

  MeshGpu CmoLoader::Load(ID3D12Device* device, std::span<const std::byte> cmo)
  {
    using namespace er::format;

    CmoModel model{};
    if (parseCmo(cmo, model) != CmoStatus::Ok || model.meshes.empty())
      return {};

    const CmoMeshInfo& mesh = model.meshes[0];
    if (mesh.vertexData.empty() || mesh.indexData.empty())
      return {};

    const std::span<const std::byte> vb = mesh.vertexData[0];
    const std::span<const std::byte> ib = mesh.indexData[0];
    if (vb.empty() || ib.empty())
      return {};

    winrt::com_ptr<ID3D12CommandAllocator> alloc;
    winrt::check_hresult(
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.put())));
    winrt::com_ptr<ID3D12GraphicsCommandList> cl;
    winrt::check_hresult(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.get(),
                                                   nullptr, IID_PPV_ARGS(cl.put())));

    MeshGpu out;
    winrt::com_ptr<ID3D12Resource> vbUpload, ibUpload;
    out.vertexBuffer = createBuffer(device, cl.get(), vb.data(), vb.size(),
                                    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, vbUpload);
    out.indexBuffer = createBuffer(device, cl.get(), ib.data(), ib.size(),
                                   D3D12_RESOURCE_STATE_INDEX_BUFFER, ibUpload);

    winrt::check_hresult(cl->Close());
    ID3D12CommandList* lists[] = {cl.get()};

    winrt::com_ptr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    winrt::check_hresult(device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.put())));
    queue->ExecuteCommandLists(1, lists);

    winrt::com_ptr<ID3D12Fence> fence;
    winrt::check_hresult(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    winrt::check_hresult(queue->Signal(fence.get(), 1));
    winrt::check_hresult(fence->SetEventOnCompletion(1, ev));
    WaitForSingleObjectEx(ev, INFINITE, FALSE);
    CloseHandle(ev);

    out.vertexCount = mesh.vertexCounts[0];
    out.indexCount = mesh.indexCounts[0];
    out.boundingRadius = mesh.boundingRadius;
    out.vbView = {out.vertexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(vb.size()),
                  static_cast<UINT>(CMO_VERTEX_SIZE)};
    out.ibView = {out.indexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(ib.size()),
                  DXGI_FORMAT_R16_UINT};

    // Submeshes referencing buffer 0 (triangle list ⇒ indexCount = primCount*3).
    for (const CmoSubmeshInfo& sm : mesh.submeshes)
    {
      if (sm.vertexBufferIndex == 0 && sm.indexBufferIndex == 0)
        out.submeshes.push_back({sm.primCount * 3, sm.startIndex, sm.materialIndex});
    }
    for (const CmoMaterialInfo& mat : mesh.materials)
      out.materialDiffuse.push_back(mat.diffuseTexture);

    return out;
  }
} // namespace Neuron::Render
