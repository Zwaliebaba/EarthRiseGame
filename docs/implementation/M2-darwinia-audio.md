# M2 — Darwinia Look + Audio (Implementation Plan)

> Derived from [`../masterplan.md`](../masterplan.md) §17 (milestone **M2**).
> **Status:** 🔨 In progress (M0/M1a/M1b complete per masterplan footer).
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

## Current state (what M1b left us)

- `NeuronRender/` has `DeviceResources` (DX12 device/swap-chain/fences, §11.1),
  `SceneRenderer` (draws **placeholder unit cubes** — bases 100 m blue, ships 20 m orange —
  via per-instance stream; header explicitly says *"M2 replaces the cube with real CMO mesh
  assets and adds bloom"*), and `CanvasRenderer`.
- Shaders present: `SceneVS/PS`, `CanvasVS/PS` (HLSL → embedded DXIL, §12.4). `ScenePS`
  notes *"M2 adds bloom pre-pass and additive particles."* Scene currently renders straight
  to the LDR backbuffer — **no HDR target / bloom chain yet**.
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
  - [ ] **DDS reader** — `DDS_HEADER` (+`DXT10`) → `DXGI_FORMAT`, BC1–BC7 + mips, upload via
        COPY queue (§11.1). Target `NeuronRender` (texture loader, gfx filter) + a parser unit
        usable headless.
  - [ ] **CMO reader** — materials + ≤8 texture slots (diffuse = DDS), vertex streams
        (pos/normal/tangent/color/uv), 16-bit indices, submeshes; static meshes first
        (skinning later, R11). Target `NeuronRender/assets/`.
  - [ ] **Monospace font pipeline** — fixed-grid atlas; codepoint→cell; config
        `cols,rows,firstCodepoint,cellPx` (§12.2, §22.4 Unicode-capable). Feeds area F.
  - [ ] **`NeuronTools/`** project group: `ddscheck`, `meshcook` (repack for instancing),
        `fontpack`, plus `datacook`/`datacheck` (§12.6) for the audio-cue catalog (area F/G)
        and `wavcheck` (area E). Wire as pre-build/CI steps (§16).
  - [ ] **`assets/`** top-level dir per §5 (`.dds`, font bitmaps, `.cmo`, `audio/*.wav`).
- **Tests (`<project>Test`, §16.1):**
  - [ ] `NeuronRenderTest`: DDS parser — valid header, each BC format, mip count, truncated/
        garbage → clean failure.
  - [ ] `NeuronRenderTest`: CMO parser — submesh/material/index counts on a known mesh;
        malformed → rejected, not crash.
  - [ ] Platform-independent parser cases also added to `NeuronTools/testrunner/` (§16.2) so
        Linux CI catches regressions without a Windows build.
  - [ ] `datacheck` referential-integrity run in CI for the cue catalog (cue→clip exists,
        3D⇒mono honored — §12.6).
- **Depends on:** nothing (foundation). **Blocks:** B, E (wavcheck), F.

### B. SceneRenderer upgrade — real instanced CMO ships

- **Goal:** replace placeholder cubes with instanced CMO meshes + materials, keeping the
  per-instance stream from M1b.
- **Masterplan refs:** §11 (Scene), §11.1 (binding model: per-instance structured-buffer
  SRV, descriptor tables for material textures), §12 (CMO/DDS).
- **Current state:** `SceneRenderer` draws a unit cube via instance stream; viewProj via root
  constants; `kMaxEntities = 512`.
- **Work:**
  - [ ] Load base + ship CMO meshes (area A) into DEFAULT-heap vertex/index buffers.
  - [ ] Material/texture binding: descriptor tables for diffuse DDS, static samplers (§11.1).
  - [ ] Keep instanced draw path; per-instance world matrix + emissive (already in stream).
  - [ ] Low-poly bright-emissive silhouettes per the Darwinia look (§11).
- **Tests (`NeuronRenderTest`):**
  - [ ] Mesh→GPU buffer sizing/stride matches CMO; instance stream layout unchanged.
  - [ ] SceneRenderer init/teardown with a loaded mesh (no leaks; DeviceResources teardown
        path from existing M1b tests still green).
- **Depends on:** A (DDS+CMO). **Blocks:** C (bloom needs real emissive geometry to look
  right, but can be developed against cubes).

### C. HDR forward + bloom + tone-map

- **Goal:** the §11.1 pass graph — render scene to an HDR `R16G16B16A16_FLOAT` target,
  bright-pass → Gaussian ping-pong bloom (additive) → tone-map to LDR backbuffer.
- **Masterplan refs:** §11 (bloom/additive), §11.1 (pass graph steps 1–4, no MSAA, barriers).
- **Current state:** scene renders directly to LDR backbuffer; no HDR target, no post chain.
- **Work:**
  - [ ] Add HDR scene target + `D32_FLOAT` depth; render area B into it.
  - [ ] Bright-pass extract → downsample → separable Gaussian blur (ping-pong) PSOs +
        shaders (`shaders/` HLSL → embedded DXIL, §12.4).
  - [ ] Tone-map + optional scanline/vignette/grain → LDR backbuffer.
  - [ ] Manual resource-state barriers RT↔SRV, batched (§11.1). Canvas (area F) composites
        after tone-map, no bloom.
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
- **Current state:** does not exist.
- **Work:**
  - [ ] Create `NeuronAudio/` (`engine/` XAudio2, `spatial/` X3DAudio, `wav/` RIFF reader,
        `mixer/` buses) + `NeuronAudioTest`. Links NeuronCore (math/types), **not**
        NeuronRender. Add both to `EarthRise.sln`/`.slnx`.
  - [ ] **WAV/RIFF reader** — parse `RIFF`/`fmt `/`data` → `WAVEFORMATEX` + PCM-16 samples;
        mono (3D emitters) / stereo (music/ambient/UI). No MP3/OGG/ADPCM.
  - [ ] **Voice graph** — mastering voice → 4 submix buses → pooled source voices; per-bus +
        master volume. Event SFX loaded fully; ambient beds + music **streamed** via
        buffer-queue `IXAudio2VoiceCallback`.
  - [ ] **X3DAudio** — listener = scene camera (pos/orient/velocity, camera-relative /
        floating-origin — **no `int64` reaches audio**, R2); emitters compute output matrix +
        Doppler + distance LPF (`SetOutputMatrix`/`SetFrequencyRatio`/filter).
  - [ ] **Event sounds = client-side feedback** off replicated sim events (NeuronClient
        replica/interp) — no audio on the wire, no determinism requirement.
  - [ ] UWP **suspend/resume**: stop/restart engine, release/reacquire voices.
  - [ ] `wavcheck` tool (area A/NeuronTools) validates assets at build time.
- **Tests (`NeuronAudioTest`, §16.1):**
  - [ ] WAV/RIFF parser — valid PCM-16 mono/stereo, invalid/compressed rejected, truncated
        chunk handled, edge cases.
  - [ ] Bus/volume logic (per-bus + master gain composition).
  - [ ] X3DAudio emitter math (pan/Doppler/distance) on known listener/emitter setups.
  - [ ] XAudio2 device init/teardown (Windows-only; lives in `NeuronAudioTest`).
  - [ ] Platform-independent WAV-parser cases also in `NeuronTools/testrunner/` (§16.2).
- **Depends on:** A (wavcheck), camera (M1b, exists). **Blocks:** Done gate audio clauses.
- **⚠️ Guard:** confirm **ERHeadless still builds and runs with no audio** after this lands
  (CI must build ERHeadless without linking NeuronAudio) — an explicit M2 *Done* clause.

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

- [ ] Instanced fleet of **real CMO meshes** with **thrusters + glow** renders (B, C, D).
- [ ] Over a **legible monospace HUD** with **radar/overview basics** (F).
- [ ] At **target render frame time** (App. B; measured via §11.1 timestamp queries) (H).
- [ ] **3D-positioned** thruster/weapon SFX + **ambient bed** + **music**, mixed across the
      4 buses (E).
- [ ] **ERHeadless still builds/runs with no audio** (E guard, H).
- [ ] **GPU-compute particles** working (D).
- [ ] **Settings screen** present and wired (G).
- [ ] **Windowed menu/options UI** reproduces the reference canvas (Main Menu +
      Screen/Graphics/Other Options), interactive, strings via the §22.4 table
      ([`docs/design/darwinia-menu-ui.md`](../docs/design/darwinia-menu-ui.md)) (F).
- [ ] All 14 `<project>Test` suites green, incl. new **NeuronAudioTest** (§16.1).
