# EarthRise — NeuronRender Architecture (DX12)

> Companion to `masterplan.md` §11 (rendering), §11.1 (DX12 engine), §11.2 (camera/VFX),
> §12.4 (shaders). **Status:** DRAFT v0.1 — architecture + class-layout sketch for
> review. **MS-only:** Direct3D 12, DXGI, DirectXMath; embedded DXIL shaders; no
> third-party libraries. C++23, MSVC, **client/UWP only** (ERHeadless links none).
>
> **Targets (decided, §11.1):** **D3D12 FL 12_0, Resource Binding Tier 2, SM6.0, Root
> Signature 1.1**; **triple-buffered** flip-model; **HDR forward + bloom**; no MSAA;
> GPU-compute particles. Broad reach (runs on integrated GPUs).

---

## 1. Layout & dependencies

```
NeuronRender/                [static lib, client/UWP only — files flat; names below are VS Filters, not dirs]
├── gfx/      DeviceResources, SwapChain, FrameContext, CommandQueue, Fence,
│             DescriptorHeaps, PsoCache, RootSignatures, UploadRing, ResourceUploader,
│             GpuMemory, Barriers, GpuTimer, DebugLayer/DRED
├── scene/    SceneRenderer (3D), Camera, FloatingOrigin, InstanceBuffer, MeshGpu,
│             TextureGpu, passes (Forward, BrightPass, Bloom, Particles, ToneMap)
├── canvas/   CanvasRenderer (2D), QuadBatch, TextBatch (monospace), LineBatch
└── assets/   DdsLoader, CmoLoader, FontAtlas   (parsers; cooked data from NeuronTools)

depends on:  NeuronCore (math, UniversePos/sector, handles, time) + Windows SDK (d3d12,
             dxgi, d3dcompiler-free — DXIL is embedded), C++/WinRT (CoreWindow swapchain)
consumes:    CompiledShaders/*.h (embedded DXIL, §12.4); cooked .dds/.cmo/font (§12)
used by:     EarthRise.Client (UWP)          NOT linked by: ERHeadless / NeuronClient
tested by:   NeuronRenderTest (device init/teardown, batch logic, camera math)
```

**Namespace:** `er::gfx`. **Error model:** `winrt::check_hresult` at boundaries; hot
path is `noexcept` and allocation-free (per-frame data from the upload ring).

---

## 2. Device & frame model (`gfx/`)

```cpp
namespace er::gfx {

inline constexpr UINT kFrameCount = 3;          // triple-buffered (§11.1)

class DeviceResources {                          // owns device + queues + swapchain
public:
    HRESULT Initialize(winrt::Windows::UI::Core::CoreWindow const& window);
    void    Shutdown() noexcept;

    // --- frame lifecycle (called by SceneRenderer/Client each frame) ---
    FrameContext& BeginFrame();                  // waits frame fence, resets allocator
    void          EndFrame(FrameContext&);       // execute lists, Present, signal fence
    void          WaitForGpu() noexcept;         // full flush (resize/suspend/shutdown)

    // --- UWP lifecycle ---
    void OnResize(UINT w, UINT h);
    void OnSuspend() noexcept;                    // WaitForGpu + IDXGIDevice3::Trim()
    void OnResume()  noexcept;
    void OnDeviceRemoved();                       // DRED dump -> recreate device + resources

    ID3D12Device*       Device()  const noexcept;
    ID3D12CommandQueue* Direct()  const noexcept; // graphics+compute (FL 12_0)
    ID3D12CommandQueue* Copy()    const noexcept; // async uploads
    DescriptorHeaps&    Heaps()   noexcept;
    GpuMemory&          Memory()  noexcept;
};

struct FrameContext {                             // one per in-flight frame
    ID3D12CommandAllocator*    allocator;
    ID3D12GraphicsCommandList* list;
    UINT64                     fenceValue;
    UploadRing                 upload;            // per-frame dynamic constants/instances
    DescriptorRing             viewRing;          // per-frame shader-visible descriptors
    UINT                       backBufferIndex;
};

} // namespace er::gfx
```

- **Adapter selection:** DXGI factory enumerates adapters → pick highest-perf hardware
  adapter meeting **FL 12_0** (`CheckFeatureSupport`: binding tier ≥2, SM6, RS 1.1);
  **WARP** fallback (dev/CI). Dev builds enable the **debug layer + GPU-based validation**
  and **DRED** (auto-breadcrumbs + page-fault) for device-removed diagnostics.
- **Swap chain:** `CreateSwapChainForCoreWindow`, **`FLIP_DISCARD`**, `kFrameCount`
  buffers, LDR format (`R8G8B8A8_UNORM` / `R10G10B10A2`); optional **waitable** object
  for frame pacing; tearing flag where allowed.
- **Sync:** one fence; each frame signals `fenceValue`; `BeginFrame` waits until the
  GPU passed the value the slot used last → safe to reuse its allocator + rings.

---

## 3. Descriptor & binding model (`gfx/`)

```cpp
class DescriptorHeaps {
public:
    // Persistent (non-shader-visible) staging heaps -> copied to the per-frame ring.
    DescriptorHandle AllocCbvSrvUav();           // persistent SRVs (textures, instance bufs)
    DescriptorHandle AllocRtv();
    DescriptorHandle AllocDsv();
    // Per-frame shader-visible ring (one large heap, sub-allocated per FrameContext).
    GpuDescriptorRange CopyToFrameRing(FrameContext&, std::span<const DescriptorHandle>);
    ID3D12DescriptorHeap* ShaderVisibleHeap() const noexcept; // bound once per frame
    ID3D12DescriptorHeap* SamplerHeap()       const noexcept; // tiny static set
};
```

- **One large shader-visible CBV/SRV/UAV heap**, **ring-sub-allocated per frame**;
  persistent descriptors live in a non-shader-visible staging heap and are
  `CopyDescriptors`-ed into the ring each frame. Non-shader-visible **RTV/DSV** heaps.
- **Binding (Tier 2, not full bindless):** root signatures authored **in HLSL**
  (§12.4), RS 1.1. Layout:
  - **root constants** — small per-draw ids/flags,
  - **root CBV** — per-frame/per-view constants (view-proj, camera, time),
  - **descriptor table** — material textures (diffuse/normal/…),
  - **per-instance data** via a **structured buffer (SRV)** indexed by `SV_InstanceID`.
- **Static samplers** declared in the root signature (no dynamic sampler churn).
- Two shared root signatures: **`Scene`** and **`Canvas`**.

---

## 4. Resources & memory (`gfx/`)

```cpp
class ResourceUploader {                          // COPY-queue staged uploads
public:
    MeshGpu    UploadMesh(const CmoMesh&);        // DEFAULT heap + fenced copy
    TextureGpu UploadTexture(const DdsImage&);    // BC1-BC7 + mips -> DEFAULT
    void       Flush();                           // submit + signal copy fence
};

class UploadRing {                                // persistently-mapped UPLOAD buffer
public:
    template<class T> Alloc<T> Push(const T&);    // returns GPU VA for root CBV / SRV
    void Reset() noexcept;                         // per frame
};

class GpuMemory {                                 // residency / budget
public:
    void   Poll();                                // IDXGIAdapter3::QueryVideoMemoryInfo
    bool   OverBudget() const noexcept;           // -> drop mip tails / evict
};
```

- **DEFAULT** heap for static GPU data (meshes, textures), staged from **UPLOAD** on the
  **COPY** queue with fence sync; **UPLOAD ring** for dynamic per-frame constants &
  instance data.
- **Manual barriers** (`Barriers` helper batches transitions: RT↔SRV, depth, UAV).
- **GPU memory budget** polled via `QueryVideoMemoryInfo`; residency-aware texture
  streaming as sectors/fleets stream in (ties to interest, §8.4).

---

## 5. PSO management (`gfx/`)

```cpp
class PsoCache {
public:
    ID3D12PipelineState* Get(const PsoKey&);      // hash -> build-on-miss from DXIL
    void Warm(std::span<const PsoKey>);           // precompile at load (hide hitching)
private:
    // Optional ID3D12PipelineLibrary disk cache keyed by adapter+driver+build.
};
```

- PSOs built at load from **embedded DXIL** (`#include "CompiledShaders/*.h"`, §12.4),
  **hash-cached**; optional `ID3D12PipelineLibrary` on-disk cache to skip recompiles.
- **No runtime HLSL** (UWP rule, §12.4 / R9).

---

## 6. SceneRenderer (3D) & pass graph (`scene/`)

```cpp
class SceneRenderer {
public:
    HRESULT Initialize(DeviceResources&);
    void    SetCamera(const Camera&) noexcept;             // also -> floating origin & audio listener
    void    Submit(std::span<const RenderInstance>) noexcept; // camera-relative float3 (§6.4)
    void    Render(FrameContext&) noexcept;                // runs the pass graph below
};
```

**HDR forward pass graph** (per `Render`, fits the Darwinia look):

| # | Pass | Output | Notes |
| --- | --- | --- | --- |
| 1 | **Forward** | HDR `R16G16B16A16_FLOAT` (+ depth `D32_FLOAT`) | instanced emissive ships; no MSAA |
| 2 | **Bright-pass** | half-res HDR | extract > threshold |
| 3 | **Bloom** | ping-pong blur chain | separable Gaussian, additive |
| 4 | **Particles** | into HDR | GPU-compute additive billboards (§7) |
| 5 | **Tone-map + post** | LDR backbuffer | tonemap + optional scanline/vignette/grain |
| 6 | **Canvas (2D UI)** | backbuffer | §8, post-tonemap, no bloom |
| 7 | **Present** | — | flip |

- **Instancing:** one draw per mesh/material, `RenderInstance[]` in a structured buffer
  (per-instance transform/color/emissive), indexed by `SV_InstanceID`.
- **Floating origin (§6.4):** the camera is the render origin; instances arrive as
  **camera-relative `float3`** — **no `int64` reaches the GPU** (R2). `FloatingOrigin`
  rebases when the camera travels far; the same transform feeds the **X3DAudio listener**
  (§11.3).
- **Culling/LOD:** frustum cull on submit; distance LOD + the interest set (§8.4) bound
  instance counts; particle budget scales with distance (App. B / R16).

```cpp
struct Camera {                                   // §11.2 space-RTS camera
    DirectX::XMFLOAT3 eyeRel, forward, up;        // camera-relative
    float fovY, aspect, nearZ, farZ;              // RH perspective
    DirectX::XMMATRIX View() const, Proj() const; // *RH
};
```

---

## 7. VFX / particles (`scene/passes/Particles`)
- **GPU-driven:** particle pool in a structured buffer (UAV); **spawn + simulate in
  compute shaders** (FL 12_0); draw as **additive instanced billboards** into HDR.
- CPU emits **spawn requests** only (ring of `ParticleSpawn`); GPU owns simulation.
- Categories: thrusters, weapon tracers/muzzle, impacts, **explosions**, **warp/jump**,
  mining beams. **Per-frame particle budget** with distance LOD.

---

## 8. CanvasRenderer (2D UI) (`canvas/`)

```cpp
class CanvasRenderer {                             // immediate-mode, orthographic
public:
    void Begin(FrameContext&, UINT w, UINT h) noexcept;
    void Quad(Rect, Color, TextureGpu* = nullptr) noexcept;
    void Text(Vec2, std::string_view, Color) noexcept;   // monospace bitmap atlas (§12.2)
    void Line(Vec2 a, Vec2 b, Color) noexcept;
    void End() noexcept;                            // flush batched quads/text/lines
};
```

- **Batched** quads / monospace text / lines / rects in dynamic vertex buffers from the
  upload ring; one (or few) draws via the `Canvas` root signature.
- The **UI/HUD toolkit, radar and screens** (§22, `ui-hud-layout.md`) are built **on top
  of** Canvas in `EarthRise.Client/ui` — Canvas itself stays a primitive layer.
- **DPI/HUD-scale** applied as an orthographic transform; localized text via the string
  table (§22.4).

---

## 9. Asset loaders (`assets/`)
- **DdsLoader:** parse `DDS_HEADER`[+`DXT10`] → `DXGI_FORMAT` (BC1–BC7 + mips) →
  `TextureGpu` (custom parser, §12.1).
- **CmoLoader:** custom CMO reader (materials, ≤8 tex slots, vertex/index buffers,
  optional skinning) → `MeshGpu`; `meshcook` repacks for instancing (§12.3).
- **FontAtlas:** fixed-grid monospace (cols/rows/firstCodepoint/cellPx), codepoint→cell
  UVs (§12.2), Unicode-capable for localization (§22.4).

---

## 10. Threading & lifecycle
- Render runs on its **own thread**, decoupled from the 30 Hz sim/20 Hz snapshot cadence
  (interpolated state in, §8.4). Single-threaded command recording at **M1b**; parallel
  recording (per-pass lists) is a later lever.
- **Resize:** `WaitForGpu` → recreate backbuffers/size-dependent targets.
- **UWP suspend:** `WaitForGpu` → `IDXGIDevice3::Trim()`; **resume:** rebuild transient
  GPU state. **Device-removed:** dump DRED → recreate device + all resources.
- **Profiling:** PIX markers around passes; a **GPU timestamp query heap** (`GpuTimer`)
  feeds the **render frame-time perf gate** (§16.3, App. B).

---

## 11. Testing — `NeuronRenderTest` (+ headless logic in testrunner)
- **Device-dependent (Windows agent):** `DeviceResources` init/teardown (incl. WARP),
  swap-chain create/resize, fence sync, suspend/resume, a 1-triangle smoke frame.
- **Logic (mockable / Linux-testable):** **camera math** (View/Proj RH, frustum),
  **floating-origin rebase**, descriptor-ring/upload-ring allocation, `PsoKey` hashing,
  Canvas **batch building** & text UV mapping, DDS/CMO/Font parser edge cases (shared
  with §16.2).

---

## 12. Milestone mapping
- **M1b:** DeviceResources, triple-buffer + fences, descriptor heaps, one Scene + one
  Canvas pass, PSO cache, camera + basic input (§11.1, §17 M1b).
- **M2:** full HDR pass graph (bloom, tonemap, post), **GPU-compute particles**,
  instanced fleets, radar/HUD on Canvas (with §22 / `ui-hud-layout.md`).
- **M4+:** residency/streaming under interest scale; parallel command recording if the
  render thread saturates.

---

## 13. Open questions (validation)
- Backbuffer format: `R8G8B8A8_UNORM` vs `R10G10B10A2` (HDR-ish banding on bloom).
- Post-AA: skip (rely on bloom) vs a cheap FXAA-style pass in tonemap.
- Bloom cost vs quality (mip-chain downsample vs fixed ping-pong) at min-spec.
- Whether to add an optional **compute queue** (vs compute on the direct queue) for
  particle sim overlap.
- `ID3D12PipelineLibrary` disk-cache invalidation across driver/Store updates.

> See also: `masterplan.md` §11 / §12, `ui-hud-layout.md`, `neuronaudio-api.md`
> (shares the camera-relative/floating-origin rule).
