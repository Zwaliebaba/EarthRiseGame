# EarthRise — Master Implementation Plan

> **Status:** DRAFT v0.3 — for review
> **Date:** 2026-06-18
> **Scope:** A space 4X MMO with a custom C++23 engine (**NeuronCore**), a
> containerized Windows dedicated server (**ERServer**) backed by Microsoft SQL
> Server, a UWP/DirectX 12 client, and a headless client/bot host (**ERHeadless**)
> — in the visual style of *Darwinia*.

---

## Changelog

**v0.3 (this revision)**
- Server exe renamed **NeuronServer → ERServer**; headless host **NeuronHeadless →
  ERHeadless**.
- Coordinate scale **locked to 1 unit = 1 metre** (§6.1).
- Mesh source format **locked to CMO** (VS Mesh Content Pipeline); custom CMO
  parser, no DirectXTK (§12.3).
- Font bitmaps **locked to fixed-grid monospace** (§12.2).
- **STL locked as allowed** (§2 A1).
- PvP rules **locked: zoned PvP, safe zones around bases, loot-on-kill** (§13).
- Persistence **changed from SQLite to Microsoft SQL Server** via ODBC Driver 18;
  ERServer becomes stateless, SQL Server runs as a separate service (§14, §19).

**v0.2** — NeuronCore rename; NeuronClient/NeuronRender/NeuronHeadless split; 3D
Scene vs 2D Canvas; DirectXMath; MSBuild; Windows Server Core container; PvE+PvP;
user-provided meshes.

---

## 0. How to read this document

- **🔒 Locked** — decided (§2). **💡 Proposed** — my default. **❓ Open** — needs
  your input (§18).
- Custom C++23 throughout. The only non-first-party-engine code we rely on are
  **Microsoft platform components**: `cppwinrt`, **DirectXMath**, **DirectX 12**,
  **Winsock**, and **ODBC / Microsoft SQL Server**. No third-party libraries.

---

## 1. Vision & Design Pillars

**EarthRise** is a persistent, single-shard space MMO. Each player commands one
**mobile home base** in a single contiguous universe: gather resources, build
ships, explore, expand, and fight (PvE **and** zoned PvP) — a real-time 4X loop
shared by ~100 concurrent players at launch.

**Pillars:** (1) one universe, one shard, `uint64_t` coordinates; (2) the base is
a mobile unit, not a tile; (3) the Darwinia look — dark void, neon glow, bloom,
additive particles, minimalist bitmap-font HUD; (4) server-authoritative;
(5) custom engine on Microsoft platform tech only; (6) testable by construction
via headless clients/bots.

---

## 2. Locked Decisions & Constraints

### 🔒 Decisions

| Topic | Decision |
| --- | --- |
| Server OS / deploy | Windows only; **ERServer** runs in a **Windows Server Core container**. Winsock + IOCP. |
| Build system | **MSBuild** (one `EarthRise.sln`); UWP → MSIX. |
| Network transport | Custom **reliable UDP**. |
| **Persistence** | **Microsoft SQL Server** via **ODBC Driver 18**; ERServer stateless. |
| Math | **DirectXMath**-based. |
| **Coordinate scale** | **1 unit = 1 metre.** |
| Core library | **NeuronCore**. |
| Shared client library | **NeuronClient** (render-agnostic). |
| Headless host / bots | **ERHeadless** (parallel client & load testing + in-world bots). |
| Client rendering | **3D Scene** and **2D Canvas (UI)** are separate subsystems. |
| Combat | **PvE** + **zoned PvP** (safe zones around bases, loot-on-kill). |
| Meshes | **You provide**, in **CMO** format (VS Mesh Content Pipeline). |
| Fonts | **Fixed-grid monospace** bitmap atlases (you provide). |
| **STL** | **Allowed.** |
| First milestone | Networked tech slice (§16, M1). |

### 🔒 Hard constraints (brief)

C++23 (MSVC, `/std:c++latest`); client is **UWP + C++/WinRT + DX12**; one open
world, **`uint64_t` x/y/z**; one **movable base** per player; ~**100 players**;
textures **`.dds`**; fonts **bitmap** (you provide).

### Allow-list (Microsoft platform components only)

| Component | Used by | Notes |
| --- | --- | --- |
| C++/WinRT | UWP client | App model, WinRT interop |
| DirectXMath | all | Header-only, in the Windows SDK |
| DirectX 12 / DXGI | UWP client | Rendering |
| Winsock | all | UDP sockets |
| **ODBC (Driver 18) + SQL Server** | ERServer | `sql.h`/`odbc32.lib` in the Windows SDK; driver is a Microsoft component installed in the container |
| `dxc` (build-time) | tooling | HLSL → DXIL offline |
| **STL** | all | 🔒 allowed |

---

## 3. Technology Stack

| Layer | Choice |
| --- | --- |
| Language / build | C++23, MSVC; **MSBuild** (`EarthRise.sln`); UWP → MSIX |
| Math | DirectXMath (compute `XMVECTOR`/`XMMATRIX`; store `XMFLOAT*`) |
| Server | **ERServer** — Win32 console in a Windows Server Core container; Winsock UDP + IOCP |
| Database | **Microsoft SQL Server** (separate service); access via **ODBC Driver 18** |
| Transport | Custom reliable-UDP (§8) |
| Client app | `CoreApplication` + `IFrameworkView` (CoreWindow), C++/WinRT, no XAML |
| Rendering | DX12 + DXGI flip-model swap chain; **Scene (3D)** + **Canvas (2D)** split |
| Shaders | HLSL → DXIL, precompiled offline (no runtime HLSL on UWP) |
| Meshes | **CMO** (custom parser) |
| Fonts | fixed-grid monospace bitmap atlas |
| Headless/bots | **ERHeadless** — many NeuronClient sessions, render-free |
| Tests | custom assert runner + ERHeadless multi-client harness |

---

## 4. High-Level Architecture

```
                         EarthRise — single shard
  ┌────────────────────────────────┐        ┌──────────────────────────────────┐
  │  UWP CLIENT (EarthRise.Client)  │        │  ERServer (Win32 console)         │
  │  app shell: IFrameworkView      │        │  in Windows Server Core container │
  │  ┌───────────────────────────┐  │ custom │  ┌────────────────────────────┐  │
  │  │ NeuronRender (DX12)        │  │reliable│  │ Net (Winsock UDP + IOCP):  │  │
  │  │  ├ SceneRenderer  (3D)     │  │  UDP   │  │ reliability, frag, acks    │  │
  │  │  └ CanvasRenderer (2D UI)  │  │◀──────▶│  └─────────────┬──────────────┘  │
  │  └───────────┬───────────────┘  │packets │  ┌─────────────▼──────────────┐  │
  │  ┌───────────▼───────────────┐  │snap/   │  │ Simulation (fixed tick,    │  │
  │  │ NeuronClient (lib)         │  │deltas  │  │ authoritative ECS, PvE AI, │  │
  │  │  session/replica/predict/  │  │◀──────▶│  │ PvP, interest)             │  │
  │  │  interp/controller(human)  │  │        │  └─────────────┬──────────────┘  │
  │  └───────────┬───────────────┘  │        │  ┌─────────────▼──────────────┐  │
  └──────────────┼─────────────────┘        │  │ Persistence (ODBC, write-  │  │
                 │                            │  │ behind) ───┐               │  │
  ┌──────────────┼─────────────────┐ custom  │  └────────────┼───────────────┘  │
  │ ERHeadless (exe)               │reliable │               │ TCP 1433 (ODBC)  │
  │  N× NeuronClient sessions,     │  UDP    └───────────────┼──────────────────┘
  │  controller = bot / scripted   │◀───────▶                ▼
  │  (no rendering)                │        ┌───────────────────────────────────┐
  └──────────────┬─────────────────┘        │ Microsoft SQL Server (SEPARATE):  │
                 │                            │ Linux container / VM / Azure SQL  │
                 ▼                            │ (NOT a Windows container)         │
   ┌──────────────────────────────────────┐  │  durable system of record + vol  │
   │ NeuronCore (linked by ALL):          │  └───────────────────────────────────┘
   │ math(DirectXMath)·ECS·world(uint64)· │
   │ sectors·net protocol·serde·sim rules │
   └──────────────────────────────────────┘
```

**Targets**

| Target | Type | Links | Purpose |
| --- | --- | --- | --- |
| **NeuronCore** | static lib | — | Shared engine: math, ECS, world model, netcode, serde, sim rules |
| **NeuronClient** | static lib | NeuronCore | Render-agnostic client: session, replica, prediction/interp, controllers, bot AI |
| **NeuronRender** | static lib (UWP) | NeuronCore | DX12: SceneRenderer (3D) + CanvasRenderer (2D) + runtime asset loaders |
| **ERServer** | Win32 console exe | NeuronCore | Authoritative server (containerized); ODBC → SQL Server |
| **EarthRise.Client** | UWP app (MSIX) | NeuronClient, NeuronRender | Graphical game client |
| **ERHeadless** | Win32 console exe | NeuronClient | Many client sessions (bots/scripted): parallel & load testing, in-world bots |
| **NeuronTools** | Win32 console exes | NeuronCore | Asset cookers (CMO/DDS/font), shader build, test runner |

---

## 5. Repository Layout

```
/EarthRiseGame
├── masterplan.md
├── EarthRise.sln                     ← MSBuild solution
├── docs/                             protocol spec, DB schema, ADRs
├── NeuronCore/        (static lib)
│   ├── math/   ecs/   world/   net/   serde/   sim/   platform/
├── NeuronClient/      (static lib, render-agnostic)
│   ├── session/  replica/  predict/  control/  bots/
├── NeuronRender/      (static lib, UWP/DX12)        ← graphical client only
│   ├── gfx/   scene/ (3D)   canvas/ (2D UI)   assets/ (DDS, font, CMO)
├── ERServer/          (Win32 console exe; containerized)
│   ├── netio/  simloop/  ai/ (PvE)  interest/  persist/ (ODBC → SQL Server)
├── EarthRise.Client/  (UWP app, MSIX)               ← NeuronClient + NeuronRender
│   ├── app/   ui/ (HUD on CanvasRenderer)
├── ERHeadless/        (Win32 console exe)            ← NeuronClient
│   └── host/   (spins up N sessions; load + integration tests)
├── NeuronTools/       meshcook/ ddscheck/ fontpack/ shaderbuild/ testrunner/
├── assets/            .dds textures, monospace font bitmaps, your .cmo meshes
├── shaders/           .hlsl + build → shaders/bin (DXIL)
├── deploy/            ERServer Dockerfile (Server Core), SQL Server compose, scripts
└── db/                SQL schema + migration scripts
```

---

## 6. Coordinate System & World Model

### 6.1 Absolute position — `uint64_t` per axis, **1 unit = 1 metre** 🔒

```cpp
struct WorldPos { uint64_t x, y, z; };          // absolute metres, unsigned
constexpr double kMetresPerUnit = 1.0;          // 🔒 1 unit = 1 m
```

- Universe extent ≈ **2⁶⁴ m ≈ 1.84×10¹⁹ m ≈ 1949 light-years** per axis.
- **Sub-metre motion:** the authoritative grid is integer metres, so each moving
  entity carries a server-side `float` **sub-metre residual** per axis; the integer
  `WorldPos` advances when the residual crosses 1 m. Clients predict/interpolate in
  float, so visuals stay smooth even below 1 m/tick.
- Corner origin `(0,0,0)`; logical centre `2⁶³` available for symmetric spawns.

### 6.2 Relative math without overflow

```cpp
inline int64_t axisDelta(uint64_t a, uint64_t b) { return int64_t(a - b); } // wrap-safe within ±2^63
```
Relative vectors use DirectXMath `float`/`double` — never raw `uint64_t` to the GPU.

### 6.3 Sectors & interest grid

```
sector = pos >> S ; local = pos & ((1<<S)-1) ; key = morton3(sx,sy,sz)
```
**💡** `S` ≈ the gameplay sensor/interest radius; default **S = 14 → ~16 km
sectors** at 1 m/unit (tunable). Players subscribe to nearby sectors; the server
streams only entities in those sectors (§8.4) — the key to 100 players in one
open world.

### 6.4 Floating-origin rendering

The GPU never sees a `uint64_t`. Each frame the client picks a **render origin**
(camera sector corner) and uploads entities as camera-relative `float3` metres;
it **rebases** when the camera travels far, preserving float precision near the
player.

---

## 7. NeuronCore (shared library)

### 7.1 Math — DirectXMath-based 🔒
Compute with `XMVECTOR`/`XMMATRIX` (SIMD); **store** components as
`XMFLOAT2/3/4` / `XMFLOAT4X4` so they pack tightly into ECS arrays. **💡**
DirectXMath conventions: row-major, row-vector (`v*M`), **right-handed** (`*RH`
helpers). A thin `WorldPos` fixed-point layer (§6) bridges uint64 ↔ float.

### 7.2 Other subsystems
- **ECS** (custom, data-oriented): 32-bit handles (index+generation), packed
  archetype arrays, span-iterating systems; identical on client & server.
- **Serialization:** versioned binary; bit-packing for the wire; same primitives
  for warm-restart snapshots.
- **Time:** fixed sim step **💡 20 Hz (50 ms)**; tick numbers are canonical.
- **Shared sim rules:** pure functions for movement, build costs, yields, and
  **combat (PvE + PvP) damage** — one definition, used by client prediction and
  server authority.

---

## 8. Networking — Custom Reliable UDP

### 8.1 Transport per side (verified vs. current MS docs)
- **ERServer / ERHeadless:** raw **Winsock** UDP + **IOCP**.
- **UWP client:** Winsock works in UWP (or `DatagramSocket`); both behind a thin
  `ISocket`. **💡 default = Winsock UDP**.
- **Manifest capabilities:** `internetClient` + `internetClientServer` and/or
  `privateNetworkClientServer`.
- **⚠️ Loopback isolation:** UWP packages are blocked from loopback by default;
  for local/containerized testing add `CheckNetIsolation.exe LoopbackExempt -a
  -n=<PackageFamilyName>` (VS auto-adds for debug; test without it before Store).
  ERHeadless (Win32) is exempt — ideal for fast netcode iteration.

### 8.2 Channels
`Unreliable` (snapshots) · `ReliableOrdered` (commands/chat/events) ·
`ReliableUnordered` (notifications) · `Bulk` (fragmented initial world sync).

### 8.3 Reliability
16-bit per-channel sequences; **ack + 32-bit ack-bitfield**; RTT/RTO retransmit;
dup detection; **fragmentation/reassembly** (safe payload ≈1200 B); handshake
with **connection token** (anti-spoof); keepalive/timeout; light congestion
backoff. Encryption deferred (handshake leaves room).

### 8.4 State replication
Per tick the server builds each player a snapshot of **only their subscribed
sectors**, **delta-compressed against the last acked baseline**. Clients buffer +
**interpolate** remote entities (~100 ms back) and **predict + reconcile** their
own base/ships. Input is **intents/commands**, never absolute state; the server
validates everything.

### 8.5 Security
Server-authoritative; validate & rate-limit commands; never trust client
positions; connection tokens; bound every field on ingest.

---

## 9. ERServer

- Single Win32 console exe (one shard, ~100 players) in a **Windows Server Core
  container** (§19); **stateless** — all durable state lives in SQL Server.
- **Threading (💡):** IOCP net threads → decode/reliability → enqueue; a
  **single-threaded fixed-tick simulation** owns game state; a **persistence
  thread** does write-behind via ODBC. MPSC queues for handoff.
- **Tick:** intake commands → systems (movement, harvesting, building, **PvE AI**,
  **combat/PvP**) → advance → per-player interest snapshots → net → periodic
  persistence batch.
- **PvE NPCs** are server-side ECS entities (`ERServer/ai/`), distinct from
  *bots* (client sessions, §10.3).
- The DB is **out of the tick hot path**: the sim runs from in-memory ECS; SQL
  latency/availability never stalls the tick.

---

## 10. Clients

### 10.1 NeuronClient (shared, render-agnostic) 🔒
No rendering, no UWP dependency, so it links into both the UWP app and ERHeadless:
- **session** — reliable-UDP client, connection lifecycle, command queue.
- **replica** — local mirror of replicated entities.
- **predict** — prediction, reconciliation, remote-entity interpolation.
- **control** — `IClientController` → intents; impls: **human** (UWP), **bot**
  (AI), **scripted** (tests).

### 10.2 EarthRise.Client (UWP graphical app)
UWP/C++WinRT shell (`IFrameworkView` + CoreWindow, no XAML). Loop: input → human
controller → step NeuronClient → hand state to **NeuronRender**. DX12 swap chain
via `CreateSwapChainForCoreWindow` (pass the **command queue**;
`winrt::get_unknown(window)`); `winrt::com_ptr`, `winrt::check_hresult`; handles
suspend/resume.

### 10.3 ERHeadless (parallel clients & bots) 🔒
Win32 console host running **many NeuronClient sessions in one process** (each its
own UDP source port), every session driven by a **bot** or **scripted** controller,
**no rendering**. Used for: automated integration tests (N clients assert on
replicated state), load testing (~100+ bots → §16 M4), and in-world bot
population.

> **Bots ≠ PvE NPCs.** Bots are automated *client* sessions (NeuronClient); PvE
> NPCs are *server* AI entities (§9).

---

## 11. Rendering — Scene (3D) vs Canvas (2D), the Darwinia Look

The two subsystems are **separate by design** 🔒 (separate modules, PSOs, command
recording; composited last).

**SceneRenderer (3D):** (1) HDR scene pass — low-poly meshes with bright emissive
silhouettes on near-black, **instanced** ship draws; (2) bright-pass + separable
Gaussian **bloom**, additive composite; (3) additive **particles** (thrusters,
weapons, sparks, explosions); (4) **tone-map** to back buffer + optional
scanline/vignette/grain.

**CanvasRenderer (2D UI):** an **immediate-mode 2D canvas** in its own
orthographic pass — batched textured quads / monospace bitmap text / lines / rects.
The HUD and menus (`EarthRise.Client/ui/`) build on it; fully decoupled from scene
state.

Shaders authored in HLSL, **compiled offline to DXIL** (no runtime HLSL on UWP),
loaded as PSO bytecode.

---

## 12. Asset Pipeline

### 12.1 Textures — `.dds`
Custom DDS parser (`DDS_HEADER` [+ `DXT10`]) → `DXGI_FORMAT`, BC1–BC7 + mips,
uploaded via upload heap. (The VS Image Content Pipeline also outputs `.dds`, so
authoring stays consistent with CMO material textures.)

### 12.2 Fonts — fixed-grid monospace 🔒
Atlas is a uniform grid; cell = `atlasW/cols × atlasH/rows`; codepoint → cell
(`col = (c-first)%cols`, `row = (c-first)/cols`); UVs computed directly — **no
metrics file**. Config: `cols, rows, firstCodepoint, cellPx`. Rendered by
CanvasRenderer as batched quads.

### 12.3 Meshes — CMO (custom parser) 🔒
You author meshes via the **Visual Studio Mesh Content Pipeline** (FBX/OBJ/DAE →
`.cmo`). Direct3D has **no built-in model loader**, and DirectXTK is off-limits, so
NeuronRender ships a **custom CMO reader** that parses, per mesh: the **material
list** (Blinn-Phong params + up to 8 texture slots; diffuse = our DDS), one or more
**vertex buffers** (position/normal/tangent/color/texcoord), optional **skinning
vertices**, **16-bit index buffers**, **submesh** records (material/VB/IB/start/
count), and optional **skeleton + animation** clips. `tools/meshcook` optionally
repacks CMO into an engine-native, instancing-friendly layout.

### 12.4 Shaders
`shaders/*.hlsl` → `dxc` (build step) → DXIL in `shaders/bin/`.

---

## 13. Gameplay Systems (4X) — PvE & zoned PvP 🔒

- **eXplore** — sensor/fog range around base & ships; discover resources,
  anomalies, NPCs, players.
- **eXpand** — the **mobile base** relocates, projecting ship/sensor range; later
  outposts/claims.
- **eXploit** — resource nodes → harvested by ships → base storage → **build
  queue** producing ships/modules.
- **eXterminate** — two modes:
  - **PvE** — server-side AI entities (hostile factions/creatures, hazards) with
    patrol/aggro/flee/defend behaviors; PvE objectives (defend/hunt/salvage).
  - **PvP (zoned)** 🔒 — the universe is partitioned into **PvP zones vs safe
    zones**; **each base projects a safe-zone radius** (no PvP damage inside);
    **loot-on-kill** — destroyed ships/bases drop a loot container with a fraction
    of cargo/resources that others can recover. Server-authoritative targeting and
    damage; weapons via shared NeuronCore sim rules.

**Entities:** `Base` (mobile; modules: storage, shipyard, sensors, weapons; HP;
safe-zone emitter), `Ship` (💡 scout, harvester, fighter, builder), `NpcUnit`,
`ResourceNode`, `Projectile`, `LootContainer`, `Player`.

---

## 14. Persistence — Microsoft SQL Server 🔒

- **System of record = Microsoft SQL Server** (a separate service, §19), accessed
  from ERServer via **ODBC Driver 18 for SQL Server** (`sql.h`/`sqlext.h`,
  `odbc32.lib` — Windows SDK). A thin custom ODBC wrapper lives in
  `ERServer/persist/`.
- **Schema (`/db/`):** accounts, base state (pos, modules, HP, inventory), build
  queues, owned ships, persistent world objects, resource nodes, PvP/zone state.
  Versioned migrations.
- **Access patterns:** parameterized statements / stored procedures; **connection
  pooling**; **batched write-behind** on the persistence thread; bulk operations
  (TVPs / `bcp`) for large checkpoints. Encrypted connection (`Encrypt=yes`).
- **Durability:** SQL Server transactions + its transaction log are authoritative
  — **no custom journal needed**. An **optional periodic binary snapshot** of
  in-memory state enables fast warm-restart of the simulation.
- **ERServer is stateless** → container restarts/redeploys recover by reading SQL
  Server. The persistent **volume belongs to SQL Server**, not ERServer.
- ❓ accounts/identity scope — §18.

---

## 15. Build, Tooling, Testing

- **Build = MSBuild** 🔒 (`EarthRise.sln`): `EarthRise.Client` is an
  AppContainer/UWP `.vcxproj` → MSIX; the rest are standard `.vcxproj`. Shader
  build + asset cooking run as pre-build steps. Mesh/texture authoring uses the VS
  content pipeline (CMO/DDS).
- **Tests:** tiny custom assert runner; unit tests for math (DirectXMath),
  serialization, reliability (simulated loss/reorder/dup), and the CMO/DDS
  parsers. **ERHeadless** provides the multi-client integration & load harness.
- **CI / web sessions:** a `SessionStart` hook can build NeuronCore / NeuronClient
  / ERServer / ERHeadless / NeuronTools and run tests; a **SQL Server Linux
  container** stands up the DB for integration tests (UWP packaging stays local).

---

## 16. Milestone Roadmap

### M0 — Foundations *(S–M)*
`EarthRise.sln`; NeuronCore skeleton (DirectXMath math, ECS, world model, serde,
time, logging); NeuronClient + ERHeadless skeletons; custom test runner;
shader/asset build steps.
**Done:** all targets build; NeuronCore tests pass in CI.

### M1 — Networked tech slice 🔒 *(L)* ← first milestone
ERServer (containerized) runs the fixed-tick loop; reliable-UDP handshake;
**ERHeadless drives several parallel bot clients**; the UWP client renders the
**base + a few ships** with **SceneRenderer (3D)** and a **CanvasRenderer (2D) HUD**
as separate passes; player **moves the base**; positions replicate
(server-authoritative) with interpolation + basic prediction.
**Done:** 1 UWP client + ≥3 headless bots see the base move smoothly across a
sector boundary under simulated packet loss; no raw `uint64_t` reaches the GPU
(floating origin verified); 3D/2D split verified.

### M2 — Darwinia look *(M–L)*
DDS loader, **CMO loader**, monospace Canvas HUD, bloom + additive particles,
instanced ships, tone-map/composite.
**Done:** an instanced fleet (your CMO meshes) with thruster particles and glowing
silhouettes at target frame rate over a legible bitmap-font HUD.

### M3 — Core 4X loop *(L)*
Resource nodes, harvesting, base storage, build queue; sensor/fog exploration.
**Done:** fly a harvester → gather → return → build a ship, server-authoritative.

### M4 — Scale & interest management *(L)*
Sector subscriptions, delta compression; **ERHeadless spins ~100 bot clients**.
**Done:** 100-bot load test holds tick rate within the per-client bandwidth budget
(Appendix B).

### M5 — Persistence (SQL Server) & deployment *(M)*
ODBC persist layer, schema + migrations, write-behind, warm-restart snapshot;
**SQL Server service stood up (Linux container/VM/managed)**; ERServer stateless.
**Done:** kill & restart the **ERServer container** mid-play → world and every base
restore from SQL Server.

### M6 — Combat (PvE + zoned PvP) & polish *(L)*
Weapons/projectiles, PvE AI, **zones + base safe-zones + loot-on-kill**, economy
tuning, UI polish, Store-compliance pass (test without loopback exemption).
**Done:** a full 4X session playable end-to-end by multiple players + bots.

---

## 17. Risks & Mitigations

| # | Risk | Impact | Mitigation |
| --- | --- | --- | --- |
| R1 | UWP networking sandbox (capabilities, loopback) | High | §8.1; `ISocket`; headless Win32 clients dodge loopback; test Store path early. |
| R2 | `uint64_t` precision vs float GPU | Med | Floating-origin rebasing + sector-local floats (§6.4); sub-metre residual (§6.1). |
| R3 | Single-shard scaling to 100 | Med | Interest mgmt + delta compression; ERHeadless load tests from M4. |
| R4 | **SQL Server not supported in Windows containers** | Med | Run SQL Server as a **Linux container / VM / managed (Azure SQL)**; ERServer connects over TCP 1433 (§19). |
| R5 | DB network dependency/latency | Med | DB out of the tick hot path; in-memory authoritative sim; write-behind batching. |
| R6 | "Custom everything" scope | High | Only Microsoft platform components; ruthless milestone scoping. |
| R7 | UWP can't runtime-compile HLSL | Low | Offline DXIL build from day one. |
| R8 | Reliable-UDP correctness | Med | Loss/reorder/dup tests + ERHeadless harness. |
| R9 | DirectXMath alignment in ECS | Low | Store `XMFLOAT*`, compute in `XMVECTOR` (§7.1). |
| R10 | CMO format edge cases (skinning/animation) | Low–Med | Start with static meshes; add skinning when needed; validate via `meshcook`. |

---

## 18. Open Questions (need your input)

1. **SQL Server hosting (§19):** managed **Azure SQL** vs self-hosted **Linux
   container** vs **VM/host**? Plus edition/version and auth (SQL auth vs Entra ID).
2. **Accounts/identity (§14):** real login at launch, or dev "pick a name" for now?
3. **Container orchestration (§19):** plain `docker run` / Compose / Kubernetes?
4. **Sim tick rate (§7.2):** 20 Hz OK, or 30 Hz?

*(Resolved since v0.2: coordinate scale = m; mesh = CMO; fonts = fixed-grid
monospace; STL allowed; PvP = zoned + base safe-zones + loot-on-kill.)*

---

## 19. Deployment & Containerization

🔒 **ERServer** runs in a **Windows Server Core container**; **SQL Server runs
separately** (Microsoft does **not** support SQL Server in Windows containers).

**ERServer container**
- Runtime image `mcr.microsoft.com/windows/servercore:<tag>` — **tag must match
  the container host's Windows build** (Windows containers need kernel/version
  compatibility; process isolation on a matching host, else Hyper-V isolation).
- MSBuild produces a self-contained ERServer on a build agent; `deploy/Dockerfile`
  **COPY**s it into the Server Core image. Console exe = entrypoint, logs to stdout.
- Publish the **UDP** game port (`-p <port>:<port>/udp`); reliable-UDP needs the
  host:port reachable by clients (NAT/firewall aware).
- **Stateless** — no game-state volume; config + SQL connection string via env
  vars / secret store.

**SQL Server**
- Run as a **Linux container** (`mcr.microsoft.com/mssql/server`, on a Linux host),
  a **VM/host**, or a **managed instance (Azure SQL)**. ❓ choice → §18.
- Its **data files live on a persistent volume**; ERServer connects over **TCP
  1433** via ODBC with `Encrypt=yes`.

**Topology:** ERServer (Windows container) + SQL Server (Linux/managed) on a shared
network; orchestration per §18. The split keeps ERServer disposable and lets the DB
scale/upgrade independently.

---

## Appendix A — Packet format sketch
```
UDP datagram
├── Header: protocol_id u32 · connection_token u32 · sequence u16 · ack u16 · ack_bits u32
└── Payload: 1..N messages { channel u8, msg_type u8, length u16, body … }
   (fragments carry message_id, fragment_index, fragment_count)
```

## Appendix B — Tick & timing budget (initial targets)
| Quantity | Target |
| --- | --- |
| Sim tick | 20 Hz (50 ms) |
| Snapshot send / client | 10–20 Hz |
| Client render | display-rate (60+ fps), decoupled |
| Interpolation delay | ~100 ms |
| Safe UDP payload | ~1200 bytes |
| Per-client downstream | TBD at M4 |

## Appendix C — Glossary
- **Shard** — one server process hosting one contiguous world.
- **Bot** — an automated *client* session (NeuronClient), render-free.
- **PvE NPC** — a *server-side* AI entity in the simulation.
- **Safe zone** — radius around a base where PvP damage is disabled.
- **Loot-on-kill** — destroyed units drop a recoverable loot container.
- **Interest set** — entities a player is currently told about.
- **Baseline** — last snapshot a client acked; deltas diff against it.
- **Floating origin** — per-frame render origin near the camera for float precision.
- **Sector** — fixed-size cubic cell for spatial queries & interest.
- **Canvas** — the 2D immediate-mode UI render subsystem (separate from the 3D scene).
- **CMO** — Visual Studio "Compiled Mesh Object," output of the Mesh Content Pipeline.

---

*End of DRAFT v0.3 — please review §2 and §18. I'll fold answers into v0.4.*
