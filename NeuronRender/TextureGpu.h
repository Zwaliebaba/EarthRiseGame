#pragma once
// TextureGpu — a DDS texture resident on the GPU (DEFAULT heap, SRV-ready).
// Produced by DdsLoader (docs/design/neuronrender-architecture.md §9).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <dxgiformat.h>
#include <winrt/base.h>

#include <cstdint>

struct ID3D12Resource;

namespace Neuron::Render
{
  struct TextureGpu
  {
    winrt::com_ptr<ID3D12Resource> resource;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 0;

    bool valid() const noexcept { return resource != nullptr; }
  };
} // namespace Neuron::Render
