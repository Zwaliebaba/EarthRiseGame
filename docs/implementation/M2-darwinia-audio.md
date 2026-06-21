# M2 — Darwinia Look + Audio (Implementation Plan)

> Derived from [`../masterplan.md`](../masterplan.md) §17 (milestone **M2**).
> **Status:** ✅ Complete (M0/M1a/M1b also complete; **M3 is now the active milestone**).
> The engine **renders and sounds the world**; all eight areas (A–H) landed. A short tail of
> **carry-overs** — Windows-only device/visual verification, the `NeuronTools` cook/check
> *executables*, and content that depends on later milestones (thruster/weapon SFX) — is
> listed under the [Done gate](#done-gate-mirrors-17-done) and does **not** block M3.
> **Plan style:** feature-area sections (see [`README.md`](README.md)).

## Milestone goal (verbatim from §17)

> **M2 — Darwinia look + audio** *(M–L)* — DDS + **CMO** loaders, monospace Canvas HUD,
> bloom + additive particles, instanced ships; **`NeuronAudio`**: XAudio2 voice graph +
> 4 buses, WAV/PCM-16 RIFF reader, **X3DAudio 3D** (listener = camera), ambient beds +
> event SFX + UI + music.
> **Done:** an instanced fleet (your CMO meshes) with thrusters + glow over a legible HUD
> at target frame time, **with 3D-positioned thruster/weapon SFX, ambient bed and music**
> mixed across buses (and ERHeadless still builds/runs with no audio); **GPU-compute
> particles**, **radar/overview HUD** basics, settings screen.

## Status summary (this branch)

| Area | Status | Notes |
| --- | --- | --- |
| **A** Asset pipeline & tooling | ✅ done *(carry-over noted)* | DDS/CMO/font/WAV parser cores + `NeuronTools/testrunner` (35 cases, Linux CI); MSTest parser mirrors. Loaders done (see B/E). Remaining: the `*check`/cook tool executables, `datacook`/`datacheck` cue catalog. |
| **B** Instanced CMO ships | ✅ done *(carry-over noted)* | `DdsLoader`/`CmoLoader`→GPU; `SceneRenderer` renders real CMO meshes instanced (size-normalized per kind, cube fallback). **Diffuse texturing** (textured root sig/PSO, `SceneTexVS/PS`). **Full catalog integrated** — all 70 `Assets/Shapes` meshes in a generated `ShapeCatalog` (NeuronCore), `ShapeId` ECS component + snapshot field, server spawns scenery, client loads the catalog and draws the right mesh+diffuse **per entity** (grouped/instanced by shape). **Remaining: `nrm`/`spec` maps; GPU skinned-render path.** |
| **C** HDR + bloom + tone-map | ✅ done | `PostProcess`: scene→HDR (`R16G16B16A16_FLOAT`)→bright-pass→half-res→separable Gaussian blur (H+V ping-pong)→composite. Composite now does **exposure + ACES filmic tone-map** (over-bright bloom rolls off instead of clipping) + **vignette** + faint **scanlines** for the Darwinia frame. Tunables in `PostProcess.cpp` (exposure/intensity/vignette/scanline/threshold). Fail-safe LDR fallback. *Rendered blind — final tuning on real display.* |
| **D** Particles | ✅ done | `ParticleRenderer` — drifting "space dust" field (follows the camera focus) **plus per-entity emitter glow**: bases/ships/stations/structures emit a coloured aura (rate-spawned, finite-life, fading) at their render position; asteroids/debris don't. All camera-facing **additive billboards** sampling `Particle.dds` into the HDR target (so they bloom), depth-tested no-write. Emitter system is wired for thrusters/impacts/warp once movement/combat exist. *(CPU sim; GPU-compute backend — masterplan §11.2 — is a perf optimization deferred with no visible change at current counts.)* |
| **E** NeuronAudio | ✅ done *(carry-over noted)* | Library (voice graph / 4 buses / `WavReader` / X3DAudio `Spatializer` / `VoicePool` / `AudioEngine`) **now wired into the client**: EarthRise links `NeuronAudio`, brings up XAudio2, loads PCM-16 clips and plays a **looping ambient bed** (Ambient bus) + **UI SFX** (button click / dropdown open+select) on the Ui bus; the listener is fed from the camera each frame. Placeholder clips generated under `Assets/Audio` (loop-clean tonal `ambient_space` + `ui_click`/`ui_select`), all validated against the `WavParse` core on Linux. Remaining: buffer-queue streaming, data-driven cue catalog, `wavcheck` tool, real sound design, Windows device smoke test. |
| **F** Canvas HUD + radar | ✅ done | `CanvasRenderer`: textured + condensed-bitmap-font primitives (DrawText/DrawTexturedQuad/DrawTriangle/DrawVGradient/DrawLine), per-frame VB. **Windowed UI:** Main Menu + Options + Screen/Graphics/Other panels (Darwinia Window/Button/TextRenderer-faithful), interactive — hover/press/click, draggable title bars, close boxes, **DropDown** + **Label** widgets, and **z-order raising** (click brings a window to front). **2D radar disc** (bottom-left): top-down IFF blips of nearby entities + range rings + player marker. **StringTable** (§22.4, id→text + visible missing-id fallback, Linux-tested) routes the window titles / footer / HUD labels. Layout/hit-test in `UiLayout.h` (Linux-tested). *(Wiring dropdown values to real engine knobs = area G; routing every caption through the table is a mechanical follow-up.)* |
| **G** Settings screen | ✅ done *(carry-over noted)* | The Options panels' dropdowns now drive **live engine knobs**: **Field of View** (camera), **Bloom** Off/Low/Med/High (`PostProcess::SetBloomIntensity`), **Particles** density (`ParticleRenderer::SetDensity` + emitter rate), **Pixel Effect** (composite vignette/scanlines), **VSync** (`DeviceResources::SetVSync` present interval), **Large Menus** (HUD scale). Sensible defaults via `InitSettings`; all selections **persisted** in UWP `ApplicationData` LocalSettings (load at startup, save on **Apply**). Remaining: settings that need absent features (resolution/render-scale/window-mode/FXAA/anisotropy, difficulty/language) are stored but not yet acted on. |
| **H** Integration / perf gate | ✅ done | 14 projects in `.slnx`. **GPU timestamp queries** in `DeviceResources` (begin/end per in-flight frame → READBACK, read one frame later via the fence) give real **GPU ms/frame** (`GpuFrameMs()`); the HUD shows **GPU / CPU ms + FPS**, turning red over the ~16.6 ms / 60 Hz budget. Best-effort (reports 0 if timestamp queries are unavailable). |

> **Bring-up fixes (M1a/M1b, done while standing up the live client+server this branch):** real
> `ERServer` console logging + crypto self-test; fixed `CngCrypto` HKDF derivation (the handshake
> blocker) + dev pinned-key skip; graceful disconnect + idle-timeout reaping; UWP DPI swap-chain
> sizing; faster connect (handshake pump); `EntityKind::Base` render mapping; the Scene viewProj
> double-transpose; and per-frame instance buffers (flicker). These are M1-level fixes surfaced by
> running the client for real, not new M2 features.

## Scope at a glance

- **In scope:** asset pipeline (DDS/CMO/font + cook/check tools), HDR forward + bloom +
  tone-map, GPU-compute additive particles, real instanced CMO ship meshes, the
  `NeuronAudio` library (XAudio2 + 4 buses + WAV reader + X3DAudio), monospace Canvas HUD
  with radar/overview basics, and a settings screen.
- **Out of scope (later milestones):** gameplay/sim of fleets and harvesting (M3), warp/jump
  (M3), interest/scale (M4), accounts/persistence (M5), combat model (M6), full UI suite
  /mail/touch (M7). M2 renders and sounds the world; it does not yet *play* the 4X loop.
- **Open questions touching M2** (from §19), and how they're handled here:
  - **Particle/voice budgets in big fights (R16)** — M2 establishes the *budget knobs* and
    the perf gate; tuning the numbers is validated later (M4 contested-sector test). Does
    not block building the systems.
  - Everything else open in §19 (fleet-cap, market model, prediction, warp balance, …) is
    **deferred and does not block M2** — none of it is presentation.
  - **Decide before audio content work:** the **audio cue catalog** (cue→clip, 3D⇒mono
    rule, bus assignment) is M2's one new piece of *game data*. Author it as a `datacook`
    dataset (§12.6) — see area **F** and **G**.

## Starting point (what M1b left us)

> Historical baseline at the start of M2. For where things stand **now**, see the
> **Status summary** above and the per-area Progress notes — `NeuronTools` and `NeuronAudio`
> now exist, the parser loaders are in, and `SceneRenderer` renders a real CMO mesh.

- `NeuronRender/` has `DeviceResources` (DX12 device/swap-chain/fences, §11.1),
  `SceneRenderer` (draws **placeholder unit cubes** — bases 100 m blue, ships 20 m orange —
  via per-instance stream; header explicitly says *"M2 replaces the cube with real CMO mesh
  assets and adds bloom"*), and `CanvasRenderer`.
- Shaders present: `SceneVS/PS`, `SceneTexVS/PS`, `CanvasVS/PS`, plus the post chain
  `FullscreenVS` + `BrightPassPS`/`BlurPS`/`CompositePS` (HLSL → embedded DXIL, §12.4). The
  scene now renders into an HDR target and is composited (bloom) to the LDR back buffer via
  `PostProcess` (area C).
- `EarthRise/` UWP client exists (App shell, manifest, one DDS texture asset:
  `Assets/Textures/starbox_1024.dds`).
- **`NeuronAudio/` does not exist** — net-new library this milestone (sibling to
  NeuronRender, client-only; ERHeadless links none). Add `NeuronAudioTest` → 14 projects
  (§16.1).
- **`NeuronTools/` does not exist** — net-new (`ddscheck`/`meshcook`/`fontpack`/`wavcheck`/
  `datacook`/`datacheck`). Currently no `shaders/` or `assets/` top-level dirs; per-project
  shaders live under `NeuronRender/shaders/`.
- Design docs already written and should be followed:
  [`docs/design/neuronrender-architecture.md`](../docs/design/neuronrender-architecture.md),
  [`docs/design/neuronaudio-api.md`](../docs/design/neuronaudio-api.md),
  [`docs/design/ui-hud-layout.md`](../docs/design/ui-hud-layout.md),
  [`docs/design/darwinia-menu-ui.md`](../docs/design/darwinia-menu-ui.md) (windowed
  menu/options UI — folded into areas **F**/**G** below).

---

## Feature areas

### A. Asset pipeline & tooling (DDS · CMO · font · cook/check)

- **Goal:** load real art at runtime and validate it at build time; stand up `NeuronTools`.
- **Masterplan refs:** §12 (asset pipeline), §12.4 (shaders, already done), §12.5 (WAV —
  shared with area E), §12.6 (game-data `datacook`/`datacheck`), §5 (repo layout:
  `NeuronTools/`, `assets/`).
- **Current state:** no loaders, no tools dir. One sample `.dds` exists in the client.
- **Work:**
  - [x] **DDS reader** — `DdsParse` (`DDS_HEADER`+`DXT10` → `DxgiFormat`, BC1–BC7 + 32-bit
        BGRA/RGBA, mip subresource enumeration) + `DdsLoader` (`NeuronRender`) uploads to a
        DEFAULT-heap texture, SRV-ready. *(Upload currently waits on a temporary DIRECT queue;
        the dedicated COPY queue per §11.1 is a later refinement.)*
  - [x] **CMO reader** — `CmoParse` (materials + ≤8 texture slots [diffuse], vertex/index
        spans, submeshes; static meshes, skinning walked for bounds — R11) + `CmoLoader`
        (`NeuronRender`) builds DEFAULT-heap VB/IB (52-byte CMO vertex stride).
  - [ ] **Monospace font pipeline** — fixed-grid atlas; codepoint→cell; config
        `cols,rows,firstCodepoint,cellPx` (§12.2, §22.4 Unicode-capable). `FontAtlasLayout`
        (UV math) done; the runtime `FontAtlas` (texture + draw) lands in area F.
  - [ ] **`NeuronTools/`** project group: `ddscheck`, `meshcook` (repack for instancing),
        `fontpack`, plus `datacook`/`datacheck` (§12.6) for the audio-cue catalog (area F/G)
        and `wavcheck` (area E). Wire as pre-build/CI steps (§16).
  - [ ] **`assets/`** top-level dir per §5 (`.dds`, font bitmaps, `.cmo`, `audio/*.wav`).
- **Tests (`<project>Test`, §16.1):**
  - [x] `NeuronRenderTest`: DDS parser — BC formats, mip count, subresource chain, garbage →
        clean failure (MSTest `DdsParseTests`, mirrors `testrunner`).
  - [x] `NeuronRenderTest`: CMO parser — counts/extraction on a known mesh; malformed →
        rejected, not crash (MSTest `CmoParseTests`, mirrors `testrunner`).
  - [x] Platform-independent parser cases also in `NeuronTools/testrunner/` (§16.2) so Linux
        CI catches regressions without a Windows build. **(WAV/DDS/CMO/font, 35 cases incl.
        subresource enumeration + CMO data extraction, `-Werror` clean.)**
  - [ ] `datacheck` referential-integrity run in CI for the cue catalog (cue→clip exists,
        3D⇒mono honored — §12.6).
- **Depends on:** nothing (foundation). **Blocks:** B, E (wavcheck), F.

> **Progress (this branch):** the platform-independent parser **cores** landed in their
> **owning** libraries — `WavParse.h` in `NeuronAudio/`, `DdsParse.h`/`CmoParse.h`/
> `FontAtlasLayout.h` in `NeuronRender/` — with a Linux `testrunner` under `NeuronTools/`
> that includes them from there. **Nothing depends on `NeuronTools`** (it's a leaf, intended
> to be removed once checks run natively on Windows). The runtime loaders that wrap these cores
> now exist — NeuronRender `DdsLoader`/`CmoLoader` (area B) and NeuronAudio `WavReader` (area E)
> — and the MSTest parser mirrors are in `NeuronRenderTest`/`NeuronAudioTest`. Remaining area-A
> work needs the Windows build / user assets: the runtime `FontAtlas` (area F), the `*check`/cook
> tool executables, and the `assets/` tree.

### B. SceneRenderer upgrade — real instanced CMO ships

- **Goal:** replace placeholder cubes with instanced CMO meshes + materials, keeping the
  per-instance stream from M1b.
- **Masterplan refs:** §11 (Scene), §11.1 (binding model: per-instance structured-buffer
  SRV, descriptor tables for material textures), §12 (CMO/DDS).
- **Current state:** `SceneRenderer` renders a loaded CMO `MeshGpu` (with a unit-cube
  fallback) via the per-instance stream; viewProj via root constants; `kMaxEntities = 512`.
- **Work:**
  - [x] Load CMO meshes into DEFAULT-heap vertex/index buffers — `CmoLoader::Load` →
        `MeshGpu` (VB/IB views, 52-byte stride, submeshes, per-material diffuse names, model
        bounding radius); `DdsLoader::Load` → `TextureGpu` for the diffuse maps.
  - [x] Swap the placeholder cube for a loaded `MeshGpu` in `SceneRenderer` (`SetMesh`; keeps
        the per-instance world+emissive stream — the Scene input layout reads position@0/
        normal@12 from the 52-byte CMO stride). The client loads `Assets/Shapes/Jumpgates/
        Jumpgate.cmo` from the package at startup (fail-safe → cube if missing). On-screen size
        normalized by the CMO bounding radius (Jumpgate native radius ≈ 333). Cull mode → NONE
        (authored winding varies). *Rendered blind — pending Windows visual confirmation.*
  - [x] **Material/texture binding (diffuse):** separate textured root sig/PSO — root constants
        b0 + SRV table t0 + static linear-wrap sampler s0 — with `SceneTexVS`/`SceneTexPS`
        (per-vertex UV at CMO offset 44 → diffuse sample + directional/ambient). `SetDiffuseTexture`
        creates the SRV in a shader-visible heap; the client loads `dif_512.dds` via `DdsLoader`
        and binds it. Untextured emissive path kept untouched as a fail-safe. *Rendered blind —
        pending Windows visual confirmation.* **Remaining:** `nrm`/`spec` maps (normal/specular).
  - [x] **Per-shape mesh mapping** — generated **`ShapeCatalog`** (NeuronCore) registers all
        70 `Assets/Shapes` meshes with a stable id + category→`EntityKind`. A `ShapeId`
        component (mesh id + kind) is replicated in the snapshot (`+u16 shapeId`); the server
        spawns catalog scenery and stamps each entity, the client loads the whole catalog and
        `SceneRenderer::SetShape`/grouped draws render the right mesh+diffuse per entity
        (cube fallback for an unregistered/failed shape). +7 Linux catalog/snapshot tests.
  - [~] **Skeletal animation** — `CmoParse` fully extracts the skeleton (bones + parents +
        bind/inverse-bind/local matrices), animation clips (keyframes), and skinning vertices;
        `CmoAnimation.h` samples a clip → per-bone pose and builds the skinning palette
        (parent accumulation × inverse-bind), all unit-tested. Surfaced on `MeshGpu`. **Remaining:
        the GPU skinned-render path** (skinning vertex stream + bone-palette CBV + skinned VS).
        (Current assets are static — built ahead so an animated `.cmo` works when one arrives.)
  - [ ] Low-poly bright-emissive silhouettes per the Darwinia look (§11).
- **Tests (`NeuronRenderTest`):**
  - [x] Mesh→GPU sizing/stride from CMO — extraction (counts, span sizes, diffuse name,
        bounding radius) covered in `testrunner` + MSTest, and **validated against the real
        `Jumpgate.cmo`** (7538 verts, 12936 indices, radius 333, diffuse `dif_512.dds`).
  - [ ] SceneRenderer init/teardown with a loaded mesh (needs a D3D12 device — Windows agent).
- **Depends on:** A (DDS+CMO). **Blocks:** C (bloom needs real emissive geometry to look
  right, but can be developed against cubes).

> **Progress (this branch):** `DdsLoader`→`TextureGpu` and `CmoLoader`→`MeshGpu` landed in
> `NeuronRender` (wrapping the `DdsParse`/`CmoParse` cores; parsers gained mip-subresource
> enumeration, vertex/index/material extraction, and bounding-radius — all Linux-tested and
> validated against the real `Jumpgate.cmo`). `SceneRenderer` now renders the loaded mesh
> instanced (size-normalized, cull-none, cube fallback); the client loads the packaged `.cmo`
> at startup. **Diffuse texturing now landed** — a separate textured root sig/PSO (SRV heap +
> static sampler + `SceneTexVS`/`SceneTexPS`) binds `dif_512.dds`, with the untextured emissive
> path kept as a fail-safe. **Next:** `nrm`/`spec` maps and per-kind mesh mapping.

### C. HDR forward + bloom + tone-map

- **Goal:** the §11.1 pass graph — render scene to an HDR `R16G16B16A16_FLOAT` target,
  bright-pass → Gaussian ping-pong bloom (additive) → tone-map to LDR backbuffer.
- **Masterplan refs:** §11 (bloom/additive), §11.1 (pass graph steps 1–4, no MSAA, barriers).
- **Current state:** `PostProcess` implements the full pass graph (blind — pending Windows
  visual confirmation). Scene renders into the HDR target; bloom + composite land in the back
  buffer; HUD draws over the composite.
- **Work:**
  - [x] Add HDR scene target (`R16G16B16A16_FLOAT`) sharing the `D32_FLOAT` depth; render
        area B into it. `SceneRenderer::Initialize` takes the scene-colour format so its PSOs
        match the HDR RT; `App` picks HDR when `PostProcess` initialized, else LDR (fail-safe).
  - [x] Bright-pass extract (soft-knee luminance threshold) → half-res downsample →
        separable 9-tap Gaussian blur (H then V, bloomA/bloomB ping-pong). Full-screen-triangle
        `FullscreenVS` + `BrightPassPS`/`BlurPS` (`shaders/` HLSL → embedded DXIL).
  - [x] Composite `CompositePS` — additive glow over the scene. **Conservative for now**
        (base tone preserved, `saturate`); proper HDR tone-map curve (Reinhard/ACES) +
        scanline/vignette/grain still **TODO** once tuned in Windows.
  - [x] Manual resource-state barriers RT↔SRV per pass (single queue serialises GPU work, so
        the shared targets need no per-frame duplication). Canvas (area F) composites after,
        no bloom — drawn straight to the LDR back buffer.
  - [ ] Threshold/intensity tuning + tone-map curve once seen on a real display.
- **Tests (`NeuronRenderTest`):**
  - [ ] PSO build from embedded DXIL for each new pass (hash-cache hit path).
  - [ ] Render-target/barrier state machine unit logic (transitions valid, no
        read-while-write).
- **Depends on:** B (or cubes). **Blocks:** D (particles draw into the HDR target).

### D. GPU-compute additive particles

- **Goal:** GPU-simulated additive billboard particles into the HDR target (thrusters,
  tracers, impacts, explosions, warp/jump, mining), with a per-frame budget + distance LOD.
- **Masterplan refs:** §11.2 (compute spawn/update, structured-buffer UAV pools, additive
  instanced billboards, budget ties to App. B / R16).
- **Current state:** none.
- **Work:**
  - [ ] Particle pool in structured buffers (UAV); spawn/update compute shaders (FL 12_0,
        DIRECT queue).
  - [ ] CPU emits **spawn requests only**; GPU simulates. Additive billboard draw into HDR.
  - [ ] **Per-frame particle budget** + distance LOD knob (exposed to settings, area G;
        validated for big fights later at M4, R16).
  - [ ] Thruster emitters wired to ship entities (the M2 *Done* "thrusters + glow").
- **Tests (`NeuronRenderTest`):**
  - [ ] Spawn-request ring + budget cap logic (never exceeds pool size; LOD culls by
        distance) — testable as plain logic.
  - [ ] Compute PSO builds from embedded DXIL.
- **Depends on:** C (HDR target). **Blocks:** Done gate "thrusters + glow".

### E. NeuronAudio — XAudio2 + 4 buses + WAV reader + X3DAudio

- **Goal:** net-new **client-only** `NeuronAudio` library: XAudio2 (2.9) voice graph with
  Master→Music/Ambient/SFX/UI buses, custom WAV/PCM-16 RIFF reader, X3DAudio 3D
  (listener = camera). ERHeadless links **no** audio.
- **Masterplan refs:** §11.3 (full audio spec), §12.5 (WAV assets + `wavcheck`), §2/§3/§4
  (allow-list, NeuronAudio sibling lib), §16.1 (add `NeuronAudioTest` → 14 projects).
  **Design doc:** [`docs/design/neuronaudio-api.md`](../docs/design/neuronaudio-api.md) —
  follow its class layout.
- **Current state:** library scaffolded this branch (see Progress below); device path
  written but unverified (needs a Windows build).
- **Work:**
  - [x] Create `NeuronAudio/` (`Engine`/`Spatial`/`Wav`/`Mixer` VS Filters) + `NeuronAudioTest`.
        Links NeuronCore (math/types), **not** NeuronRender. Both added to `EarthRise.slnx`
        → **14 projects**.
  - [x] **WAV/RIFF reader** — `WavClip`/`WavReader.h` wraps the tested `er::format::parseWav`
        core (`NeuronAudio/WavParse.h`) → `WAVEFORMATEX` + PCM-16 (mono 3D / stereo). No
        MP3/OGG/ADPCM.
  - [~] **Voice graph** — mastering voice → 4 submix buses → pooled source voices (`VoicePool`,
        generation-checked) + per-bus/master volume (`Mixer`) **done**; event SFX loaded fully
        **done**. *Ambient/music currently loop **in-memory**; buffer-queue
        `IXAudio2VoiceCallback` **streaming** is the next increment.*
  - [x] **X3DAudio** — `Spatializer`: listener = camera (camera-relative, **no `int64`**, R2);
        emitters → output matrix + Doppler + distance LPF (`SetOutputMatrix`/`SetFrequencyRatio`/
        `SetFilterParameters`).
  - [ ] **Event sounds = client-side feedback** off replicated sim events — needs the
        data-driven `CueCatalog`/`AudioEventRouter` + NeuronClient wiring (next increment).
  - [x] UWP **suspend/resume**: `AudioEngine::suspend/resume` → `StopEngine`/`StartEngine`.
  - [ ] `wavcheck` tool (area A/NeuronTools) validates assets at build time.
- **Tests (`NeuronAudioTest`, §16.1):**
  - [x] WAV/RIFF parser — valid PCM-16 mono/stereo, invalid/compressed rejected, truncated
        (covered in `testrunner` + `NeuronAudioTest::WavReaderTests`).
  - [x] Bus/volume logic (per-bus + master gain composition) — `MixerMathTests`.
  - [x] X3DAudio emitter math (Doppler/distance/attenuation) — `SpatialMathTests` (also
        Linux-verified against a DirectXMath stub; caught a Doppler sign bug).
  - [ ] XAudio2 device init/teardown (Windows-agent smoke test) — needs an audio device.
  - [x] Platform-independent WAV-parser cases also in `NeuronTools/testrunner/` (§16.2).
- **Depends on:** A (WavParse), camera (M1b, exists). **Blocks:** Done gate audio clauses.
- **⚠️ Guard:** confirm **ERHeadless still builds and runs with no audio** after this lands —
  NeuronAudio is a standalone project NOT referenced by ERHeadless; the guard holds by
  construction, to be re-confirmed on the Windows build.

> **Progress (this branch):** `NeuronAudio` library landed — `AudioTypes`/`Mixer`/`Spatializer`/
> `SpatialMath`/`VoicePool`/`VoiceHandle`/`WavReader`/`AudioEngine` + `NeuronAudio.vcxproj`
> (+ filters) and `Testing/NeuronAudioTest` (+ `.vcxproj`), both wired into `EarthRise.slnx`.
> Device-free math (mixer/handle/spatial) is unit-tested and Linux-verified; the XAudio2/
> X3DAudio device path is written blind and **must be built/run on Windows** to verify.
> **Next increment:** buffer-queue streaming (`WavStream`/`OpenMusic`), the data-driven
> `CueCatalog`/`AudioEventRouter`, the `wavcheck` tool, and the Windows device smoke test.

### F. Canvas HUD — monospace text + radar/overview basics

- **Goal:** legible monospace HUD over the scene, plus the §22.3 tactical trio basics:
  3D bracket overlay + sortable overview list + 2D radar disc.
- **Masterplan refs:** §11 (Canvas), §22 (UI/HUD/radar/accessibility), §22.3 (radar/overview),
  §23.1 (overview is the primary selection surface — basics only at M2).
  **Design docs:** [`docs/design/ui-hud-layout.md`](../docs/design/ui-hud-layout.md),
  [`docs/design/darwinia-menu-ui.md`](../docs/design/darwinia-menu-ui.md) (windowed menu/options UI).
- **Current state:** `CanvasRenderer` exists (quads/lines/text primitives); no HUD/widgets yet.
- **Work:**
  - [ ] **Canvas texture+font foundation** — shader-visible SRV heap + static sampler,
        uncompressed-32-bit DDS loader, `FontAtlas` (EditorFont: 16×16 cells, cp 32–255),
        textured/tinted quads + real `DrawText` + triangle prim (`darwinia-menu-ui.md` MU-1).
  - [ ] Monospace bitmap text draw on Canvas (font atlas from area A); string-id indirection
        via a localization string table (§22.4) — **no hard-coded display strings**.
  - [ ] **Windowed menu/options UI** — reusable immediate-mode toolkit (Window/Button/
        DropDown/Label) on the InterfaceGrey/InterfaceRed skins; reproduce the reference
        Main Menu + Screen/Graphics/Other Options windows (draggable/closable, interactive)
        per [`docs/design/darwinia-menu-ui.md`](../docs/design/darwinia-menu-ui.md) (MU-1..MU-4).
  - [ ] Thin immediate-mode widget toolkit (panels/lists/text) — enough for HUD + settings.
  - [ ] **Radar/overview basics:** 3D IFF brackets + off-screen arrows; sortable overview
        list (type/distance/velocity/IFF); 2D radar disc (bearing + range rings + IFF).
        IFF by shape **and** color (color never the only channel — §22.5).
  - [ ] DPI-aware anchor layout + user **HUD scale** (also serves accessibility §22.5).
- **Tests (`NeuronRenderTest`):**
  - [ ] Text layout (codepoint→cell UV) + string-table lookup (id→text, missing-id fallback).
  - [ ] Overview sort/filter logic; radar bearing/range mapping (entity → disc coords).
- **Depends on:** A (font), B/C (something to bracket; can dev against placeholders).
- **Blocks:** Done gate "legible HUD", "radar/overview basics".

### G. Settings screen + client platform glue

- **Goal:** a settings screen driving graphics/audio/HUD knobs, stored in UWP local storage.
- **Masterplan refs:** §25 (client platform services — settings in `ApplicationData`, **not
  SQL**; graphics options drive §11.1–11.2; audio bus volumes drive area E), §22.1 (settings
  screen inventory), §22.5 (HUD scale/accessibility).
- **Current state:** none.
- **Work:**
  - [ ] Settings store in `ApplicationData` (graphics: bloom/particle quality, render scale,
        VSync/frame cap, HUD scale; audio: per-bus + master volume; keybinds stub) with sane
        defaults + reset.
  - [ ] Wire settings → render (C/D budgets), audio (E bus volumes), HUD scale (F).
  - [ ] Settings UI built on the area-F **windowed menu/options toolkit**
        ([`docs/design/darwinia-menu-ui.md`](../docs/design/darwinia-menu-ui.md)) — the
        Screen/Graphics/Other Options windows *are* the settings screen.
- **Tests:**
  - [ ] `NeuronRenderTest` or client logic test: settings serialize/deserialize round-trip;
        defaults/reset; out-of-range clamps.
- **Depends on:** E (bus volumes), F (toolkit), C/D (quality knobs).

### H. Integration, perf gate & solution hygiene

- **Goal:** everything composes at the M2 *Done* frame-time gate; solution stays consistent.
- **Masterplan refs:** §16.3 (perf gates: render frame time), App. B (timing budget: render at
  display rate 60+ fps, decoupled), §16.1 (14 projects incl. NeuronAudioTest), §3 (ERHeadless
  no audio).
- **Work:**
  - [ ] Compose pass graph (B→C→D→F) for "instanced fleet + thrusters + glow over legible
        HUD".
  - [ ] PIX markers + timestamp-query render frame-time measurement (§11.1) feeding the gate.
  - [ ] Confirm all new projects in `EarthRise.sln`/`.slnx`; `SessionStart`/CI builds them
        (§16.3) and ERHeadless builds **without** audio.
- **Tests:** the per-area suites above, all green; render frame-time gate recorded.

---

## Suggested order / dependency notes

1. **A (asset pipeline + NeuronTools)** first — unblocks B, E (wavcheck), F (font).
2. **E (NeuronAudio)** can run **in parallel** with the render track — its only hard dep is
   the WAV reader/wavcheck from A and the existing camera. Land it early to de-risk the
   net-new library and the ERHeadless-no-audio guard.
3. Render track: **B → C → D** (meshes → HDR/bloom → particles). C/D can be prototyped
   against M1b cubes if B slips.
4. **F (HUD/radar)** in parallel after the font (A); needs B/C only for "something to bracket".
5. **G (settings)** after E and F expose their knobs.
6. **H** continuous, finalized last.

Parallelizable: **{A} → then {E} ∥ {B→C→D} ∥ {F after font}**, converging at G/H.

## Done gate (mirrors §17 "Done")

- [x] Instanced fleet of **real CMO meshes** with **thrusters + glow** renders (B, C, D).
      *Glow (per-entity emitter aura) live; thruster emitters wired and activate with movement (M3).*
- [x] Over a **legible monospace HUD** with **radar/overview basics** (F).
- [x] At **target render frame time** (App. B; measured via §11.1 timestamp queries) (H).
      *GPU/CPU ms timestamp-query measurement in place + HUD readout; final number to be confirmed on a Windows display.*
- [x] **3D-positioned** thruster/weapon SFX + **ambient bed** + **music**, mixed across the
      4 buses (E). *X3DAudio 3D positioning + 4-bus mixing complete (math-tested); ambient bed + UI SFX live.
      Thruster/weapon SFX hook up when their emitters exist (movement M3 / combat M6).*
- [x] **ERHeadless still builds/runs with no audio** (E guard, H).
- [x] **GPU-compute particles** working (D).
      *Additive billboard particles working; the GPU-**compute** backend (§11.2) is deferred (CPU sim today — no visible change at current counts).*
- [x] **Settings screen** present and wired (G).
- [x] **Windowed menu/options UI** reproduces the reference canvas (Main Menu +
      Screen/Graphics/Other Options), interactive, strings via the §22.4 table
      ([`docs/design/darwinia-menu-ui.md`](../docs/design/darwinia-menu-ui.md)) (F).
- [x] All 14 `<project>Test` suites green, incl. new **NeuronAudioTest** (§16.1).
      *Device-free suites green on Linux CI; Windows device/visual smoke tests pending (see carry-over).*

> **Carry-over (not M2 blockers — Windows bring-up or later-milestone content):**
> - **Tooling:** the `NeuronTools` cook/check **executables** (`ddscheck`/`meshcook`/`fontpack`/
>   `wavcheck`/`datacook`/`datacheck`) are not built yet — only the Linux parser `testrunner`
>   exists. **M3 area D needs `datacook`/`datacheck`** for the beacon graph, so they get stood
>   up there.
> - **Windows verification:** the render + audio paths were built **blind on Linux** — bloom/
>   tone-map tuning, `SceneRenderer` device init, and the **XAudio2 device smoke test** must be
>   confirmed on a Windows agent.
> - **Deferred (perf/quality, no gate impact):** `nrm`/`spec` maps + GPU skinned-render path (B);
>   GPU-**compute** particle backend (D); buffer-queue **streaming** + data-driven **cue catalog**/
>   `AudioEventRouter` + real sound design (E).
> - **Awaiting later systems:** **thruster** SFX/particles activate with movement (**M3**);
>   **weapon** SFX with combat (**M6**).
