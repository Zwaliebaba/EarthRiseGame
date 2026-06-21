#pragma once
// PixMarkers.h — PIX3 GPU event / marker helpers for NeuronRender (§11.1 profiling).
//
// Wraps the WinPixEventRuntime PIX3 API (PIXScopedEvent / PIXBeginEvent /
// PIXEndEvent / PIXSetMarker) so the render code can annotate the D3D12 command
// stream with named, colored regions. In PIX — and in the Visual Studio Graphics
// Analyzer / GPU captures — these surface as a labelled event hierarchy
// ("Frame" → "Scene" / "Canvas HUD" / individual uploads) instead of a flat list
// of API calls, which is what makes a capture readable and a frame's cost
// attributable to a pass.
//
// Opt-in, zero-cost when off:
//   * Define NEURON_USE_PIX to compile the real PIX3 calls and link
//     WinPixEventRuntime. The NeuronRender Debug|x64 build sets this automatically
//     **when the WinPixEventRuntime NuGet package is restored** (see
//     NeuronRender.vcxproj — the define is gated on the package existing, so a
//     fresh checkout without the package builds unchanged).
//   * When NEURON_USE_PIX is not defined the macros expand to nothing: no include,
//     no dependency, no runtime cost — retail builds stay clean.
//
// Always call the NEURON_PIX_* macros (never the raw PIX* names) so call sites
// compile in both modes. The ID3D12GraphicsCommandList overloads in pix3.h are only
// declared if <d3d12.h> was included first, so this header includes it.

#if defined(NEURON_USE_PIX)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>   // declares the ID3D12GraphicsCommandList PIX overloads in pix3.h
#ifndef USE_PIX
#define USE_PIX      // tells pix3.h to emit real events rather than no-op stubs
#endif
#include <pix3.h>

// Per-pass colors (PIX_COLOR(r,g,b)) — consistent hues make passes easy to pick out
// on the PIX timeline.
namespace Neuron::Render::PixColors
{
    inline constexpr UINT Frame   = PIX_COLOR(0x20, 0x20, 0x40);
    inline constexpr UINT Clear   = PIX_COLOR(0x40, 0x40, 0x60);
    inline constexpr UINT Scene   = PIX_COLOR(0x00, 0x80, 0xFF);
    inline constexpr UINT Canvas  = PIX_COLOR(0xFF, 0x80, 0x00);
    inline constexpr UINT Upload  = PIX_COLOR(0x80, 0xC0, 0x40);
    inline constexpr UINT Present = PIX_COLOR(0x80, 0x40, 0x80);
}

// Scoped (RAII) event — closes automatically at the end of the enclosing C++ scope.
#define NEURON_PIX_SCOPED(cl, color, ...)  PIXScopedEvent((cl), (color), __VA_ARGS__)
// Explicit begin/end pair — for a range that spans more than one C++ scope (a frame).
#define NEURON_PIX_BEGIN(cl, color, ...)   PIXBeginEvent((cl), (color), __VA_ARGS__)
#define NEURON_PIX_END(cl)                 PIXEndEvent((cl))
// Instantaneous marker (a labelled point on the timeline, no range).
#define NEURON_PIX_MARKER(cl, color, ...)  PIXSetMarker((cl), (color), __VA_ARGS__)

#else // !NEURON_USE_PIX — compile to nothing, no dependency on WinPixEventRuntime.

namespace Neuron::Render::PixColors
{
    inline constexpr unsigned int Frame = 0, Clear = 0, Scene = 0,
                                  Canvas = 0, Upload = 0, Present = 0;
}

#define NEURON_PIX_SCOPED(cl, color, ...)  ((void)0)
#define NEURON_PIX_BEGIN(cl, color, ...)   ((void)0)
#define NEURON_PIX_END(cl)                 ((void)0)
#define NEURON_PIX_MARKER(cl, color, ...)  ((void)0)

#endif // NEURON_USE_PIX
