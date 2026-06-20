#pragma once
// MeshGpu — a CMO mesh resident on the GPU (DEFAULT-heap vertex/index buffers).
// Produced by CmoLoader (docs/design/neuronrender-architecture.md §9).
//
// The vertex stride is the full CMO vertex (CMO_VERTEX_SIZE = 52 bytes:
// pos/normal/tangent/color/uv). The Scene input layout reads only the fields it
// needs (position @0, normal @12) from that stride, so the same instanced draw
// path used for the placeholder cube works unchanged with real meshes.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>
#include <winrt/base.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Neuron::Render
{
  struct SubmeshGpu
  {
    uint32_t indexCount = 0;     // = primCount * 3 (triangle list)
    uint32_t startIndex = 0;
    uint32_t materialIndex = 0;
  };

  struct MeshGpu
  {
    winrt::com_ptr<ID3D12Resource> vertexBuffer;
    winrt::com_ptr<ID3D12Resource> indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vbView{};
    D3D12_INDEX_BUFFER_VIEW ibView{}; // DXGI_FORMAT_R16_UINT
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    std::vector<SubmeshGpu> submeshes;
    std::vector<std::string> materialDiffuse; // diffuse .dds filename per material

    bool valid() const noexcept { return vertexBuffer != nullptr && indexBuffer != nullptr; }
  };
} // namespace Neuron::Render
