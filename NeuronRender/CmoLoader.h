#pragma once
// CmoLoader — parse a CMO blob and upload its first mesh to DEFAULT-heap
// vertex/index buffers (§12.3).
//
// Wraps the platform-independent er::format::parseCmo core (CmoParse.h); the
// only Windows-specific part is the D3D12 upload (self-contained: temporary
// command list + queue + wait). M2 scope: static meshes, vertex/index buffer 0
// of mesh 0 (the common single-buffer case); submeshes + per-material diffuse
// texture names are surfaced for the material binding pass.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>

#include <cstddef>
#include <span>

#include "MeshGpu.h"

namespace Neuron::Render
{
  class CmoLoader
  {
  public:
    // Returns an empty MeshGpu (valid()==false) on parse failure or if the mesh
    // has no vertex/index buffer 0.
    static MeshGpu Load(ID3D12Device* device, std::span<const std::byte> cmo);
  };
} // namespace Neuron::Render
