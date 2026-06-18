# EarthRise — Master Implementation Plan

> **Status:** DRAFT v0.7 — for review
> **Date:** 2026-06-18
> **Scope:** A space 4X MMO with a custom C++23 engine (**NeuronCore**), a
> containerized Windows dedicated server (**ERServer**) backed by a networked
> Microsoft SQL Server, a UWP/DirectX 12 client, and a headless client/bot host
> (**ERHeadless**) — in the visual style of *Darwinia*.

---

## Changelog

**v0.7 (this revision) — engine/netcode review pass**
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

**EarthRise** is a persistent, single-shard space MMO. Each player commands one
**mobile home base** in a single contiguous universe: gather resources, build
ships, explore, expand, and fight (PvE **and** zoned PvP) — a real-time 4X loop
shared by ~100 concurrent players at launch.

**Pillars:** (1) one universe, one shard, signed `int64_t` coordinates; (2) the base
is a mobile unit, not a tile; (3) the Darwinia look — dark void, neon glow, bloom,
additive particles, minimalist bitmap-font HUD; (4) server-authoritative;
(5) custom engine on Microsoft platform tech only; (6) testable by construction
via headless clients/bots.

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
| Client prediction | **Deferred past M1** — interpolation + snap-on-ack first; predict/reconcile later. |
| Combat | **PvE** + **zoned PvP** (base safe-zones, loot-on-kill). |
| Meshes / Fonts | **CMO** meshes / **fixed-grid monospace** bitmap fonts (you provide both). |
| STL | **Allowed** (but tick hot path uses arena/pool/`pmr` allocators — §7.2). |
| Sim tick / snapshot | **30 Hz sim (~33.3 ms) / 20 Hz snapshot**; true fixed-step accumulator. |
| Persistence durability | **Economy = write-through / outbox; position = write-behind (RPO bounded).** |
| Dev / Prod | **Dev: Docker Desktop. Prod: Kubernetes** (Windows nodes + UDP LB). |
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
| Meshes / fonts | CMO (custom parser) / fixed-grid monospace atlas |
| Headless/bots | **ERHeadless** — many NeuronClient sessions, render-free |
| Tests | custom assert runner + ERHeadless multi-client harness + **record/replay determinism harness** |

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
ERHeadless) · **NeuronRender** (lib, UWP/DX12 → UWP app) · **ERServer** (exe) ·
**EarthRise.Client** (UWP/MSIX) · **ERHeadless** (exe) · **NeuronTools** (exes).

---

## 5. Repository Layout

```
/EarthRiseGame
├── masterplan.md   ·   EarthRise.sln   ·   docs/   ·   db/ (SQL schema+migrations)
├── NeuronCore/    math/ ecs/ world/ net/ serde/ sim/ platform/
├── NeuronClient/  session/ replica/ interp/ control/ bots/   (predict/ added later)
├── NeuronRender/  gfx/ scene/(3D) canvas/(2D) assets/(DDS,font,CMO)        [UWP]
├── ERServer/      netio/ simloop/ ai/(PvE) interest/ auth/ persist/(ODBC)
├── EarthRise.Client/  app/ ui/(HUD on CanvasRenderer)                      [UWP/MSIX]
├── ERHeadless/    host/ (N sessions; load + integration + replay tests)
├── NeuronTools/   meshcook/ ddscheck/ fontpack/ testrunner/
├── assets/        .dds · monospace font bitmaps · your .cmo meshes
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

---

## 13. Gameplay Systems (4X) — PvE & zoned PvP 🔒
- **eXplore** — sensor/fog range; discover resources, anomalies, NPCs, players.
- **eXpand** — the **mobile base** relocates, projecting range; later outposts.
- **eXploit** — nodes → harvested → base storage → **build queue** → ships/modules.
- **eXterminate:**
  - **PvE** — server AI entities (factions/creatures/hazards): patrol/aggro/flee/
    defend; objectives (defend/hunt/salvage).
  - **PvP (zoned)** — universe split into **PvP vs safe zones**; **each base
    projects a safe-zone radius** (no PvP damage inside); **loot-on-kill** drops a
    recoverable `LootContainer` with a fraction of cargo. Server-authoritative. Loot
    and currency transfers are **economy events** (write-through; §15).

**Entities:** `Base` (mobile; storage/shipyard/sensors/weapons; HP; safe-zone
emitter), `Ship` (💡 scout/harvester/fighter/builder), `NpcUnit`, `ResourceNode`,
`Projectile`, `LootContainer`, `Player`.

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
- **Schema (`/db/`, Azure-SQL-compatible):** accounts (§14), base state, build
  queues, ships, world objects, resource nodes, zones/PvP, **outbox**, **snapshots**.
  Avoid features absent in Azure SQL DB (cross-DB queries, SQL Agent jobs → Elastic
  Jobs, FILESTREAM). Versioned migrations.
- **App→DB auth:** **🔒 SQL login now → managed identity / Entra ID on Azure SQL.**
- **ERServer stateless** → restarts recover from snapshot + log.

---

## 16. Build, Tooling, Testing
- **MSBuild** 🔒 (`EarthRise.sln`): UWP `.vcxproj` → MSIX; others standard. The
  **`dxc` (DXCCompile) task** emits embedded SM6/DXIL shader headers (§12.4); asset
  cooking (CMO/DDS) runs as pre-build steps via the VS content pipeline.
- **Tests:** custom assert runner; units for math (DirectXMath), serialization,
  reliability (loss/reorder/dup), **crypto handshake (incl. MITM/nonce/replay
  cases)**, and CMO/DDS parsers. **ERHeadless** = multi-client integration & load
  harness plus an **input-log record/replay determinism harness** (same input →
  identical sim, the primary netcode debugging tool).
- **Per-milestone perf gates** are numeric: sim tick p50/p99 ms, per-client
  bandwidth, render frame time (§17, App. B).
- **CI / web sessions:** `SessionStart` hook builds NeuronCore/NeuronClient/
  ERServer/ERHeadless/NeuronTools and runs tests against a **dev SQL Server reached
  over the network** (UWP packaging stays local).

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
Scene + 2D Canvas HUD** as separate passes; snap-on-ack correction. **Done:** 1 UWP +
≥3 bots share the world; no `int64_t` reaches the GPU; 3D/2D split verified; loopback
path tested **and** a non-exempt run validated before Store.

**M2 — Darwinia look** *(M–L)* — DDS + **CMO** loaders, monospace Canvas HUD, bloom
+ additive particles, instanced ships. **Done:** an instanced fleet (your CMO
meshes) with thrusters + glow over a legible HUD at target frame time.

**M3 — Core 4X loop** *(L)* — nodes, harvesting, storage, build queue, sensor/fog.
**Done:** harvest → return → build a ship, server-authoritative.

**M4 — Scale & interest** *(L)* — sector subscriptions, delta compression, snapshot
job pool; **ERHeadless ~100 bots**. **Done:** 100-bot load test holds 30 Hz within
the bandwidth budget (App. B); per-client bandwidth measured vs the M0 estimate.

**M5 — Accounts, auth & persistence** *(M–L)* — real login (custom username/password,
PBKDF2 + pepper + rate-limit); ODBC persist layer + schema/migrations + **outbox
(write-through) + write-behind + snapshot/log warm-restart**; **SQL Server over the
network (self-hosted)**; ERServer stateless. **Done:** register/login works; kill &
restart the ERServer container → world + bases + economy restore with **zero economy
loss**.

**M6 — Combat, deployment & polish** *(L)* — weapons/projectiles (local sub-stepping
for fast shots), PvE AI, **zones + base safe-zones + loot-on-kill**; **Kubernetes
production deploy (Windows nodes + UDP LB)** and **Azure SQL migration**; optional
own-unit **prediction** where feel needs it; economy/UI polish; Store-compliance
pass. **Done:** full 4X session playable end-to-end by players + bots on the prod
topology.

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

---

## 19. Open Questions & Future Considerations

**Resolved this revision (v0.7):** sim tick = 30 Hz; coords = `int64` centred at 0
with struct sector key; client prediction deferred past M1; durability boundary
(economy write-through / position write-behind); shaders SM6/DXIL via `dxc`; M1 split
M1a/M1b; crypto hardening (server-key pinning, nonce/replay).

**Live unknowns (track, don't pretend they're closed):**
- **Per-client bandwidth budget** — estimated now (App. B) and *validated* at M4;
  this number gates the single-shard 100-player premise.
- Whether any subsystem actually needs **own-unit prediction** (decided by feel at M6).
- AES-GCM **rekey interval** vs packet rate; replay-window width.

**Deferred to post-launch:**
- Federated **Entra ID** / social login (launch is custom username/password).
- **Multi-shard** topology + directory/matchmaking service (launch is single shard).
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
| **Per-client downstream (estimate)** | **~100 visible entities × ~16 B delta × 20 Hz ≈ 32 KB/s (~256 kbit/s); validate at M4** |
| Per-client upstream | intents only, ≪ downstream |

> The downstream estimate is a *day-one sanity check* on the single-shard premise:
> ~256 kbit/s/client × 100 ≈ 25 Mbit/s aggregate egress — comfortably within a single
> pod. Delta size and visible-entity count are the variables to confirm at M4.

## Appendix C — Glossary
- **Shard** — one server process / one contiguous world.
- **Bot** — automated *client* session (NeuronClient), render-free.
- **PvE NPC** — *server-side* AI entity.
- **Safe zone** — radius around a base where PvP damage is off.
- **Loot-on-kill** — destroyed units drop a recoverable container.
- **Session token** — credential issued at login, validates reliable traffic.
- **Interest set / Baseline** — entities told to a player / last acked snapshot.
- **Floating origin** — per-frame render origin near the camera.
- **Sector** — fixed cubic cell for spatial queries & interest (keyed by `SectorId`).
- **Outbox** — durable, ordered queue of economy events drained to SQL (write-through).
- **RPO** — Recovery Point Objective: bounded data the system may lose on hard crash.
- **Canvas** — the 2D immediate-mode UI subsystem (separate from the 3D scene).
- **CMO** — VS "Compiled Mesh Object," output of the Mesh Content Pipeline.

---

*End of DRAFT v0.7 — review pass applied; M0 underway (NeuronCore scaffolded).*
