# NeuronRender — D3D12 renderer (static library).

# ── HLSL shaders → CompiledShaders/<Stem>.h (dxc, SM 6.7) ────────────────────
set(_shader_out_dir "${CMAKE_CURRENT_BINARY_DIR}/CompiledShaders")
neuron_compile_shaders(_shader_headers
  "${CMAKE_CURRENT_SOURCE_DIR}/Shaders" "${_shader_out_dir}"
  CanvasVS:vs_6_7  CanvasPS:ps_6_7
  SceneVS:vs_6_7   ScenePS:ps_6_7
  SceneTexVS:vs_6_7 SceneTexPS:ps_6_7
  FullscreenVS:vs_6_7
  BrightPassPS:ps_6_7  BlurPS:ps_6_7  CompositePS:ps_6_7
  ParticleVS:vs_6_7  ParticlePS:ps_6_7
)

add_library(NeuronRender STATIC
  CanvasRenderer.cpp
  CmoLoader.cpp
  DdsLoader.cpp
  DeviceResources.cpp
  NeuronRender.cpp
  ParticleRenderer.cpp
  PostProcess.cpp
  SceneRenderer.cpp
  ${_shader_headers}   # generated; forces dxc to run before the .cpp compile
)

target_include_directories(NeuronRender PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Sources include the generated headers as "CompiledShaders/<Stem>.h".
target_include_directories(NeuronRender PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# winrt::com_ptr / winrt::check_hresult come from the Windows SDK's header-only
# C++/WinRT base library (no package). vcvars usually puts this on INCLUDE, but
# add it explicitly so a bare Ninja configure also resolves <winrt/base.h>.
file(GLOB _cppwinrt_dirs "C:/Program Files (x86)/Windows Kits/10/Include/*/cppwinrt")
if(_cppwinrt_dirs)
  list(SORT _cppwinrt_dirs)
  list(GET _cppwinrt_dirs -1 _cppwinrt_dir)
  target_include_directories(NeuronRender PRIVATE "${_cppwinrt_dir}")
endif()

target_precompile_headers(NeuronRender PRIVATE pch.h)

# D3D12 + DXGI + GUIDs. PUBLIC so the eventual client exe links them automatically.
# runtimeobject: the renderer's COM helpers come from C++/WinRT's header-only
# <winrt/base.h>, whose error path references the WinRT C API (RoGetActivationFactory /
# RoOriginateLanguageException). UWP auto-linked it; a Win32 target must name it.
target_link_libraries(NeuronRender PUBLIC NeuronCore d3d12 dxgi dxguid runtimeobject)

# ── PIX (opt-in) — mirrors the old NEURON_USE_PIX NuGet guard ─────────────────
if(EARTHRISE_USE_PIX)
  # Pulled via the vcpkg 'pix' feature (see vcpkg.json / x64-windows-pix preset).
  find_package(unofficial-winpixeventruntime CONFIG QUIET)
  if(TARGET unofficial::winpixeventruntime::winpixeventruntime)
    target_link_libraries(NeuronRender PUBLIC unofficial::winpixeventruntime::winpixeventruntime)
    target_compile_definitions(NeuronRender PRIVATE NEURON_USE_PIX)
    message(STATUS "EarthRise: PIX markers enabled (NEURON_USE_PIX)")
  else()
    message(WARNING "EARTHRISE_USE_PIX=ON but winpixeventruntime not found via vcpkg; building without PIX.")
  endif()
endif()
