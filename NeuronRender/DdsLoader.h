#pragma once
// DdsLoader — parse a DDS blob and upload it to a DEFAULT-heap texture (§12.1).
//
// Wraps the platform-independent er::format::parseDds + enumerateDdsSubresources
// cores (DdsParse.h); the only Windows-specific part is the D3D12 upload. The
// load is self-contained (creates a temporary command list + queue and waits),
// so the returned TextureGpu is immediately SRV-ready. Supports BC1–BC7 and
// 32-bit BGRA/RGBA with full mip chains.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>

#include <cstddef>
#include <span>

#include "TextureGpu.h"

namespace Neuron::Render
{
  class DdsLoader
  {
  public:
    // Returns an empty TextureGpu (valid()==false) on parse/format failure or a
    // truncated payload.
    static TextureGpu Load(ID3D12Device* device, std::span<const std::byte> dds);
  };
} // namespace Neuron::Render
