# EarthRise — Master Implementation Plan

> **Status:** DRAFT v0.12 — for review
> **Date:** 2026-06-19
> **Scope:** A space 4X MMO with a custom C++23 engine (**NeuronCore**), a
> containerized Windows dedicated server (**ERServer**) backed by a networked
> Microsoft SQL Server, a UWP/DirectX 12 client with **XAudio2/X3DAudio audio
> (NeuronAudio)**, and a headless client/bot host (**ERHeadless**) — in the visual
> style of *Darwinia*. **Gameplay fantasy: _Homeworld × EVE_** — each player is a
> **fleet commander** (RTS direct control) operating a **mobile mothership-base** in
> a persistent, contested sandbox.

---

## Changelog

**v0.12 (this revision) — overview-driven control model**
- **§23 reworked around an overview-driven command model** (EVE-Echoes prior art) as
  the **shared primary scheme for desktop *and* touch**: select/command via the §22.3
  **overview list + smart-select buttons + a smart context action**, routed through the
  existing intent layer (§8.4). The **3D view is camera-first**; **box-select is demoted
  to a desktop convenience.**
- **Touch ambiguity designed out:** camera lives on **two-finger gestures only**, so
  one-finger select-vs-pan never conflicts; tap-and-hold = radial context menu. Full
  gesture/smart-action tables in **`docs/design/touch-controls.md`**.
- **R20 downgraded Med→Low** (decided approach with prior art, not an open UX gamble).
- Updates §2 (Client input), §22.3, §23, R20.

**v0.11 — completeness pass (client engine, UI/radar, navigation, live-ops)**
- **DirectX 12 engine architecture written down (new §11.1):** adapter/feature-level
  selection (**target FL 12_0, Resource Binding Tier 2, SM6.0** — broad reach),
  **triple-buffered** flip-model swap chain + fences, command queues/allocators/lists,
  **descriptor-heap strategy**, root-signature/binding model, resource heaps + upload
  ring, **PSO management**, the **HDR forward + bloom** pass graph, barriers, GPU
  memory budget, UWP suspend/`Trim`, device-removed/DRED, PIX/timestamp profiling.
- **Camera & VFX (new §11.2):** space-RTS camera (orbit/pan/zoom/follow, floating-origin
  rebase, feeds the audio listener) + **GPU-compute additive particle system**.
- **Navigation & travel decided (new §13.12):** **warp + jump-beacon network** —
  sublight combat movement, **interdictable warp** to scanned points, long-haul
  **jump drive + fuel** between beacons (player/territory-owned beacons = chokepoints).
  Fills the biggest gap (an interstellar world with no travel model).
- **Client UI, HUD, radar & accessibility (new §22):** screen inventory, HUD spec,
  **3D bracket overlay + sortable overview list + 2D radar disc** (IFF, range rings),
  scalable/DPI-aware UI, **localization-ready** string table (English at launch),
  accessibility (**scalable UI + audio cues** at launch).
- **Input & controls (new §23):** **mouse+keyboard (primary) + touch (tablets)** RTS
  scheme; commands → server-validated intents; rebindable keymap.
- **Communications (new §24):** chat channels + **in-game mail + offline
  notifications** (needs Mail/Notifications tables — §15).
- **Client platform services (new §25):** settings/config (UWP local storage),
  client logging & **crash/minidump** reporting, MSIX/Store update + version gate.
- **Game-data files & tooling (new §12.6):** text source → cooked binary via
  `datacook`/`datacheck`, **dev hot-reload**; one dataset drives server, client & bots.
- **World clock & live-ops (new §26):** **24/7, rolling restarts, no scheduled
  downtime** (warm-restart SLA); authoritative world clock; event director; live tunables.
- **Risks R20–R22**; §2 decisions, milestones (M1b/M2/M3/M5/M7), §19, glossary updated.

**v0.10 — audio subsystem**
- **Audio added (new §11.3, `NeuronAudio`):** **XAudio2 (2.9)** for playback +
  **X3DAudio** for 3D positional sound, **WAV (PCM 16-bit) only** via a custom
  RIFF/WAVE reader (mirrors the CMO/DDS custom-parser approach). Both are Windows-SDK
  components — fits the MS-only allow-list (§2).
- **Dedicated `NeuronAudio` client library** (sibling to NeuronRender), used only by
  the UWP client; **ERHeadless/bots link no audio**. Adds `NeuronAudioTest` per the
  §16.1 per-project test policy → **14 projects**. Updates §2, §3, §4, §5, §16.1.
- **Four mixer buses** (Master → **Music / Ambient / SFX / UI**): **ambient
  background beds**, **event SFX**, **UI**, and a **musical score**.
- **3D from day one:** X3DAudio listener = scene camera; mono emitters positioned in
  **camera-relative / floating-origin** space (§6.4) — no `int64` reaches audio.
- **Event sounds are client-side feedback** off replicated sim events — **no audio on
  the wire**, no determinism requirement (presentation only).
- **Delivered in M2 (Darwinia look)**; risk **R19** added; glossary + allow-list
  updated.

**v0.9 — gameplay design pass (PvP/PvE depth)**
- **Core fantasy fixed as _Homeworld × EVE_:** RTS-style direct **fleet command**
  from a **mobile mothership-base**, inside a persistent EVE-like contested universe.
  Updates §1 pillars, §13.
- **§13 rewritten from a half-page sketch into a real game-design spec** with
  subsections: 13.1 Player fantasy & fleet command, 13.2 Combat model
  (role + fitting + damage-type counters), 13.3 Progression (hybrid tech-tree +
  fitting + catch-up), 13.4 Player-driven crafting economy, 13.5 World structure
  (tiered security high→low→null), 13.6 Territorial conquest, 13.7 PvE content
  (dynamic faction invasions + procedural anomalies), 13.8 Social/parties &
  ownership, 13.9 New-player onboarding & loss mitigation, 13.10 Retention loop.
- **Base role decided:** capital-class, **disable-not-destroy** (forced retreat at
  low HP + cargo loss; never a one-base-per-player total wipe). §13.1, §13.6.
- **Fleet scale decided:** launch cap **6–12 ships/player**, architecture designed
  to scale entity counts beyond the initial 100-player shard as the game grows.
  Updates App. B and §13.1.
- **App. B re-derived for fleets** — a contested sector is now players × fleet, not
  a flat ~100 entities; adds entity-aggregation / hard interest-culling as the lever.
- **Risk table:** added R15 (gameplay-design depth/retention), R16 (fleet entity
  blow-up vs bandwidth), R17 (newcomer brutality in conquest), R18 (thin social
  glue at small scale). §18.
- **Milestones:** M3 expanded to the real 4X+combat loop; new **M7 (territorial
  conquest, economy, PvE content, onboarding)**. §17.
- **§19 updated** with the new live unknowns (fleet-cap balance, market shard model,
  whether persistent corps must come forward).

**v0.8 — testing policy**
- **Microsoft Native Unit Test policy added (§16):** every project must have a
  corresponding `<project>Test` MSTest project (e.g., `NeuronCoreTest`,
  `NeuronClientTest`, `NeuronRenderTest`, `ERServerTest`, `ERHeadlessTest`). Each new
  feature ships with tests in the matching `<project>Test` project. All test projects
  are included in `EarthRise.sln`.

**v0.7 — engine/netcode review pass**
- **Simulation tick reset to 30 Hz** (snapshot stays 20 Hz). 60 Hz in v0.6 only
  mirrored a scaffold constant; 30 Hz halves per-tick CPU/delta-encoding cost and
  is ample for a command-driven 4X. Fast projectiles sub-step locally. Updates §2,
  §3, §4, §7.2, §9, §17, App. B. `TimerCore.h` constant updated to match.
- **Fixed-step sim loop made real.** The provided `TimerCore.h` is a *variable*-step
  timer (DX `StepTimer` with the fixed path stripped); §7.2/§9 now mandate a true
  accumulator with bounded catch-up (`MaxCatchUpTicksPerFrame`), WinRT-free in core.
- **World coordinates → `int64_t` centered at 0** (was `uint64_t` at 2⁶³). Interest
  grid keyed on a **`SectorId` struct hash**, not a 64-bit Morton code (three 50-bit
  sector indices overflow 64 bits). Position unified to **sector index + sector-local
  float offset**. Updates §6, §7.1.
- **Crypto hardened (§8/§14):** server **static-key pinning/signature** added to the
  ECDH handshake (resists active MITM on the login exchange); **AES-GCM nonce =
  direction‖64-bit packet counter** with **rekey-before-wrap**; **explicit replay
  window** on decrypt; **stateless cookie before ECDH** (handshake-DoS guard);
  connection token widened to 64-bit; **clock sync** and **protocol-version gate**
  added to the connection sequence.
- **Client-side prediction deferred past M1** — interpolation + snap-on-ack for the
  tech slice; predict/reconcile added later only where feel demands it (§8.4, §10.1,
  §17). Cuts scope risk R8.
- **Persistence durability boundary defined (§15):** economy events
  (trades/currency/loot) are **write-through / transactional outbox**; high-frequency
  position is **write-behind** with a stated **RPO**. Warm-restart uses a **binary
  state snapshot + event log since snapshot**, not row-by-row reconstruction.
- **Shaders locked to SM6/DXIL via `dxc`** (not "dxc else fxc" — fxc cannot emit
  SM6). Root signatures authored in HLSL. (§12.4)
- **M1 split into M1a (headless transport) + M1b (client render)** to de-risk
  reliable-UDP/crypto before a DX12 pipeline is in the loop. Per-milestone numeric
  perf gates added; per-client bandwidth budget estimated **now** (App. B).
- **Folded-in engineering specifics:** ECS handle generation bits + deterministic
  iteration order; frame-arena / pool (`pmr`) allocators in the tick hot path;
  input-log **record/replay determinism harness**; snapshot-encoding **job pool**;
  IOCP **per-connection affinity**; login **rate-limit/lockout + server pepper**.
- New **§21 Observability & Telemetry**; risk table updated (R1, R6, R12–R14).

**v0.6** — Sim tick set to 60 Hz to match `TimerCore.h`; NeuronCore scaffolded as a
Windows Store static library, added to the solution.
**v0.5** — Shaders precompiled to embedded headers; custom username/password login
locked; App→DB auth SQL login → managed identity; SQL edition direction confirmed.
**v0.4** — SQL external (not containerized), self-hosted → Azure SQL; real login +
CNG crypto; dev Docker Desktop / prod Kubernetes; 20 Hz.
**v0.3** — ERServer/ERHeadless; metres; CMO; monospace fonts; STL; zoned PvP;
SQLite→SQL Server.
**v0.2** — NeuronCore/NeuronClient/NeuronRender/NeuronHeadless; 3D vs 2D;
DirectXMath; MSBuild; Server Core container; PvE+PvP; user meshes.

---

## 0. How to read this document

- **🔒 Locked** — decided (§2). **💡 Proposed** — my default. **❓ Open** — needs
  input (§19).
- Custom C++23 throughout. The only non-engine code we rely on are **Microsoft
  platform components**: `cppwinrt`, **DirectXMath**, **DirectX 12**, **Winsock**,
  **ODBC / SQL Server**, and **Windows CNG** (crypto). No third-party libraries.

---

## 1. Vision & Design Pillars

**EarthRise** is a persistent, single-shard space MMO whose fantasy is
**_Homeworld × EVE_**: each player is a **fleet commander** who directs a small
fleet of ships (RTS-style) from one **mobile mothership-base** through a single
contiguous, contested universe — explore, harvest, craft, trade, expand, and wage
**PvE _and_ territorial PvP** in a real-time 4X loop shared by ~100 concurrent
players at launch and designed to grow well past that.

**Pillars:**
1. **Fleet command from a mobile home.** You don't fly one ship — you command a
   fleet from a mobile mothership-base (a slow, powerful **capital** that is
   *disabled, not destroyed* — §13.1). Base is a unit, not a tile.
2. **A persistent, contested sandbox.** One universe, one shard, signed `int64_t`
   coordinates; **tiered security (high→low→null, §13.5)** with player-claimable
   **territory** in nullsec (§13.6). Risk scales with reward.
3. **Depth through fitting & economy, not just numbers.** Role + module **fitting**
   with **damage-type counters** (§13.2); a **player-driven crafting economy**
   where destruction creates demand (§13.4); hybrid tech-tree progression (§13.3).
4. **A living world.** Dynamic NPC **faction invasions** and procedural
   **anomalies/expeditions** give PvE a pulse (§13.7).
5. **The Darwinia look** — dark void, neon glow, bloom, additive particles,
   minimalist bitmap-font HUD.
6. **Server-authoritative**, on a **custom engine using Microsoft platform tech
   only**, **testable by construction** via headless clients/bots.

---

## 2. Locked Decisions & Constraints

### 🔒 Decisions

| Topic | Decision |
| --- | --- |
| Server OS / deploy | Windows only; **ERServer** in a **Windows Server Core container**. Winsock + IOCP. |
| Build system | **MSBuild** (`EarthRise.sln`); UWP → MSIX. |
| Network transport | Custom **reliable UDP**, **encrypted** (CNG handshake, **server-key pinned**). |
| **Database** | **SQL Server over the network — not containerized.** Self-hosted (Developer/Standard) now → **Azure SQL** later. ODBC Driver 18. |
| **Authentication** | **Real login at launch — custom username/password**; CNG PBKDF2 hashing (+ server pepper, rate-limit/lockout); encrypted handshake. |
| **App→DB auth** | **SQL login now → managed identity / Entra ID on Azure SQL.** |
| **Shaders** | Precompiled to embedded headers; **SM6 / DXIL via `dxc`** (DXCCompile). Variable Name `g_p%(Filename)`, Output `$(ProjectDir)CompiledShaders\%(Filename).h`. |
| Math | **DirectXMath**-based. |
| Coordinate scale / type | **1 unit = 1 metre; `int64_t` per axis, signed, origin (0,0,0).** |
| Core / client / render libs | **NeuronCore**, **NeuronClient** (render-agnostic), **NeuronRender** (DX12). |
| Headless host / bots | **ERHeadless**. |
| Client app model | **UWP + CoreWindow + DX12** (Store reach). Loopback/sandbox toil accepted; see R1. |
| Client rendering | **3D Scene** and **2D Canvas (UI)** are separate subsystems. |
| **Audio** | **XAudio2 (2.9)** playback + **X3DAudio** 3D sound; **WAV / PCM-16 only** (custom RIFF reader). Dedicated **`NeuronAudio`** lib (client-only; ERHeadless has none). Buses: **Master→Music/Ambient/SFX/UI**. 3D from day one (§11.3). |
| Client prediction | **Deferred past M1** — interpolation + snap-on-ack first; predict/reconcile later. |
| **Core fantasy** | **_Homeworld × EVE_** — RTS **fleet command** from a **mobile mothership-base** in a persistent contested sandbox (§13.1). |
| **Combat** | **PvE + territorial PvP**; **role + fitting + damage-type counters** (§13.2); **loot-on-kill** (ships); **base = capital, disable-not-destroy** (§13.1). |
| **World / PvP zones** | **Tiered security high→low→null**; **claimable nullsec territory** (§13.5–13.6). |
| **Progression** | **Hybrid tech-tree + fitting**, with catch-up flattening (§13.3). |
| **Economy** | **Player-driven crafting** (raw→refine→components→ships) + regional markets + sinks (§13.4). |
| **PvE anchor** | **Dynamic faction invasions** + **procedural anomalies/expeditions** (§13.7). |
| **Social** | **Light parties/fleets** at launch; **individual** asset/territory ownership; persistent corps tracked as first expansion (§13.8, §19). |
| **Fleet scale** | **6–12 ships/player** at launch (💡 8), data-driven cap raised as scale grows (§13.1, App. B). |
| Meshes / Fonts | **CMO** meshes / **fixed-grid monospace** bitmap fonts (you provide both). |
| STL | **Allowed** (but tick hot path uses arena/pool/`pmr` allocators — §7.2). |
| Sim tick / snapshot | **30 Hz sim (~33.3 ms) / 20 Hz snapshot**; true fixed-step accumulator. |
| Persistence durability | **Economy = write-through / outbox; position = write-behind (RPO bounded).** |
| Dev / Prod | **Dev: Docker Desktop. Prod: Kubernetes** (Windows nodes + UDP LB). |
| **Navigation / travel** | **Warp + jump-beacon network**: sublight combat move; **interdictable warp** to scanned points; long-haul **jump drive + fuel** between beacons (§13.12). |
| **Renderer target** | **D3D12 FL 12_0, Resource Binding Tier 2, SM6.0**; **triple-buffered** flip-model; **HDR forward + bloom**, no MSAA; GPU-compute particles (§11.1–11.2). |
| **Client input** | **Overview-driven command model** (EVE-Echoes-style; shared by desktop & touch): select/command via overview list + smart-select + smart action; **mouse+keyboard + touch**; box-select demoted to a desktop shortcut; commands → **server-validated intents** (§23). |
| **Tactical UI / radar** | **3D bracket overlay + sortable overview list + 2D radar disc** (IFF, range rings) (§22). |
| **Communications** | **Chat channels + in-game mail + offline notifications** (§24). |
| **Server uptime** | **24/7, rolling restarts via warm-restart; no scheduled downtime** (§26). |
| **Localization** | **English at launch, localization-ready** (string table; Unicode-capable font) (§22). |
| **Accessibility** | **Scalable UI/HUD + audio cues** at launch (§22). |
| First milestone | Networked tech slice, split **M1a (headless) → M1b (client)** (§17). |

### 🔒 Hard constraints (brief)

C++23 (MSVC, `/std:c++latest`); client **UWP + C++/WinRT + DX12**; one open world,
**`int64_t` x/y/z**; one **movable base**/player; ~**100 players**; textures
**`.dds`**; fonts **bitmap** (you provide).

### Allow-list (Microsoft platform components only)

| Component | Used by | Notes |
| --- | --- | --- |
| C++/WinRT | UWP client | App model / WinRT interop |
| DirectXMath | all | Header-only (Windows SDK) |
| DirectX 12 / DXGI | UWP client | Rendering |
| Winsock | all | UDP sockets |
| ODBC (Driver 18) + SQL Server | ERServer | `sql.h`/`odbc32.lib` (SDK); driver installed in the container |
| Windows CNG (`bcrypt.h`) | ERServer + client | ECDH, **ECDSA (server-key sign)**, AES-GCM, PBKDF2 hashing |
| **XAudio2 (2.9)** (`xaudio2.h`) | NeuronAudio (client) | Low-level audio playback / voice graph (SDK; UWP-supported) |
| **X3DAudio** (`x3daudio.h`) | NeuronAudio (client) | 3D positional audio (DSP settings for XAudio2 voices) |
| MSBuild HLSL Compiler (`dxc`) | build-time | HLSL → embedded **SM6/DXIL** bytecode headers |
| STL | all | 🔒 allowed |

---

## 3. Technology Stack

| Layer | Choice |
| --- | --- |
| Language / build | C++23, MSVC; **MSBuild**; UWP → MSIX |
| Math | DirectXMath (`XMVECTOR`/`XMMATRIX`; store `XMFLOAT*`) |
| Server | **ERServer** — Win32 console in a Windows Server Core container; Winsock UDP + IOCP; **30 Hz** fixed-step sim / **20 Hz** snapshot |
| Database | **SQL Server over the network** (self-hosted → Azure SQL); ODBC Driver 18 |
| Crypto | **Windows CNG** — ECDH handshake (**server-key pinned**), AES-GCM AEAD, PBKDF2 password hashing |
| Transport | Custom **encrypted** reliable-UDP (§8) |
| Client app | `CoreApplication` + `IFrameworkView` (CoreWindow), C++/WinRT, no XAML |
| Rendering | DX12 + DXGI flip-model; **Scene (3D)** + **Canvas (2D)** split |
| Shaders | HLSL → **embedded SM6/DXIL headers** via `dxc` (var `g_p%(Filename)`, out `CompiledShaders\%(Filename).h`); root sigs in HLSL; no runtime HLSL on UWP |
| **Audio** | **NeuronAudio** (client-only lib): **XAudio2 (2.9)** voice graph + **X3DAudio** 3D; **WAV/PCM-16** via custom RIFF reader; buses Master→Music/Ambient/SFX/UI (§11.3) |
| Meshes / fonts | CMO (custom parser) / fixed-grid monospace atlas |
| Headless/bots | **ERHeadless** — many NeuronClient sessions, render-free |
| Tests | **Microsoft Native Unit Test** (`<project>Test`) per project (§16.1) + custom headless assert runner + ERHeadless multi-client harness + **record/replay determinism harness** |

---

## 4. High-Level Architecture

```
                         EarthRise — single shard
  ┌────────────────────────────────┐        ┌──────────────────────────────────┐
  │  UWP CLIENT (EarthRise.Client)  │        │  ERServer (Win32 console)         │
  │  app shell: IFrameworkView      │        │  in Windows Server Core container │
  │  ┌───────────────────────────┐  │encrypt.│  ┌────────────────────────────┐  │
  │  │ NeuronRender (DX12)        │  │reliable│  │ Net (Winsock UDP+IOCP):    │  │
  │  │  ├ SceneRenderer  (3D)     │  │  UDP   │  │ CNG handshake (pinned key),│  │
  │  │  └ CanvasRenderer (2D UI)  │  │◀──────▶│  │ reliability, frag, AEAD,   │  │
  │  └───────────┬───────────────┘  │packets │  │ acks, auth/session         │  │
  │  ┌───────────▼───────────────┐  │snap/   │  └─────────────┬──────────────┘  │
  │  │ NeuronClient (lib)         │  │deltas  │  ┌─────────────▼──────────────┐  │
  │  │  session/replica/interp/   │◀─┼───────▶│  │ Simulation (30 Hz fixed,   │  │
  │  │  controller(human)         │  │        │  │ auth. ECS, PvE AI, PvP)    │  │
  │  └────────────────────────────┘  │        │  └─────────────┬──────────────┘  │
  └────────────────────────────────┘        │  ┌─────────────▼──────────────┐  │
                 ▲                            │  │ Persistence (ODBC, write-  │  │
  ┌──────────────┴─────────────────┐ encrypt.│  │ through outbox + write-    │  │
  │ ERHeadless (exe)               │reliable │  │ behind, accounts) ─────┐   │  │
  │  N× NeuronClient sessions,     │  UDP    │  └────────────────────────┼───┘  │
  │  bot / scripted (no rendering) │◀────────▶                           │      │
  └──────────────┬─────────────────┘         └───────────────────────────┼──────┘
                 ▼                                       TCP 1433 (ODBC,  │
   ┌──────────────────────────────────────┐              Encrypt=yes)    ▼
   │ NeuronCore (linked by ALL):          │  ┌───────────────────────────────────┐
   │ math(DirectXMath)·ECS·world(int64)·  │  │ Microsoft SQL Server (EXTERNAL,   │
   │ sectors·net protocol·serde·sim rules │  │ NETWORK SERVICE — not a container)│
   └──────────────────────────────────────┘  │ self-hosted now → Azure SQL later │
                                              └───────────────────────────────────┘
```

**Targets:** **NeuronCore** (lib, all) · **NeuronClient** (lib → UWP app +
ERHeadless) · **NeuronRender** (lib, UWP/DX12 → UWP app) · **NeuronAudio** (lib,
XAudio2/X3DAudio → UWP app **only**; ERHeadless links none) · **ERServer** (exe) ·
**EarthRise.Client** (UWP/MSIX) · **ERHeadless** (exe) · **NeuronTools** (exes).

---

## 5. Repository Layout

```
/EarthRiseGame
├── masterplan.md   ·   EarthRise.sln   ·   docs/   ·   db/ (SQL schema+migrations)
├── NeuronCore/    math/ ecs/ world/ net/ serde/ sim/ platform/
├── NeuronClient/  session/ replica/ interp/ control/ bots/   (predict/ added later)
├── NeuronRender/  gfx/ scene/(3D) canvas/(2D) assets/(DDS,font,CMO)        [UWP]
├── NeuronAudio/   engine/(XAudio2) spatial/(X3DAudio) wav/(RIFF reader) mixer/(buses) [client]
├── ERServer/      netio/ simloop/ ai/(PvE) interest/ auth/ persist/(ODBC)
├── EarthRise.Client/  app/ ui/(HUD on CanvasRenderer)                      [UWP/MSIX]
├── ERHeadless/    host/ (N sessions; load + integration + replay tests)    [no audio]
├── NeuronTools/   meshcook/ ddscheck/ fontpack/ wavcheck/ testrunner/
├── assets/        .dds · monospace font bitmaps · your .cmo meshes · audio/*.wav (PCM-16)
├── shaders/       .hlsl sources → dxc (SM6/DXIL) → CompiledShaders/*.h (embedded)
└── deploy/        ERServer Dockerfile (Server Core) · k8s manifests · dev scripts
```
> Generated `CompiledShaders/` headers live under each project that compiles shaders
> (per `$(ProjectDir)`), so they're picked up by `#include` at build time.

---

## 6. Coordinate System & World Model

### 6.1 Absolute position — `int64_t` per axis, **1 unit = 1 metre** 🔒
```cpp
struct WorldPos { int64_t x, y, z; };   // absolute metres, signed, origin at 0
```
- Extent ≈ **±2⁶³ m ≈ ±975 light-years** per axis — far beyond any gameplay need;
  the meaningful constraint is sector size vs. float precision (§6.3), not range.
- **Signed, origin-centred:** symmetric spawns need no 2⁶³ bias; relative deltas are a
  plain subtraction (§6.2).
- **Sub-metre / smooth motion:** position is carried as **sector index + a
  sector-local `float` offset** (§6.3). The offset integrates velocity each tick and
  **rebases** into the integer sector when it leaves the sector. One representation
  serves both the authoritative sim and floating-origin rendering (§6.4).

### 6.2 Relative math
```cpp
inline int64_t axisDelta(int64_t a, int64_t b) { return a - b; } // valid while |a-b| < 2^63
```
Relative vectors use DirectXMath `float`; never raw `int64_t` to the GPU. Pairs are
only ever differenced within interest range, so the subtraction cannot overflow.

### 6.3 Sectors & interest grid
`sector = pos >> S` (arithmetic shift), `local = pos - (sector << S)`. **💡** default
**S = 14 → ~16 km sectors** (tunable to sensor range). Float precision inside a sector
is 16 384 m / 2²⁴ ≈ **1 mm** — ample.

The interest map is keyed on a **`SectorId` struct**, *not* a packed Morton code: at
S = 14 each sector index is ~50 bits, so a 3-axis Morton key would need 150 bits and
cannot fit a `uint64_t`.
```cpp
struct SectorId { int64_t x, y, z; bool operator==(const SectorId&) const = default; };
struct SectorHash { size_t operator()(const SectorId&) const noexcept; }; // mix of 3 axes
```
Players subscribe to nearby sectors; the server streams only those entities (§8.4).
(If spatial-locality ordering is ever required, add a **128-bit** Morton key — never
a 64-bit one.)

### 6.4 Floating-origin rendering
Per frame, pick a render origin (camera sector corner); upload entities as
camera-relative `float3` metres; **rebase** when the camera travels far. Shares the
sector-offset representation from §6.1, so no separate render-space conversion path.

---

## 7. NeuronCore

### 7.1 Math — DirectXMath 🔒
Compute in `XMVECTOR`/`XMMATRIX`; store `XMFLOAT*` for tight ECS packing. **💡**
row-major, row-vector, right-handed (`*RH`). A `WorldPos` fixed-point layer bridges
`int64` sector ↔ sector-local `float`.

### 7.2 Other subsystems
- **ECS** (custom, data-oriented): **32-bit handles = index + generation bits**
  (stale-handle safe), packed archetypes, span systems; **deterministic system &
  iteration order** (client and server must step identically). Identical client/server
  code.
- **Allocators:** the 30 Hz tick hot path (ECS, packet assembly, snapshot encoding)
  uses a **per-frame arena + pools** (`std::pmr` monotonic/pool resources). STL is
  allowed but must not allocate from the global heap inside the tick.
- **Serialization:** versioned binary; bit-packing for the wire; same primitives for
  warm-restart snapshots; carries a **protocol version** for the handshake gate (§8.5).
- **Time:** fixed sim step **🔒 30 Hz (~33.3 ms)** via a **real accumulator** — the
  loop accumulates real elapsed time and runs whole fixed steps, clamped to
  `MaxCatchUpTicksPerFrame`, carrying the remainder. (The provided `TimerCore.h` is a
  *variable*-step timer; the server loop must not advance by raw wall-clock delta.)
  Ticks are canonical; **snapshots 20 Hz**. Core timing stays **WinRT-free**.
- **Shared sim rules:** pure functions for movement, build costs, yields, combat
  (PvE+PvP) — used by client interpolation/validation and server authority.

---

## 8. Networking — Encrypted Reliable UDP

### 8.1 Transport (verified vs current MS docs)
- **ERServer / ERHeadless:** Winsock UDP + IOCP.
- **UWP client:** Winsock (or `DatagramSocket`) behind a thin `ISocket`; **💡
  default Winsock**.
- **Capabilities:** `internetClient` + `internetClientServer`/
  `privateNetworkClientServer`.
- **⚠️ Loopback isolation:** UWP is blocked from loopback by default; for local
  testing add `CheckNetIsolation.exe LoopbackExempt -a -n=<PFN>` (VS auto-adds in
  debug; test without it before Store). ERHeadless (Win32) is exempt.

### 8.2 Channels
`Unreliable` (snapshots) · `ReliableOrdered` (commands/chat/events) ·
`ReliableUnordered` (notifications) · `Bulk` (fragmented world sync). Per-channel
16-bit sequences are for reliability/ordering only — **not** the AEAD nonce (§8.3).

### 8.3 Reliability & encryption
**Reliability:** 16-bit per-channel sequences; **ack + 32-bit ack-bitfield**; RTT/RTO
retransmit; dup detection; **fragmentation/reassembly** (~1200 B safe payload);
**64-bit connection token** (anti-spoof); keepalive/timeout; congestion backoff.

**Encryption (required):**
- Handshake performs a **CNG ECDH** key agreement; the server's ephemeral key is
  **signed by a pinned static server key (ECDSA)** and the client verifies it, so an
  active **MITM cannot sit in the login exchange** (§14). The client ships with the
  server's static public key.
- Packets then use **AES-GCM (CNG)** AEAD. **Nonce = direction-bit ‖ monotonic 64-bit
  packet counter** (never random, never reused under a key); **rekey before the
  counter can wrap**. This 64-bit packet number is separate from the 16-bit channel
  sequences.
- **Replay protection:** a sliding **replay window** on decrypt rejects stale/duplicate
  packet numbers (independent of the reliability ack bitfield).
- Protects the **login exchange** (§14) and, by default, **all reliable traffic**.

### 8.4 State replication
Per tick, each player gets a snapshot of **only their subscribed sectors**,
**delta-compressed vs the last acked baseline**. Clients **interpolate** remote
entities (~100 ms back). **Own-unit prediction/reconciliation is deferred past M1**
(§10.1) — until then own units are server-confirmed and **snap-corrected on ack**.
Input is **intents/commands**; the server validates everything (speed/rate checks).

### 8.5 Connection sequence
1. **Stateless cookie:** client → server hello; server replies with a token derived
   from client addr + secret (no per-connection state, no crypto yet) — a
   **handshake-DoS guard** so ECDH is only spent on cookie-returning clients.
2. **Protocol-version gate:** client presents build/protocol version; mismatches are
   rejected with a clear reason before any state is allocated.
3. **CNG ECDH** handshake with **server-key signature verification** → shared key.
4. **Clock sync:** RTT/offset estimation (timestamp echo) so interpolation delay and
   later prediction share a common clock.
5. **Login** (§14, custom username/password) over the encrypted channel → verify vs
   SQL → expiring **session token**.
6. Initial world sync (`Bulk`) → enter the tick/snapshot loop.

---

## 9. ERServer
- Single Win32 console exe (one shard, ~100 players) in a **Windows Server Core
  container** (§20); **stateless** (durable state in SQL Server).
- **Threading (💡):** IOCP net threads → decode/reliability/decrypt → enqueue; a
  **single-threaded 30 Hz simulation** owns state; a **persistence thread** does the
  outbox + write-behind via ODBC. MPSC queues. Reliability/decrypt state is
  **per-connection-affinitised** (or lock-free per-conn queues) so IOCP threads never
  race on a connection's sequence/nonce/reassembly state.
- **Tick:** commands → systems (movement, harvest, build, **PvE AI**, **PvP**) →
  advance fixed step → per-player interest snapshots → net → periodic persistence
  batch.
- **Snapshot encoding** (interest + delta for ~100 players) runs over a **read-only
  job pool** against a frozen post-tick state — the first scaling lever if the single
  sim thread saturates (M4). M1a may encode inline.
- **PvE NPCs** are server ECS entities (`ERServer/ai/`), distinct from *bots*.
- DB is **out of the tick hot path** — SQL latency never stalls the sim.

---

## 10. Clients

### 10.1 NeuronClient (render-agnostic) 🔒
No rendering / no UWP dep; links into the UWP app **and** ERHeadless. Modules:
**session** (encrypted reliable-UDP, login, command queue), **replica** (entity
mirror), **interp** (interpolation + snap-on-ack), **control** (`IClientController`
→ intents: human / bot / scripted). A **`predict/`** module
(prediction/reconciliation) is added **post-M1** only where input feel requires it
(fast direct control); slow command-driven units stay interpolation-only.

### 10.2 EarthRise.Client (UWP)
C++/WinRT shell (`IFrameworkView`+CoreWindow, no XAML). Loop: input → human
controller → step NeuronClient → hand state to NeuronRender. DX12 swap chain via
`CreateSwapChainForCoreWindow` (pass the **command queue**;
`winrt::get_unknown(window)`); `winrt::com_ptr`/`check_hresult`; suspend/resume.

### 10.3 ERHeadless (parallel clients & bots) 🔒
Win32 host running **many NeuronClient sessions in one process** (each own UDP
port), bot/scripted-driven, **no rendering** → integration tests, **record/replay
determinism** runs, load tests (~100+ bots, §17 M4), and in-world bots.
> **Bots ≠ PvE NPCs.** Bots are *client* sessions; PvE NPCs are *server* AI.

---

## 11. Rendering — Scene (3D) vs Canvas (2D), Darwinia Look
Separate subsystems 🔒 (own modules/PSOs/command lists; composited last).
- **SceneRenderer (3D):** HDR scene (low-poly + bright emissive silhouettes,
  **instanced** ships) → bright-pass + Gaussian **bloom** (additive) → additive
  **particles** → tone-map (+ optional scanline/vignette/grain).
- **CanvasRenderer (2D UI):** immediate-mode orthographic pass — batched quads /
  monospace bitmap text / lines / rects; HUD & menus build on it, fully decoupled.

Shaders are precompiled into embedded SM6/DXIL byte-array headers (no runtime HLSL on
UWP); see §12.4.

### 11.1 DirectX 12 engine architecture (NeuronRender) 🔒
The renderer targets **broad reach**: **D3D12 feature level 12_0, Resource Binding
Tier 2, Shader Model 6.0, Root Signature 1.1** — runs on most DX12 GPUs incl.
integrated, ample for the cheap low-poly Darwinia look. No mesh shaders / ray tracing.

- **Device & adapter:** DXGI factory enumerates adapters; pick the highest-performance
  hardware adapter that meets FL 12_0 (`CheckFeatureSupport` for binding tier, SM6,
  RS 1.1); **WARP** fallback for dev/CI. **Debug layer + GPU-based validation** in dev
  builds; **DRED** (Device Removed Extended Data) for device-removal diagnostics.
- **Frames in flight = 3 (triple-buffered).** Swap chain via
  `CreateSwapChainForCoreWindow`, **`DXGI_SWAP_EFFECT_FLIP_DISCARD`**, LDR backbuffer
  (`R8G8B8A8_UNORM` or `R10G10B10A2`). Per-frame: **command allocator + a fence
  value**; the CPU waits on the frame fence before reusing a frame's allocators/upload
  ring. Optional **waitable swap chain** for low-latency frame pacing.
- **Command model:** one **DIRECT** queue (graphics; compute runs here at FL 12_0) +
  one **COPY** queue for async resource uploads. Command lists recorded per frame
  (single-threaded record at M1b; parallel record a later lever).
- **Descriptor heaps:**
  - One large **shader-visible CBV/SRV/UAV heap**, **ring-sub-allocated per frame** for
    per-draw descriptors; a non-shader-visible **staging heap** holds persistent
    descriptors copied in via `CopyDescriptors`.
  - A small **sampler heap** (static sampler set; most samplers as static samplers in
    the root signature).
  - Non-shader-visible **RTV** and **DSV** heaps.
  - **Binding model (Tier 2, not full bindless):** root signatures authored in HLSL
    (§12.4) — **root constants** (per-draw ids) + **root CBVs** (per-frame/per-view) +
    **descriptor tables** for material textures; **per-instance data via a structured
    buffer (SRV)** drives instanced ship draws.
- **Resources & memory:**
  - **DEFAULT** heap for static GPU data (meshes, DDS textures), uploaded from **UPLOAD**
    staging on the COPY queue with fence sync.
  - Dynamic per-frame constants from a **persistently-mapped UPLOAD ring buffer**.
  - **Manual resource-state tracking & barriers** (RT↔SRV, etc.), batched to minimize
    transitions.
  - **GPU memory budget** via `IDXGIAdapter3::QueryVideoMemoryInfo`; residency-aware
    texture streaming as fleets/sectors stream in.
- **PSO management:** PSOs built at load from the **embedded DXIL** (§12.4), **cached by
  hash**; optional `ID3D12PipelineLibrary` disk cache. Shared **Scene** and **Canvas**
  root signatures.
- **Pass graph (HDR forward — fits the Darwinia look):**
  1. **Scene forward** → HDR target **`R16G16B16A16_FLOAT`** (instanced emissive ships;
     depth `D32_FLOAT`). No MSAA (low-poly + bloom; optional cheap post-AA in tonemap).
  2. **Bright-pass** extract → downsample → **Gaussian bloom** (ping-pong blur).
  3. **Additive particles** (§11.2) into HDR.
  4. **Tone-map + post** (scanline / vignette / grain) → LDR backbuffer.
  5. **Canvas (2D UI)** orthographic pass → backbuffer (post-tonemap, no bloom).
  6. **Present.**
- **Resize / suspend / loss:** fence-flush then recreate swap-chain buffers on resize;
  handle DPI/orientation via `DisplayInformation`; on UWP **suspend**, call
  `IDXGIDevice3::Trim()` to release memory, rebuild on resume; on
  `DXGI_ERROR_DEVICE_REMOVED`, recreate the device (DRED logs the cause).
- **Profiling:** **PIX markers** + a **timestamp query heap** feed the render
  frame-time perf gate (§16.3, App. B). Render runs on its own thread, decoupled from
  the sim/snapshot cadence.

### 11.2 Camera & VFX / particles 🔒
- **Camera (space-RTS):** right-handed perspective; **orbit / pan / zoom / follow**,
  edge-scroll + drag, **center-on-selection**, smooth zoom from ship-detail to fleet
  tactical range. The camera **is** the **floating-origin** render origin (§6.4) —
  panning far triggers a rebase — and supplies the **X3DAudio listener** (§11.3). Touch
  adds pinch-zoom / two-finger rotate (§23).
- **VFX — GPU-compute particles:** particle pools in structured buffers (UAV);
  **spawn/update in compute shaders** (FL 12_0), drawn as **additive instanced
  billboards** into the HDR target. Categories: thrusters, weapon tracers/muzzle,
  impacts, **explosions**, **warp/jump** effects, mining beams. CPU emits spawn
  requests only; the GPU simulates. A **per-frame particle budget** with distance LOD
  keeps big fights within frame time (ties to App. B / R16). Additive throughout for
  the neon Darwinia glow.

### 11.3 Audio — XAudio2 / X3DAudio (NeuronAudio) 🔒
A dedicated **client-only** `NeuronAudio` library (sibling to NeuronRender). It links
**NeuronCore** (math/types) but **not** NeuronRender; the UWP client owns both and
feeds the audio listener from the **same camera** it renders with. **ERHeadless/bots
link no audio** (presentation only). MS-platform components only — no third-party.

- **Playback — XAudio2 (2.9) 🔒** (`xaudio2.h`, UWP-supported): a mastering voice →
  **four submix buses 🔒 — Music · Ambient · SFX · UI** → pooled source voices.
  Per-bus + master volume. Short **event SFX** are loaded fully into memory; long
  **ambient beds** and the **music score** are **streamed** via a buffer-queue
  `IXAudio2VoiceCallback`.
- **Format — WAV / PCM-16 only 🔒.** A **custom RIFF/WAVE reader** (in `NeuronAudio/
  wav/`) parses `RIFF`/`fmt `/`data` chunks → `WAVEFORMATEX` + PCM samples. **No
  MP3/OGG/ADPCM** (keeps it MS-only and simple, mirroring the custom CMO/DDS
  parsers). **Mono** sources for 3D-positioned emitters (X3DAudio pans mono);
  **stereo** for music/ambient/UI. A `wavcheck` tool validates assets at build time.
- **3D positional — X3DAudio 🔒 (from day one)** (`x3daudio.h`): the
  `X3DAUDIO_LISTENER` is the scene **camera** (position/orientation/velocity);
  each positional sound is an `X3DAUDIO_EMITTER`. Per audio-update, compute the
  output matrix + **Doppler** + distance LPF and apply via `SetOutputMatrix` /
  `SetFrequencyRatio` / filter on the source voice. Emitter & listener positions are
  **camera-relative / floating-origin** (§6.4) — **no `int64` reaches audio**, same
  as the GPU rule (R2).
- **Content categories (the four buses):**
  - **Ambient background** — looping environmental beds (space drones, nebula hum,
    proximity-to-hazard ambience), cross-faded by region/security tier (§13.5).
  - **Event SFX** — gameplay feedback: weapons fire/impacts, shield/armor/hull hits,
    explosions, thrusters, mining, **build-complete**, loot pickup, alerts/warnings
    (e.g. base low-hull → retreat, §13.1).
  - **UI** — Canvas/HUD interaction sounds (2D, non-positional).
  - **Music** — adaptive/looping score (streamed).
- **Event sounds are client-side feedback** triggered by **replicated sim events /
  state changes** (NeuronClient replica/interp), not by the server sending audio:
  **no audio on the wire**, and **no determinism requirement** (unlike the sim).
- **Lifetime/threading:** XAudio2 runs its own audio thread; the client submits
  buffers and updates 3D params from the game/render thread at a fixed audio cadence.
  On UWP **suspend/resume**, stop/restart the engine and release/reacquire voices.

---

## 12. Asset Pipeline
- **Textures `.dds`:** custom parser (`DDS_HEADER`[+`DXT10`]) → `DXGI_FORMAT`,
  BC1–BC7 + mips. (VS Image Content Pipeline also emits `.dds`.)
- **Fonts (fixed-grid monospace) 🔒:** uniform grid; codepoint → cell; UVs computed;
  no metrics file. Config: `cols, rows, firstCodepoint, cellPx`.
- **Meshes (CMO) 🔒:** authored via the **VS Mesh Content Pipeline** (FBX/OBJ/DAE →
  `.cmo`). Direct3D has **no built-in model loader** and DirectXTK is barred, so
  NeuronRender ships a **custom CMO reader** (materials + ≤8 texture slots [diffuse
  = DDS], vertex buffers [pos/normal/tangent/color/uv], optional skinning, 16-bit
  indices, submeshes, optional skeleton/animation). `meshcook` repacks for
  instancing.

### 12.4 Shaders — precompiled, embedded as headers 🔒
`shaders/*.hlsl` are built to **SM6/DXIL via `dxc`** (the **DXCCompile** MSBuild item,
**not** the legacy `fxc`/`FXCompile` task — fxc tops out at SM5.1 and cannot emit
DXIL) with:
- **Variable Name** → `g_p%(Filename)`
- **Header File Output** → `$(ProjectDir)CompiledShaders\%(Filename).h`
- **Root signatures authored in HLSL** (`[RootSignature(...)]`), versioned with the
  embedded bytecode.

Each shader becomes a generated header declaring a bytecode array
`const BYTE g_p<Filename>[]`, `#include`d directly to build PSOs — **no runtime
shader file I/O** (ideal for UWP). Usage:
```cpp
#include "CompiledShaders/SceneVS.h"
#include "CompiledShaders/ScenePS.h"
psoDesc.VS = { g_pSceneVS, sizeof(g_pSceneVS) };
psoDesc.PS = { g_pScenePS, sizeof(g_pScenePS) };
```

### 12.5 Audio assets — WAV / PCM-16 🔒
- **`assets/audio/*.wav`** — **PCM 16-bit** only (mono for 3D emitters, stereo for
  music/ambient/UI). Parsed at load by the `NeuronAudio` custom RIFF reader (§11.3);
  no runtime decode of compressed formats.
- **`wavcheck` (NeuronTools):** build-time validation — confirms RIFF/WAVE + PCM-16,
  flags non-PCM/compressed files, warns if a 3D-tagged cue is stereo (X3DAudio needs
  mono). Long streamed assets (music/ambient) are loaded incrementally, not embedded.

### 12.6 Game-data files & tooling 🔒
The **catalog/balance boundary** (§15) keeps balance out of SQL and code. Game data —
**item/hull/module stats, crafting recipes, the research tree, audio cue catalog, the
jump-beacon graph, region/security layout, anomaly & invasion definitions** — is
authored as **text source** and cooked to the **versioned binary serde** (§7.2):

- **`datacook` (NeuronTools):** text source → packed binary, loaded by **NeuronCore**
  so **one dataset drives the server sim, the client display, and bots** (no drift).
- **`datacheck` (NeuronTools):** referential-integrity validation in CI — item ids
  exist, recipe inputs/outputs resolve, **research prereqs are acyclic**, cue→clip
  honours the *3D ⇒ mono* rule (§11.3), every beacon links to a valid region.
- **Dev hot-reload:** reload game data into a running dev ERServer / client for fast
  iteration; in prod, tunables reload as a **live-ops** lever (§26) without a redeploy.
- Game data is **versioned with the build** and gated by the protocol version (§8.5),
  so client and server always agree on rules.

---

## 13. Gameplay Systems (4X) — Fleet Command, PvE & Territorial PvP

> **This section is the game design.** v0.8 had only a half-page sketch; v0.9
> expands it after a design pass. Decisions below marked 🔒 were chosen in the v0.9
> discussion; 💡 are defaults to validate; ❓ are tracked open questions (§19).
>
> **Companion design docs** (`docs/design/`, tuned against ERHeadless bots):
> [`combat-balance.md`](docs/design/combat-balance.md) (ship-role spreadsheet,
> damage model, fitting), [`tech-tree.md`](docs/design/tech-tree.md) (research
> progression), [`economy-crafting.md`](docs/design/economy-crafting.md)
> (resource/crafting dependency graph).

### 13.0 The 4X loop, restated
- **eXplore** — sensor/fog range; scan **anomalies/expeditions** (§13.7), discover
  resources, NPCs, players, and territory objectives.
- **eXpand** — relocate the **mobile base**, project sensor/build range; in nullsec,
  **claim and hold territory** (§13.6); later, outposts/structures.
- **eXploit** — harvest nodes → **refine → components → ships/modules** via the
  player crafting economy (§13.4) and the base's build queue.
- **eXterminate** — **fleet-vs-fleet** combat (§13.2): PvE faction invasions &
  site-clearing (§13.7) and territorial PvP (§13.6).

### 13.1 Player fantasy & fleet command 🔒
Each player commands **one mobile mothership-base + a small fleet of ships**,
controlled **RTS-style** (select, order move/attack/harvest/guard/retreat;
control groups; formations). This is the **Homeworld** half of the fantasy; the
persistent, contested, fitting-driven universe is the **EVE** half.

- **Mobile base = capital, _disable-not-destroy_ 🔒.** The base is slow and powerful;
  it can mount weapons and tank, but **cannot be permanently destroyed**. At low HP
  it is forced into an **emergency jump/retreat** (cooldown + **cargo loss**),
  protecting the one-base-per-player investment while keeping it genuinely
  vulnerable and worth defending. Capturing territory means *driving off* the
  defender's base + fleet, not deleting their account's core asset.
- **Fleet size cap (launch) 🔒 = 6–12 ships/player (💡 start at 8).** Chosen so a
  20-player contested sector stays RTS-readable and within the bandwidth budget
  (App. B). The cap is **data-driven** and raised deliberately as the shard/engine
  scale up (the game is expected to grow well past 100 players — §App. B, R16).
- **Ship roles (💡):** *scout* (sensors/tackle), *fighter* (DPS), *harvester*
  (eXploit), *builder/logistics* (repair + construction), *hauler* (cargo),
  *EWAR* (jam/web/disrupt). Roles map to combat archetypes in §13.2.

### 13.2 Combat model — role + fitting + damage-type counters 🔒
Combat is **fleet-vs-fleet**, server-authoritative, and resolved by the shared sim
rules (§7.2). Depth comes from **composition and fitting**, not raw numbers.

- **Defense layers:** **shield / armor / hull**, each with per-**damage-type
  resists**. Shields regenerate; armor is repaired by logistics ships; hull damage
  is the danger zone.
- **Damage types (💡):** **kinetic / thermal / EM** (+ *explosive* later) — a
  rock-paper-scissors against resist profiles, so the *right* fit beats a bigger
  *wrong* fit. Rewards scouting the enemy before committing.
- **Fitting grid 🔒:** each hull has slots (high/mid/low + rig-equivalents) and a
  **power/CPU budget**; modules = weapons, tank, propulsion, **EWAR**
  (jam/web/scram/disrupt), reps, sensors. Fitting is the join between the **tech
  tree** (§13.3) and moment-to-moment play.
- **Tactical archetypes:** *tackle* (hold targets in place), *DPS*, *logistics*
  (remote repair), *EWAR*, *tank/anchor*. A balanced fleet beats an unbalanced one
  — the core skill expression.
- **Range/positioning:** weapons have optimal/falloff and tracking; positioning,
  focus-fire, and target-calling matter (RTS layer).
- **Active abilities (💡, post-launch lever):** overheat, MWD burst, EWAR bursts —
  added later **only where feel demands it** and gated by the prediction work (R-set,
  §8.4/§10.1), since they raise netcode/UI cost.
- **Loot-on-kill (economy event, §15):** a destroyed *ship* drops a recoverable
  `LootContainer` with a fraction of fit/cargo. Insurance (§13.9) softens the loss.

### 13.3 Progression — hybrid tech-tree + fitting 🔒
- **Vertical (tech tree):** **research** unlocks new **hull classes** and **module
  tiers**. Research is fed by resources + data salvaged from anomalies/NPCs (§13.7),
  giving exploration a progression payoff.
- **Horizontal (fitting + tactics):** *which* modules you fit and *how* you fly the
  fleet decide fights. A fully-researched player still loses to a better-fit,
  better-piloted smaller fleet.
- **Catch-up / flattened curve 💡:** diminishing returns on top tiers and accessible
  competitive hulls so newcomers reach *relevance* fast (full mastery still takes
  time). Counters the EVE "veterans permanently dominate" failure mode (R17).
- **❓ Time-gated training** (EVE-style passive skill points) is **not** in the
  launch model; tracked as a possible future axis (§19).

### 13.4 Economy — player-driven crafting 🔒
Destruction must create **demand**, or conquest and loot are hollow.

- **Production chain:** **raw resources → refine → components → ships/modules.**
  Most player-flown ships and modules are **player-built**; NPC seeding is minimal
  and exists mainly to bootstrap and to act as a price floor/ceiling.
- **Resources (💡):** a handful of raw types with **regional scarcity** (rarer/richer
  in low/null), so the security gradient (§13.5) drives trade and risk-taking.
- **Markets (💡):** **player-driven regional markets** (buy/sell orders at trade
  hubs), not a single global auction house — regional price differences create
  **hauling/trade gameplay** (and targets for piracy in low/null).
- **Single currency + sinks 🔒-intent:** fees, **ship insurance** (§13.9), refit
  costs, structure upkeep/territory tax (§13.6), fuel. Sinks keep the loot/destruction
  economy from inflating.
- **Persistence:** all trades/currency/loot/build-completion are **economy events**
  → **write-through / outbox, zero-loss** (§15).

### 13.5 World structure — tiered security (high → low → null) 🔒
The universe is a **risk→reward gradient**, replacing v0.8's "safe-zone bubbles
only" model (which doesn't support conquest):

| Tier | PvP | Territory | Role |
| --- | --- | --- | --- |
| **High-sec** | **Off** (NPC-enforced; aggressors disabled by response fleets) | None | **Protected onboarding & safe industry.** Lower yields. |
| **Low-sec** | **On**, no claims | None | Contested resource/PvP space; piracy; gateway to null. Better yields. |
| **Null-sec** | **On**, full | **Claimable (§13.6)** | Territorial conquest endgame. Best yields, anomalies, invasions. |

- **Base safe-zone emitter** from v0.8 is retained **only as a high-sec/low-sec
  defensive bubble around your own base** (no-PvP-inside in high-sec; a defensive
  bonus in low/null), not as the universe-wide PvP gate.
- Security tier is a property of a region/sector cluster, enforced server-side.

### 13.6 Territorial conquest (null-sec) 🔒
The PvP endgame, fitted to the **Homeworld×EVE individual-commander** model.

- **Objectives:** nullsec sectors contain **capturable structures** (e.g.
  resource-extractors, sensor arrays, jump beacons) that yield **income / buffs /
  build bonuses** to whoever holds them.
- **Ownership (💡, individual-first):** capture is by the **player (and their fleet)**
  who clears and holds it — consistent with "persons own fleets and use them to
  protect." Ownership is **persistent**, banked to the owner's account, and
  defended by parking/garrisoning fleet + base presence.
- **Contest mechanic 💡:** capturing requires **clearing defenders + a hold timer**
  (no instant flips); defenders get **reinforcement/retreat windows** so conquest
  is a campaign, not a drive-by. The owner's base **disable-not-destroy** rule
  (§13.1) is how a defender is ultimately pushed off a holding.
- **Upkeep / tax:** held structures cost upkeep (currency sink, §13.4) and pay out
  while held — use-it-or-lose-it pressure that keeps territory fought over.
- **⚠️ Social-glue caveat (R18):** with only **light parties/fleets** (§13.8) and no
  persistent corps, *individuals* hold territory. At ~100 players this is workable,
  but coordinated defense of large holdings is hard solo. **§19 tracks promoting
  persistent corporations forward** if conquest proves to need shared
  ownership/wallet/defense scheduling — the most likely first social expansion.

### 13.7 PvE content — invasions + anomalies 🔒
PvE is a **first-class anchor**, not just ambient AI.

- **Dynamic faction invasions/events 🔒:** NPC factions launch **escalating
  incursions** into sectors — server-driven events that **degrade local
  yields/safety until repelled**, pulling players (even rivals) into temporary
  cooperation. Tie escalation to the world clock and to how heavily a region is
  exploited (a built-in coupling of PvE pressure to the 4X loop).
- **Procedural anomalies / expeditions 🔒:** **scannable sites** (combat /
  exploration / salvage) that spawn across the world with **scaling difficulty and
  loot**, gated by sensor/scan skill — repeatable PvE income, research-data source
  (§13.3), and the main exploration gameplay. Harder/richer sites cluster in
  low/null (risk→reward, §13.5).
- **NPC AI** stays server ECS (`ERServer/ai/`): patrol/aggro/flee/defend/escalate;
  invasions and site guardians are scripted on top.
- **❓ World bosses / capital threats** and **escalating-territory-heat** were
  considered and **deferred** (not selected) — tracked in §19 as natural post-launch
  PvE expansions that fit this frame.

### 13.8 Social, parties & fleets 🔒 (light) — ownership model
- **Launch social layer = light parties/fleets 🔒:** ad-hoc **squads** for shared
  vision, fleet-up, voice-adjacent coordination, and **friendly-fire rules**; plus
  global/local/party **chat**. No persistent corporations/alliances/shared wallet
  at launch.
- **Ownership stays individual** (territory §13.6, base, fleet, assets) — the
  Homeworld "you command your own fleet" identity.
- **Diplomacy (lightweight 💡):** per-player/party standings (ally / neutral /
  hostile) drive the friendly-fire and engagement rules; no formal war-dec system
  at launch.
- **⚠️ Retention risk (R18):** MMО stickiness comes from social bonds. The plan
  **must** revisit persistent corps early if retention or conquest coordination
  suffers — see §19. This is the single biggest open *design* risk in v0.9.

### 13.9 New-player onboarding & loss mitigation 🔒
EVE-style conquest is brutal to newcomers; growth depends on a soft landing.

- **Protected starter / high-sec space 🔒 (§13.5):** new players spawn into no-PvP
  high-sec to learn the loop (move base, harvest, refine, fit, run an anomaly,
  fleet up) before choosing to venture into low/null.
- **Ship insurance / loss mitigation 🔒:** opt-in **insurance** partially reimburses
  destroyed *ships* (currency sink + risk buffer), keeping players willing to fight.
  The base's **disable-not-destroy** rule (§13.1) means the *core* asset is never
  fully lost.
- **Onboarding objective chain (💡):** a guided early sequence that teaches the loop
  and seeds first goals (considered; recommended to include with M3/M7).

### 13.10 Retention / session loop 💡
A healthy MMO needs daily reasons to log in:
- **Short loop (a session):** run anomalies / haul a trade route / defend against an
  invasion / skirmish in low-sec → earn resources + currency + research data.
- **Mid loop (days–weeks):** research up the tech tree, refit, grow the fleet, push
  into low/null.
- **Long loop (weeks–months):** claim and hold nullsec territory; build the regional
  economy; rivalries. ❓ Seasons/leaderboards tracked for post-launch (§19).

### 13.11 Entities (updated)
`Base` (mobile **capital**; storage/shipyard/sensors/weapons; HP+resists; safe-zone
emitter; **disable-not-destroy** retreat state), `Ship` (🔒 roles: scout / fighter /
harvester / builder-logistics / hauler / EWAR; **fitting grid** of `Module`s),
`Module` (weapon/tank/prop/EWAR/rep/sensor with power-CPU cost + damage-type/resist
stats), `ResourceNode`, `Projectile`, `LootContainer`, `MarketOrder`,
`TerritoryStructure` (claimable, owner, upkeep, yield; incl. **jump beacons**),
`JumpBeacon`, `AnomalySite`, `InvasionEvent`, `NpcUnit` (PvE AI), `Player`,
`Party/Fleet` (light squad).

### 13.12 Navigation & travel — warp + jump-beacon network 🔒
The universe spans ±975 ly, so travel needs three scales (all **server-authoritative**;
all but the longest are sim-stepped so they can be **interdicted** — core PvP):

1. **Sublight maneuver** (combat / short range) — the RTS movement layer (§6); ships and
   the base move at sublight within an engagement. Authoritative, validated per tick.
2. **Warp** (in-system / short hop) — *align then warp* to a **scanned/bracketed
   destination** (resource node, beacon, anomaly, structure, fleet member) at high
   speed; **travel time ∝ distance**. Warp is fast sublight along a path, **not
   instant**, so it can be **interdicted** by tackle/warp-disruptors (§13.2) —
   enabling **interception, ganks and blockades**.
3. **Jump** (long-haul) — a network of **jump beacons** (public NPC beacons in
   high-sec; **player/territory-owned** beacons in low/null, a `TerritoryStructure`
   type, §13.6). A **jump drive** (on the base and jump-capable ships) jumps between
   **linked beacons**, consuming **fuel** (a harvested/crafted resource — economy sink,
   §13.4), with a **spool-up** (a vulnerability window) and a post-jump **cooldown**.
   Jump range is limited → crossing the galaxy means **chaining beacons**.

- **Mobile-base travel:** the capital base warps slowly and jumps via beacons (larger
  fuel cost / longer spool) — this is how the "mobile home" actually relocates.
- **Why beacons matter:** they make **territory strategic** (own the network = control
  movement; chokepoints to camp) and give the economy a steady **fuel** sink.
- **Fuel risk:** running dry strands a fleet/base (a real exploration hazard).
- **Netcode/interest (R21):** warp/jump cross sectors fast → rapid interest-set churn;
  the server **prefetches the destination sector's interest** at warp/jump start, and
  validates interdiction server-side. Ties to §6.3 / §8.4.
- **Onboarding:** high-sec has **dense public beacons + short distances**; low/null are
  sparser and riskier (risk→reward, §13.5).
- **Data-driven:** the beacon graph and region/security layout are **game data**
  (§12.6); player beacons live in the schema (`TerritoryStructures`, §15).

---

## 14. Accounts & Authentication 🔒 (real login at launch)
- **Accounts** in SQL Server: username, **salted PBKDF2 password hash via CNG**
  (per-user random salt + **server-side pepper**, high iteration count — never
  plaintext), profile, created/last-login, status. *(CNG offers no Argon2; PBKDF2-
  HMAC-SHA512 with a high, tunable iteration count is the deliberate MS-only choice.)*
- **Registration & login** over the **encrypted, server-authenticated** channel
  (§8.5): credentials sent only after ECDH + server-key verification; server verifies
  and issues an expiring **session token** bound to the connection.
- **Abuse controls:** per-account and per-IP **login rate-limiting + lockout/backoff**
  to blunt credential stuffing against custom auth.
- **Sessions:** token validation on reliable traffic; reconnect support; **one
  active session per account** (deny/kick duplicate; reconnect handled atomically to
  avoid races); login binds the session to the player's `Base`/entity.
- **🔒 Launch = custom username/password.** Federated **Entra ID** remains a possible
  post-launch option (especially with Azure SQL).
- **Dev stub:** a "pick a name" identity stays behind a dev flag for M1–M4
  iteration; real auth lands with persistence (M5).

---

## 15. Persistence — SQL Server over the network 🔒
- **System of record = Microsoft SQL Server, accessed over the network — not
  containerized.** **Self-hosted host/VM now (Developer/Standard) → Azure SQL
  later**; migration is essentially a connection-string + auth change.
- **Access:** **ODBC Driver 18** (`sql.h`/`odbc32.lib`, Windows SDK); thin custom
  ODBC wrapper in `ERServer/persist/`. Parameterized statements / stored procs;
  **connection pooling**; the persistence thread runs both paths below; TVP/`bcp` for
  big checkpoints; **`Encrypt=yes`**.
- **Durability boundary 🔒:**
  - **Economy events (write-through / transactional outbox):** trades, currency,
    build completion, kills/**loot-on-kill**, account changes. Committed
    transactionally (or staged to a durable outbox drained in order) **before** they
    are considered authoritative — **zero-loss** on crash.
  - **High-frequency state (write-behind):** position, velocity, transient AI state —
    **batched**, with a stated **RPO** (e.g. ≤ a few seconds of movement may be lost
    on hard crash; never an economy event).
- **Warm restart:** a periodic **binary state snapshot (blob) + an event log since the
  snapshot** (same serde primitives, §7.2) — restart replays the log onto the last
  snapshot for a clean, verifiable state, rather than reconstructing the sim from
  normalized rows.
- **Schema (`/db/`, Azure-SQL-compatible — see `db/schema.sql`):** accounts/sessions
  (§14); **wallet + append-only currency ledger**; **player standings**; the
  **`ItemDefs` catalog** (canonical item-id space); **tiered-security regions**
  (§13.5); **bases** (layered HP + disable-not-destroy state, §13.1); **ships**
  (hull/role + layered HP) and **fitting** (`ShipModules`/`FitTemplates`, §13.2);
  **itemized inventory** (`BaseInventory`/`ShipCargo`) + build queue; **research +
  unlocked blueprints** (§13.3); **markets** (`MarketOrders`/`MarketTrades`, §13.4);
  **territory** (`TerritoryStructures` + capture log, incl. **jump beacons** §13.12,
  §13.6); **insurance** (§13.9); itemized **loot**; **killmail log**; **in-game mail +
  offline notifications** (§24 — `Mail`/`Notifications`, **migration 003**);
  **outbox**, **snapshots**.
  **Catalog/balance boundary:** item/hull/module *stats*, crafting *recipes*,
  research *costs/prereqs*, and anomaly/invasion *definitions* are **game data**
  (versioned with the build, loaded by NeuronCore) — **not** SQL; SQL keeps only the
  canonical item ids + mutable player/economy/world state.
  **Transient sim state** (NPC units, projectiles, anomaly sites, invasion events,
  live parties) is **never normalized** — it lives only in the warm-restart
  `SimSnapshots` blob (§9, §13.7). Avoid features absent in Azure SQL DB (cross-DB
  queries, SQL Agent jobs → Elastic Jobs, FILESTREAM). Versioned migrations
  (`db/migrations/`, forward-only).
- **App→DB auth:** **🔒 SQL login now → managed identity / Entra ID on Azure SQL.**
- **ERServer stateless** → restarts recover from snapshot + log.

---

## 16. Build, Tooling, Testing
- **MSBuild** 🔒 (`EarthRise.sln`): UWP `.vcxproj` → MSIX; others standard. The
  **`dxc` (DXCCompile) task** emits embedded SM6/DXIL shader headers (§12.4); asset
  cooking (CMO/DDS) runs as pre-build steps via the VS content pipeline.

### 16.1 Microsoft Native Unit Test projects 🔒

**Policy: every feature must have a test in the matching `<project>Test` project.**

| Project under test | Test project | Scope |
| --- | --- | --- |
| **NeuronCore** | **NeuronCoreTest** | ECS, WorldPos/sectors, serde, sim rules, fixed-step timer, allocators |
| **NeuronClient** | **NeuronClientTest** | Session, ReplicaManager, InterpBuffer, controller logic |
| **NeuronRender** | **NeuronRenderTest** | DeviceResources init/teardown, SceneRenderer/CanvasRenderer unit logic |
| **NeuronAudio** | **NeuronAudioTest** | WAV/RIFF parser (valid/invalid/edge), bus/volume logic, X3DAudio emitter math, XAudio2 device init/teardown |
| **ERServer** | **ERServerTest** | Net I/O, handshake, reliability, sim loop, persistence layer |
| **ERHeadless** | **ERHeadlessTest** | Multi-session host, bot harness, record/replay determinism |

- Each `<project>Test` is a **Microsoft Native Unit Test project** (`.vcxproj` with
  `<UseOfMfc>false</UseOfMfc>` and the native test framework; includes
  `<ProjectReference>` to the project under test).
- Every `<project>Test` is added to **`EarthRise.sln`** alongside its project.
- A feature is not complete until its `<project>Test` tests pass. New feature code and
  its test cases land in the **same commit**.
- Tests that require Windows platform APIs (CNG, IOCP, D3D12) live in the matching
  `<project>Test`; platform-independent logic also gets a Linux-compatible test in
  `NeuronTools/testrunner/` (see §16.2) so CI can catch regressions without a Windows
  build.

### 16.2 Custom headless runner (Linux-compatible)

- **Tests:** custom assert runner in `NeuronTools/testrunner/`; units for math
  (DirectXMath), serialization, reliability (loss/reorder/dup), **crypto handshake
  (incl. MITM/nonce/replay cases)**, CMO/DDS/**WAV** parsers, and InterpBuffer.
  **ERHeadless** = multi-client integration & load harness plus an **input-log
  record/replay determinism harness** (same input → identical sim, the primary
  netcode debugging tool).

### 16.3 Perf gates & CI

- **Per-milestone perf gates** are numeric: sim tick p50/p99 ms, per-client
  bandwidth, render frame time (§17, App. B).
- **CI / web sessions:** `SessionStart` hook builds NeuronCore/NeuronClient/
  ERServer/ERHeadless/NeuronTools and runs tests against a **dev SQL Server reached
  over the network** (UWP packaging stays local). Native Unit Test projects run on the
  Windows build agent.

---

## 17. Milestone Roadmap

**M0 — Foundations** *(S–M)* — `EarthRise.sln`; NeuronCore skeleton (DirectXMath,
ECS w/ generation handles, world [`int64`/sector], serde, **fixed-step time**,
allocators, logging); NeuronClient + ERHeadless skeletons; test runner; `dxc`
shader-header build wired. **Done:** all targets build; NeuronCore tests pass;
fixed-step accumulator unit-tested.

**M1a — Networked transport (headless)** 🔒 *(M–L)* ← first — ERServer (containerized)
30 Hz fixed loop; **stateless-cookie → version-gate → CNG ECDH w/ server-key
verify → AES-GCM (nonce/replay)** handshake; reliable-UDP (acks, frag, resend);
**ERHeadless drives ≥3 parallel bots**; server-authoritative replication +
interpolation; **move the base**. No rendering. **Done:** ≥3 bots see the base cross
a sector boundary under simulated loss/reorder/dup; MITM/replay tests pass; tick p99
within budget (App. B).

**M1b — Client tech slice** *(M)* — UWP client renders base + a few ships with **3D
Scene + 2D Canvas HUD** as separate passes; **DX12 engine foundation** (FL 12_0,
triple-buffered flip-model + fences, descriptor heaps, PSO cache, barriers — §11.1);
**space-RTS camera** + basic **mouse+keyboard** input; snap-on-ack correction.
**Done:** 1 UWP + ≥3 bots share the world; no `int64_t` reaches the GPU; 3D/2D split
verified; camera pan/zoom + select/move work; loopback path tested **and** a
non-exempt run validated before Store.

**M2 — Darwinia look + audio** *(M–L)* — DDS + **CMO** loaders, monospace Canvas HUD,
bloom + additive particles, instanced ships; **`NeuronAudio`**: XAudio2 voice graph +
4 buses, WAV/PCM-16 RIFF reader, **X3DAudio 3D** (listener = camera), ambient beds +
event SFX + UI + music. **Done:** an instanced fleet (your CMO meshes) with thrusters
+ glow over a legible HUD at target frame time, **with 3D-positioned thruster/weapon
SFX, ambient bed and music** mixed across buses (and ERHeadless still builds/runs with
no audio); **GPU-compute particles**, **radar/overview HUD** basics, settings screen.

**M3 — Core 4X loop, fleet command & navigation** *(L)* — nodes, harvesting, storage,
build queue, sensor/fog; **RTS fleet control** (select/move/attack/guard/control-groups)
over a 6–12-ship fleet + mobile base; **navigation: warp + jump-beacon network**
(fuel, interdiction) + **starmap UI**; tactical **overview list**. **Done:** harvest →
return → build a ship, **warp/jump across beacons**, and **command a multi-ship fleet**
to clear a basic NPC site, server-authoritative.

**M4 — Scale & interest** *(L)* — sector subscriptions, delta compression, snapshot
job pool; **ERHeadless ~100 bots**. **Done:** 100-bot load test holds 30 Hz within
the bandwidth budget (App. B); per-client bandwidth measured vs the M0 estimate.

**M5 — Accounts, auth & persistence** *(M–L)* — real login (custom username/password,
PBKDF2 + pepper + rate-limit); ODBC persist layer + schema/migrations + **outbox
(write-through) + write-behind + snapshot/log warm-restart**; **SQL Server over the
network (self-hosted)**; ERServer stateless; **24/7 rolling-restart drill** (warm-restart
as an uptime SLA) + **client reconnect with backoff/jitter** (§26, R22). **Done:**
register/login works; kill & restart the ERServer container → world + bases + economy
restore with **zero economy loss**, and connected clients reconnect cleanly.

**M6 — Combat model & deployment** *(L)* — **role + fitting + damage-type combat**
(shield/armor/hull + resists, module fitting grid, EWAR/logistics archetypes),
weapons/projectiles (local sub-stepping for fast shots), PvE AI, **loot-on-kill +
base disable-not-destroy**; **Kubernetes production deploy (Windows nodes + UDP LB)**
and **Azure SQL migration**; optional own-unit/fleet **prediction** where feel needs
it; Store-compliance pass. **Done:** balanced fleet-vs-fleet fights where fit &
composition beat raw numbers, playable on the prod topology.

**M7 — Sandbox: conquest, economy, PvE content & onboarding** *(L–XL)* —
**tiered security (high→low→null)**; **territorial conquest** (claimable structures,
capture/hold timers, upkeep/yield, ownership persistence); **player crafting economy**
(refine→components→build) + **regional markets** + currency sinks + **ship
insurance**; **dynamic faction invasions** + **procedural anomalies/expeditions**;
**protected starter onboarding** + objective chain; retention loop; **full UI suite**
(fitting / market / research / inventory / territory), **in-game mail + notifications**
(§24), **touch control scheme** (§23). **Interest at scale:** entity aggregation/LOD
validated for contested sectors (App. B, R16). **Done:** a full sandbox session —
onboard in high-sec, build & fit a fleet, run anomalies, trade, push into null, and
**claim & hold territory** through a contested fight — playable end-to-end by players +
bots (mouse+keyboard and touch), with zero economy loss across a server restart.

---

## 18. Risks & Mitigations

| # | Risk | Impact | Mitigation |
| --- | --- | --- | --- |
| R1 | **UWP networking sandbox / loopback / platform decline** | High | §8.1; `ISocket`; headless Win32 dodges loopback; test Store path early (M1b). **UWP kept for Store reach; revisit Win32/Windows App SDK if Store is dropped.** |
| R2 | `int64`/sector vs float GPU precision | Med | Floating origin + sector-local floats (~1 mm @ S=14); single sector-offset representation (§6). |
| R3 | Single-shard scaling to 100 | Med | 30 Hz sim; interest mgmt + delta compression + snapshot job pool; ERHeadless load tests (M4). |
| R4 | **External DB latency/availability** | Med | DB out of tick hot path; in-memory sim; outbox + write-behind; co-locate ERServer near SQL/Azure SQL. |
| R5 | **K8s + Windows containers + UDP** | Med | Windows node pool; **UDP-capable LoadBalancer**; one pod per shard + **client→pod affinity** (§20). |
| R6 | **Credential/session security & active MITM** | High | **Server-key-pinned ECDH** + AES-GCM; **nonce-per-packet + replay window**; PBKDF2 + pepper; rate-limit/lockout; tokens; `Encrypt=yes` to SQL. |
| R7 | Azure SQL feature parity | Low–Med | Keep schema Azure-SQL-compatible from day one (§15). |
| R8 | "Custom everything" scope | High | Only MS platform components; strict milestone scoping; **prediction deferred**; M1 split M1a/M1b. |
| R9 | UWP no runtime HLSL | Low | Embedded SM6/DXIL headers from day one (§12.4). |
| R10 | Reliable-UDP / crypto correctness | Med | Loss/reorder/dup + handshake/MITM/nonce/replay tests; **record/replay determinism harness**; ERHeadless. |
| R11 | CMO edge cases (skinning/anim) | Low–Med | Static meshes first; add skinning later; validate via meshcook. |
| R12 | **Write-behind data loss on crash** | Med | Durability boundary: economy = write-through/outbox (zero loss); position = write-behind within a stated RPO; snapshot + log restart (§15). |
| R13 | **GCM nonce reuse / handshake DoS** | High | Direction‖64-bit packet-counter nonce, rekey-before-wrap; stateless cookie before ECDH (§8.3/§8.5). |
| R14 | **Fixed-step vs provided variable-step timer** | Med | Real accumulator with bounded catch-up, WinRT-free core; unit-tested in M0 (§7.2). |
| R15 | **Gameplay depth / retention thin** (was: no real game design) | **High** | §13 rewritten to a full spec (combat/progression/economy/conquest/PvE/onboarding); retention loop defined (§13.10); validate the *fun* with bots + a closed playtest before M7 polish. |
| R16 | **Fleet entity-count blows the bandwidth/sim budget** | **High** | Launch fleet cap 6–12; **interest-bounded** per-client visible set + entity aggregation/LOD + projectile batching; load-test the *contested-sector* case at M4 (App. B). |
| R17 | **Conquest too brutal for newcomers → no growth** | High | High-sec protected onboarding (§13.5/13.9); ship insurance + base disable-not-destroy; catch-up/flattened power curve (§13.3); onboarding objective chain. |
| R18 | **Thin social glue (light fleets only) hurts retention & conquest defense** | Med–High | Individual ownership works at ~100 players; **persistent corporations tracked as the first social expansion** (§13.8/§19) — promote forward if retention or coordinated defense suffers. |
| R19 | **Audio on UWP / WAV-only / X3DAudio correctness** | Low | XAudio2 2.9 + X3DAudio are SDK components (UWP-supported); custom RIFF reader + `wavcheck` validate PCM-16; emitter math + parser unit-tested (NeuronAudioTest, testrunner); suspend/resume voice handling; audio is presentation-only (no sim/determinism impact). |
| R20 | **UI/HUD scope, esp. RTS-on-touch** | **Med→Low** | **Decided approach (§23.1):** an **overview-driven command model** shared by desktop & touch (per EVE Echoes prior art) — select/command via overview list + smart-select + smart action, **camera on two-finger only** (no one-finger select-vs-pan ambiguity), box-select demoted to a desktop shortcut. Stances/autopilot cut APM; custom immediate-mode toolkit on Canvas; touch spec in `docs/design/touch-controls.md`; still prototype/validate touch separately before relying on it. |
| R21 | **Warp/jump interception netcode & fast interest churn** | Med | Warp/jump are sim-stepped & server-authoritative (interdiction validated); **prefetch destination-sector interest** on warp/jump start; beacon graph is data-driven; test interception + sector-transition churn at M3/M7 (§13.12, §8.4). |
| R22 | **24/7 uptime depends on warm-restart reliability** | Med–High | Warm-restart (snapshot+outbox) is now an **uptime SLA**, not just crash recovery: restart drills in M5; reconnect **backoff/jitter** against thundering-herd; rolling-restart playbook (§26); economy stays zero-loss via outbox (§15). |

---

## 19. Open Questions & Future Considerations

**Resolved this revision (v0.11 — completeness):** travel = warp + jump-beacon network;
renderer = D3D12 FL 12_0 / Tier 2 / SM6.0, triple-buffered HDR forward + bloom;
input = mouse+keyboard primary + touch on tablets; tactical UI = 3D overlay + overview
list + 2D radar disc; comms = chat + in-game mail + offline notifications; uptime =
24/7 rolling restarts (no scheduled downtime); localization = English at launch but
localization-ready; accessibility = scalable UI + audio cues at launch.

**Resolved earlier (v0.9 — gameplay):** core fantasy = Homeworld × EVE fleet
command; combat = role + fitting + damage-type counters; progression = hybrid
tech-tree + fitting; economy = player-driven crafting; world = tiered security
high→low→null with claimable nullsec territory; PvE = invasions + procedural
anomalies; social = light parties/fleets w/ individual ownership; base = capital,
disable-not-destroy; launch fleet cap 6–12.

**Resolved earlier (v0.7):** sim tick = 30 Hz; coords = `int64` centred at 0 with
struct sector key; client prediction deferred past M1; durability boundary; shaders
SM6/DXIL via `dxc`; M1 split M1a/M1b; crypto hardening.

**Live unknowns (track, don't pretend they're closed):**
- **Fleet entity counts vs bandwidth** — the *contested-sector* case (players × fleet),
  not the flat-100 case, must be validated at M4 (App. B, R16); fleet cap is the dial.
- **Whether persistent corporations must be promoted forward** from post-launch into
  the conquest milestone (R18) — likeliest first social expansion if territorial
  defense/retention needs shared ownership/wallet/scheduling at light-fleet scale.
- **Fleet-cap balance** (6 vs 8 vs 12) — RTS readability + combat feel vs entity cost.
- **Market model:** regional hubs vs a smaller number of trade centers; how order
  matching/persistence scales as an economy event.
- **Economy balance:** resource scarcity by tier, sink/faucet tuning, insurance payout
  rate — needs live data; expect post-M7 iteration.
- **Onboarding objective chain** scope (recommended for M3/M7).
- Whether any subsystem needs **own-unit/fleet prediction** (decided by feel at M6).
- AES-GCM **rekey interval** vs packet rate; replay-window width.
- **RTS-on-touch control scheme** (select-vs-pan disambiguation, command palette) —
  prototype & validate before relying on the tablet target (R20).
- **Warp/jump balance** — warp-speed vs distance, jump range/fuel/spool/cooldown, and
  interdiction strength; tune for interception gameplay without making travel tedious.
- **Mail/notification volume & anti-spam** (rate-limits, blocklists) at scale (§24).
- **Particle/voice budgets** in big fights (render + audio) vs App. B (R16).

**Deferred to post-launch (fit the v0.9 frame, not selected now):**
- **Persistent corporations / alliances + diplomacy & shared wallet** (launch = light
  fleets) — see R18; may be pulled forward.
- **Time-gated skill training** as a progression axis (§13.3).
- **World bosses / capital-class PvE threats** and **escalating-territory-heat** PvE
  (§13.7).
- **Active combat abilities** (overheat/EWAR bursts) where feel/prediction allow (§13.2).
- **Seasons / leaderboards** and other meta-retention (§13.10).
- **Gamepad support** and **multi-language** localization (launch is M+K+touch, English).
- **Colorblind-safe IFF** and **visual alerts/subtitles** for audio cues — recommended
  accessibility follow-ups beyond the launch scope (§22.5).
- Federated **Entra ID** / social login (launch is custom username/password).
- **Multi-shard** topology + directory/matchmaking — the main lever to grow well past
  100 players (§13.1 growth intent).
- Azure SQL **tier sizing**, read-replicas, geo-redundancy.
- Revisiting **Win32 / Windows App SDK** if Microsoft Store distribution is dropped.

---

## 20. Deployment & Containerization

🔒 **ERServer** runs in a **Windows Server Core container**. 🔒 **SQL Server is an
external network service — never containerized.**

**SQL Server (external)**
- **Self-hosted host/VM now (Developer/Standard) → Azure SQL later.** ERServer
  connects via **ODBC over TCP 1433, `Encrypt=yes`**; connection string +
  credentials from a **secret/env**.
- Migration self-hosted → Azure SQL = connection-string + auth change (managed
  identity/Entra ID); schema kept Azure-SQL-compatible (§15).

**Dev — Docker Desktop** 🔒
- Runs **only the ERServer Windows container** locally; SQL Server is reached **over
  the network** (a local dev instance on the host/LAN or a shared dev SQL).
- Because SQL isn't containerized, the Docker Desktop Windows/Linux
  container-mode toggle is a **non-issue** — no mixed-OS container stack needed.
- Inner-loop option: run ERServer directly on the host for fast iteration; use the
  container for parity checks.

**Prod — Kubernetes** 🔒
- ERServer is a **Windows container** → the cluster needs a **Windows node pool**
  (e.g., AKS Windows nodes), tag matched to the host build.
- Reliable-UDP needs a **UDP-capable LoadBalancer** (HTTP ingress won't route UDP):
  Service `type: LoadBalancer` with `protocol: UDP` / NodePort / cloud UDP LB.
- **One shard = one ERServer pod**, on its own UDP endpoint, with **client→pod
  session affinity**. A future directory/matchmaking service routes players to
  shards.
- **Azure SQL** accessed from the cluster (private endpoint / firewall); connection
  string via a K8s Secret.
- ERServer is **stateless** → safe rolling restarts; persistence lives in SQL.

---

## 21. Observability & Telemetry
You cannot hold the M4 bandwidth budget you cannot measure. ERServer exposes,
per-shard and per-client:
- **Sim:** tick time p50/p99, catch-up steps used, entity/system counts.
- **Net:** per-client downstream/upstream bytes, packet loss / retransmit / reorder
  rates, fragment counts, **AEAD-auth failures**, replay-window rejects.
- **Persistence:** outbox depth & drain latency, write-behind batch size/lag, RPO
  watermark.
- **Auth:** login attempts / lockouts / rate-limit hits.

Exported as structured logs + counters (lightweight, MS-only — perf counters / ETW /
log lines), consumable by the headless harness for automated perf gates.

---

## 22. Client UI, HUD, Radar & Accessibility 🔒
UI is built on the **CanvasRenderer** primitive layer (§11) with a thin **custom
immediate-mode widget toolkit** (panels, buttons, lists, sliders, text fields,
drag) — no third-party UI lib. **DPI-aware, anchor-based layout** with a user **HUD
scale**; all strings flow through a **localization string table** (§22.4).

### 22.1 Screen inventory (launch)
- **Boot / login / register** (account, §14) → server/version status.
- **Main HUD (in-space)** — the primary screen (§22.2).
- **Fitting** — hull slot grid, modules, **PG/CPU budget**, save/load **fit
  templates** (§13.2; `FitTemplates`).
- **Inventory / cargo & build queue** — base storage + ship cargo; enqueue builds.
- **Market / trade** — regional order book, place buy/sell, fees, my orders (§13.4).
- **Starmap / navigation** — the **jump-beacon graph** (§13.12), set destination/route,
  security-tier coloring, owned territory, fleet/base location.
- **Research / tech-tree** — branches × tiers, datacore costs, queue (§13.3).
- **Social** — standings, party/fleet roster, chat (§24).
- **Mail & notifications** (§24).
- **Settings** — graphics / audio / keybinds / accessibility / language (§25).

### 22.2 HUD spec (in-space)
- **Tactical view & radar (§22.3).**
- **Fleet command bar** — control groups, selected ships, formation/stance, orders.
- **Selected-unit & target panels** — **shield / armor / hull** bars + per-type
  resists, modules, range; base capacitor/jump-fuel/jump-cooldown.
- **Sensor / scan UI** — scan anomalies/beacons; fog/sensor range.
- **Alerts / notifications** — low-hull → retreat, under attack, jump ready, invasion
  incoming (paired with **audio cues**, §22.5).
- **Resource / credits readout**, **chat** (§24), **context commands**.

### 22.3 Radar / tactical overview 🔒 (3D overlay + overview list + 2D radar disc)
Three synced views (selection is shared):
- **3D bracket overlay** — IFF brackets on entities in the scene + **off-screen
  direction arrows** for out-of-view contacts (fits the minimalist Darwinia look).
- **Sortable overview list** — the **primary selection & command surface** on both
  desktop and touch (§23.1): contacts with **type / distance / velocity / IFF**,
  sortable & filterable (EVE-style "overview"); tap/click to select & issue actions.
- **2D radar disc** — relative **bearing + range rings + IFF**, with vertical-offset
  indicators for true-3D awareness.
- **IFF** = friend / neutral / hostile from **player standings** (§13.8), shown by
  **shape + color** (color is never the only channel — see accessibility §22.5).

### 22.4 Localization 🔒 (English at launch, localization-ready)
All UI text is referenced by **string id** through a string table; **no hard-coded
display strings**. The monospace bitmap-font pipeline (§12.2) stays **Unicode-capable**
(codepoint→cell). **English ships at launch**; adding a language is **data, not code**.
*(Fixed-grid monospace makes CJK hard; treat CJK/RTL as a later, separate effort.)*

### 22.5 Accessibility 🔒 (launch scope)
- **Scalable UI / HUD 🔒** — user HUD/text scale (also serves the **touch/tablet**
  target and high-DPI). 
- **Audio cues for key events 🔒** — distinct, learnable cues (low hull, under attack,
  jump ready) via the NeuronAudio cue catalog (§11.3) so players aren't forced to watch
  everything.
- **💡 Recommended next:** colorblind-safe IFF (shape/icon, not color alone — partly
  satisfied by §22.3) and on-screen **visual alerts** mirroring critical audio cues;
  tracked in §19.

---

## 23. Input & Controls 🔒 (overview-driven; mouse+keyboard + touch)
Input flows: UWP **CoreWindow** pointer/key/gesture events → an **`IInputSource`**
abstraction → either UI events or **fleet commands → intents** (server-validated,
§8.4; never client-authoritative). **Keymap is rebindable** (client config, §25).

### 23.1 Primary model — overview-driven command (shared by desktop & touch) 🔒
Following **EVE Echoes** (which proved this genre on touch), the **primary control
surface is the §22.3 overview list + radar + smart-select buttons — not box-dragging
in the 3D world.** Selection and commands route through the **same intent layer** on
both desktop and touch; only the *input affordances* differ. This keeps required APM
and precision low (suits "fleet **commander**", not "unit micromanager"), and makes the
two platforms one game, not two.

- **Selection:** tap/click a contact in the **overview** (or its bracket/radar blip);
  **smart-select buttons** — *all combat ships · all of type · all harvesters · control
  group #* — replace manual box-dragging for grouping.
- **Commands:** a **smart single action** resolves by target (empty space = move; enemy
  = attack/lock; node = harvest; beacon = warp/jump; ally = assist), plus a small
  **contextual command bar** (move/attack/guard/orbit/keep-range/warp/retreat) and
  module/ability buttons. The 3D view is **camera-first**; you rarely "drive" it.
- **Autonomy reduces input:** stances + formations + control groups + **autopilot**
  (route across the beacon graph, §13.12) mean a fight is "pick target → orbit/
  focus-fire", not per-ship micro.

### 23.2 Desktop (mouse + keyboard) affordances
The overview model **plus** power-user shortcuts: left-click select; **drag = box-select
(a desktop convenience, not the primary path)**; right-click = smart/context command;
double-click = select-by-type; **`Ctrl+#` set / `#` recall control groups**; camera =
WASD/edge-scroll pan, wheel zoom, MMB/alt-drag orbit, center-on-selection; hotkeys for
modules/abilities and warp/jump.

### 23.3 Touch (tablets) affordances
Same overview model with touch affordances; **the camera never lives on one finger**, so
the select-vs-pan ambiguity (R20) is designed out:
- **One finger:** tap = select (overview blip/bracket); tap world = smart action;
  **tap-and-hold = radial context menu** (recovers "right-click").
- **Two fingers (camera only):** pan, **pinch-zoom**, twist-rotate.
- **On-screen:** smart-select bar, contextual command bar, control-group bar, module
  buttons — all **large, scalable hit targets** (§22.5).
- **Box-select** is a *secondary* tool behind an explicit "marquee" toggle (not the
  default drag), since grouping is normally done via smart-select buttons.
- Full scheme + gesture/smart-action tables: **`docs/design/touch-controls.md`**.

### 23.4 Selection & command model (shared)
Single / additive / by-type / control-group selection; **smart context action** keyed
by target; commands **queue** (shift-chain on desktop; sequential taps on touch) and
respect formations/stance. All commands become validated **intents** (§8.4).

---

## 24. Communications & Notifications 🔒
- **Chat (real-time):** channels — **global · local (by region/sector) · party/fleet ·
  direct**. Carried on the **ReliableOrdered** channel (§8.2); server-relayed with
  **rate-limit + mute/block**; transient (recent scrollback only).
- **In-game mail (persistent) 🔒:** player-to-player messages stored in SQL
  (`Mail` table, **migration 003**, §15); inbox UI, unread counts; **survives offline**.
- **Offline notifications 🔒:** server-generated events surfaced **on login and live** —
  **territory attacked/lost** (§13.6), **market order filled** (§13.4), **insurance
  paid** (§13.9), **killmail** involving you, **build complete** — stored
  (`Notifications` table) until read. Strong async-retention hook.

---

## 25. Client Platform Services 🔒
- **Settings / config:** stored in **UWP local storage** (`ApplicationData`), **not
  SQL** — graphics options, **audio bus volumes**, **keybinds**, HUD scale/layout,
  accessibility, language. Sane defaults + reset.
- **Graphics options:** resolution / fullscreen (UWP), VSync / frame cap, bloom &
  particle quality, render scale, HUD scale — driving §11.1–11.2.
- **Client logging & crash reporting:** structured **rolling local logs** + **ETW**;
  on crash capture a **minidump** + recent logs (Windows Error Reporting / local),
  **opt-in upload**; **DRED** for GPU device-removed. (Server telemetry is §21.)
- **Updates / patching:** client ships and auto-updates via **MSIX / Microsoft Store**;
  the **protocol-version gate** (§8.5) blocks mismatched clients with a clear
  "update required" message; assets are in-package; game data is build-versioned (§12.6).

---

## 26. World Clock & Live-Ops 🔒
- **24/7, no scheduled downtime 🔒:** rely on the **warm-restart** (snapshot + outbox,
  §15) for **rolling restarts/deploys** — k8s restarts the shard pod and clients
  **reconnect** (session token + one-session rule, §14; backoff/jitter to avoid a
  reconnect storm — R22). A brief reconnect blip is acceptable; **warm-restart
  correctness is now an uptime SLA**, drilled at M5.
- **Authoritative world clock:** the server maps the monotonic sim tick → UTC and is the
  single time source (clients align via clock-sync, §8.5). Drives **invasion scheduling,
  anomaly spawn/despawn, market-order expiry, structure upkeep ticks, insurance
  expiry**, and daily/weekly cadences.
- **Event director:** a lightweight server system schedules invasions/anomalies on the
  world clock + region exploitation (§13.7), within the sim/bandwidth budget.
- **Live-ops levers:** server tunables (spawn/faucet/sink rates, fleet cap) are **game
  data** with **hot-reload** (§12.6) → economy/PvE pacing adjustable **without a
  redeploy**, guided by telemetry (§21).

---

## Appendix A — Packet format sketch
```
UDP datagram (post-handshake: AES-GCM AEAD; nonce = dir-bit ‖ 64-bit packet number)
├── Header (AAD): protocol_id u32 · connection_token u64 · packet_number u64
│                 · per-channel: sequence u16 · ack u16 · ack_bits u32
└── Payload (encrypted): 1..N messages { channel u8, msg_type u8, length u16, body … }
   (fragments: message_id, fragment_index, fragment_count)
```
> The 64-bit `packet_number` doubles as the AEAD nonce input and feeds the replay
> window; the 16-bit per-channel `sequence` is reliability/ordering only.

## Appendix B — Tick & timing budget
| Quantity | Target |
| --- | --- |
| Sim tick | 30 Hz (~33.3 ms) |
| Snapshot send / client | 20 Hz |
| Client render | display-rate (60+ fps), decoupled |
| Interpolation delay | ~100 ms |
| Safe UDP payload | ~1200 bytes |
| Launch fleet cap | **6–12 ships/player (💡 8)** — data-driven (§13.1) |
| **Per-client downstream (re-derived for fleets)** | see below — **interest-bounded**, not flat 100 |
| Per-client upstream | RTS intents/orders only, ≪ downstream |

> **v0.9 — fleets change the entity math.** v0.8 assumed a flat ~100 visible
> entities. With fleet command, a *contested sector* is roughly
> **(players in interest) × (base 1 + fleet ≈ 8) + NPCs + projectiles**. A 20-player
> brawl is therefore on the order of **~180–250 ship/base entities** (plus
> short-lived projectiles), **not 100** — and a worst-case "everyone in one sector"
> blows the budget. So:
> - **Per-client downstream is bounded by _interest_, not headcount:** cap the
>   visible-entity set per client (sector subscription + max-N nearest + LOD), e.g.
>   target **≤ ~250 visible entities × ~16 B delta × 20 Hz ≈ 80 KB/s (~640 kbit/s)**
>   in a big fight, ≪ that when dispersed.
> - **Levers (M4/M7, R16):** hard interest culling; **entity aggregation/LOD** for
>   distant fleets (send a fleet as a cluster, not N ships); projectile batching;
>   per-client visible-entity cap; fleet-cap tuning. Validate the *contested-sector*
>   case — not just the dispersed case — in the M4 load test.
> - **Growth:** the shard is sized for ~100 now but the fleet cap, interest budget,
>   and (later) multi-shard (§19) are the knobs to grow "much more" when successful.

## Appendix C — Glossary
- **Shard** — one server process / one contiguous world.
- **Bot** — automated *client* session (NeuronClient), render-free.
- **PvE NPC** — *server-side* AI entity.
- **Safe zone** — radius around a base where PvP damage is off (high/low-sec defensive bubble).
- **Loot-on-kill** — destroyed *ships* drop a recoverable container with a fraction of fit/cargo.
- **Fleet command** — RTS-style direct control of a player's multi-ship fleet (Homeworld layer).
- **Mothership-base** — a player's single mobile **capital** base; *disable-not-destroy* (forced retreat at low HP, never permanently lost).
- **Fitting** — slotting modules onto a hull within a power/CPU budget; the join between tech tree and combat.
- **Damage-type counters** — kinetic/thermal/EM vs per-layer resists; the right fit beats a bigger wrong fit.
- **Security tier** — high (no PvP) → low (PvP, no claims) → null (PvP + claimable territory).
- **Territory structure** — claimable nullsec object with an owner, upkeep, and yield.
- **Anomaly / expedition** — scannable procedural PvE site (combat/exploration/salvage), scaling difficulty & loot.
- **Invasion** — server-driven NPC-faction incursion event that degrades a region until repelled.
- **Session token** — credential issued at login, validates reliable traffic.
- **Interest set / Baseline** — entities told to a player / last acked snapshot.
- **Floating origin** — per-frame render origin near the camera.
- **Sector** — fixed cubic cell for spatial queries & interest (keyed by `SectorId`).
- **Outbox** — durable, ordered queue of economy events drained to SQL (write-through).
- **RPO** — Recovery Point Objective: bounded data the system may lose on hard crash.
- **Canvas** — the 2D immediate-mode UI subsystem (separate from the 3D scene).
- **CMO** — VS "Compiled Mesh Object," output of the Mesh Content Pipeline.
- **NeuronAudio** — client-only audio library (XAudio2 + X3DAudio + WAV reader); not linked by ERHeadless.
- **XAudio2** — Microsoft low-level audio API (voice graph); v2.9 on Windows 10/UWP.
- **X3DAudio** — Microsoft 3D-audio helper; computes DSP settings (pan/volume/Doppler) for XAudio2 voices.
- **Submix / bus** — an XAudio2 voice that groups sources for shared volume/routing (Music/Ambient/SFX/UI).
- **WAV / RIFF** — uncompressed audio container; EarthRise uses **PCM-16 only**, via a custom reader.
- **Source / mastering voice** — XAudio2 objects: a playing sound / the final output mix.
- **Warp** — fast, sim-stepped (interdictable) travel to a scanned point within range.
- **Jump beacon** — a network node a jump drive jumps between; public (high-sec) or player/territory-owned (low/null).
- **Jump drive** — base/ship system for long-haul beacon-to-beacon jumps (fuel + spool-up + cooldown).
- **Interdiction** — disrupting an enemy's warp (tackle/warp-disruptor) to force an engagement.
- **Overview** — the sortable contact list used to manage targets in 3D space.
- **IFF** — Identify Friend/Foe: friend/neutral/hostile state (from standings), shown by shape + color.
- **Frames in flight** — number of frames the CPU may queue ahead of the GPU (EarthRise: 3, fence-synced).
- **Descriptor heap** — DX12 GPU-visible table of resource views (CBV/SRV/UAV/sampler); ring-sub-allocated per frame.
- **PSO** — Pipeline State Object: a compiled DX12 pipeline (shaders + fixed-function state), hash-cached.
- **Root signature** — DX12 binding layout (root constants/CBVs + descriptor tables); authored in HLSL (RS 1.1).
- **HDR forward** — forward rendering into a float HDR target, then bloom + tone-map (the Darwinia look).
- **Trim** — `IDXGIDevice3::Trim()`, releases GPU memory on UWP suspend.
- **String table** — id→text indirection enabling localization without code changes.
- **Event director** — server system that schedules invasions/anomalies on the world clock.

---

*End of DRAFT v0.12 — overview-driven control model (§23, EVE-Echoes-style; shared by
desktop & touch; box-select demoted; touch ambiguity designed out; R20 Med→Low; spec in
`docs/design/touch-controls.md`). Earlier: v0.11 completeness pass (DX12 §11.1, camera/
VFX §11.2, navigation §13.12, UI/HUD/radar §22, input §23, comms §24, platform services
§25, game-data §12.6, live-ops §26), v0.10 audio (NeuronAudio), v0.9 gameplay (§13).
M0, M1a, M1b complete; M2 is next on the engineering track.*
