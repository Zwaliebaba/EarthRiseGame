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

#include "CmoParse.h" // skeleton + animation data (er::format::CmoBone / CmoAnimationClip)

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
    float boundingRadius = 0.0f; // model-space bounding-sphere radius (for size normalization)
    std::vector<SubmeshGpu> submeshes;
    std::vector<std::string> materialDiffuse; // diffuse .dds filename per material

    // Skeletal animation (empty for static meshes). The skinning vertex stream
    // and a skinned vertex shader are still TODO; this carries the CPU-side
    // skeleton + clips so a render path can sample poses (CmoAnimation.h) and
    // build a bone palette once that lands.
    std::vector<er::format::CmoBone> bones;
    std::vector<er::format::CmoAnimationClip> animations;

    bool valid() const noexcept { return vertexBuffer != nullptr && indexBuffer != nullptr; }
    bool skinned() const noexcept { return !bones.empty(); }
  };
} // namespace Neuron::Render
