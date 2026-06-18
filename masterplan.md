# EarthRise — Master Implementation Plan

> **Status:** DRAFT v0.6 — for review
> **Date:** 2026-06-18
> **Scope:** A space 4X MMO with a custom C++23 engine (**NeuronCore**), a
> containerized Windows dedicated server (**ERServer**) backed by a networked
> Microsoft SQL Server, a UWP/DirectX 12 client, and a headless client/bot host
> (**ERHeadless**) — in the visual style of *Darwinia*.

---

## Changelog

**v0.6 (this revision)**
- **Simulation tick set to 60 Hz** (snapshot stays 20 Hz), aligning the plan with
  `NeuronCore/platform/TimerCore.h`. Updates §2, §3, §7.2, §9, §17, Appendix B.
- **NeuronCore** scaffolded as a Windows Store static library from the provided
  template headers (`math/`, `platform/`) and added to `EarthRise.slnx`.

**v0.5**
- **Shaders precompiled to embedded byte-array headers** via the MSBuild HLSL
  Compiler task — Variable Name `g_p%(Filename)`, Header File Output
  `$(ProjectDir)CompiledShaders\%(Filename).h` (§12.4).
- **Login provider locked: custom username/password.**
- **App→DB auth locked:** SQL login now → managed identity / Entra ID on Azure SQL.
- SQL Server edition direction confirmed (self-hosted Developer/Standard now). **All
  open questions resolved** — architecture is fully specified for M0 (§19).

**v0.4** — SQL Server external (not containerized), self-hosted → Azure SQL; real
login + CNG crypto; dev Docker Desktop / prod Kubernetes; 20 Hz.
**v0.3** — ERServer/ERHeadless; metres; CMO; monospace fonts; STL; zoned PvP;
SQLite→SQL Server.
**v0.2** — NeuronCore/NeuronClient/NeuronRender/NeuronHeadless; 3D vs 2D;
DirectXMath; MSBuild; Server Core container; PvE+PvP; user meshes.

---

## 0. How to read this document

- **🔒 Locked** — decided (§2). **💡 Proposed** — my default. **❓ Open** — needs
  input (§19 — currently none).
- Custom C++23 throughout. The only non-engine code we rely on are **Microsoft
  platform components**: `cppwinrt`, **DirectXMath**, **DirectX 12**, **Winsock**,
  **ODBC / SQL Server**, and **Windows CNG** (crypto). No third-party libraries.

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
| Server OS / deploy | Windows only; **ERServer** in a **Windows Server Core container**. Winsock + IOCP. |
| Build system | **MSBuild** (`EarthRise.sln`); UWP → MSIX. |
| Network transport | Custom **reliable UDP**, **encrypted** (CNG handshake). |
| **Database** | **SQL Server over the network — not containerized.** Self-hosted (Developer/Standard) now → **Azure SQL** later. ODBC Driver 18. |
| **Authentication** | **Real login at launch — custom username/password**; CNG password hashing; encrypted handshake. |
| **App→DB auth** | **SQL login now → managed identity / Entra ID on Azure SQL.** |
| **Shaders** | Precompiled to embedded headers (MSBuild HLSL Compiler): Variable Name `g_p%(Filename)`, Header File Output `$(ProjectDir)CompiledShaders\%(Filename).h`. |
| Math | **DirectXMath**-based. |
| Coordinate scale | **1 unit = 1 metre.** |
| Core / client / render libs | **NeuronCore**, **NeuronClient** (render-agnostic), **NeuronRender** (DX12). |
| Headless host / bots | **ERHeadless**. |
| Client rendering | **3D Scene** and **2D Canvas (UI)** are separate subsystems. |
| Combat | **PvE** + **zoned PvP** (base safe-zones, loot-on-kill). |
| Meshes / Fonts | **CMO** meshes / **fixed-grid monospace** bitmap fonts (you provide both). |
| STL | **Allowed.** |
| Sim tick / snapshot | **60 Hz sim (~16.67 ms) / 20 Hz snapshot.** |
| Dev / Prod | **Dev: Docker Desktop. Prod: Kubernetes** (Windows nodes + UDP LB). |
| First milestone | Networked tech slice (§17, M1). |

### 🔒 Hard constraints (brief)

C++23 (MSVC, `/std:c++latest`); client **UWP + C++/WinRT + DX12**; one open world,
**`uint64_t` x/y/z**; one **movable base**/player; ~**100 players**; textures
**`.dds`**; fonts **bitmap** (you provide).

### Allow-list (Microsoft platform components only)

| Component | Used by | Notes |
| --- | --- | --- |
| C++/WinRT | UWP client | App model / WinRT interop |
| DirectXMath | all | Header-only (Windows SDK) |
| DirectX 12 / DXGI | UWP client | Rendering |
| Winsock | all | UDP sockets |
| ODBC (Driver 18) + SQL Server | ERServer | `sql.h`/`odbc32.lib` (SDK); driver installed in the container |
| Windows CNG (`bcrypt.h`) | ERServer + client | ECDH, AES-GCM, PBKDF2 hashing |
| MSBuild HLSL Compiler (`fxc`/`dxc`) | build-time | HLSL → embedded bytecode headers |
| STL | all | 🔒 allowed |

---

## 3. Technology Stack

| Layer | Choice |
| --- | --- |
| Language / build | C++23, MSVC; **MSBuild**; UWP → MSIX |
| Math | DirectXMath (`XMVECTOR`/`XMMATRIX`; store `XMFLOAT*`) |
| Server | **ERServer** — Win32 console in a Windows Server Core container; Winsock UDP + IOCP; **60 Hz** sim / **20 Hz** snapshot |
| Database | **SQL Server over the network** (self-hosted → Azure SQL); ODBC Driver 18 |
| Crypto | **Windows CNG** — ECDH handshake, AES-GCM, PBKDF2 password hashing |
| Transport | Custom **encrypted** reliable-UDP (§8) |
| Client app | `CoreApplication` + `IFrameworkView` (CoreWindow), C++/WinRT, no XAML |
| Rendering | DX12 + DXGI flip-model; **Scene (3D)** + **Canvas (2D)** split |
| Shaders | HLSL → **embedded byte-array headers** via MSBuild HLSL Compiler (var `g_p%(Filename)`, out `CompiledShaders\%(Filename).h`); no runtime HLSL on UWP |
| Meshes / fonts | CMO (custom parser) / fixed-grid monospace atlas |
| Headless/bots | **ERHeadless** — many NeuronClient sessions, render-free |
| Tests | custom assert runner + ERHeadless multi-client harness |

---

## 4. High-Level Architecture

```
                         EarthRise — single shard
  ┌────────────────────────────────┐        ┌──────────────────────────────────┐
  │  UWP CLIENT (EarthRise.Client)  │        │  ERServer (Win32 console)         │
  │  app shell: IFrameworkView      │        │  in Windows Server Core container │
  │  ┌───────────────────────────┐  │encrypt.│  ┌────────────────────────────┐  │
  │  │ NeuronRender (DX12)        │  │reliable│  │ Net (Winsock UDP+IOCP):    │  │
  │  │  ├ SceneRenderer  (3D)     │  │  UDP   │  │ CNG handshake, reliability,│  │
  │  │  └ CanvasRenderer (2D UI)  │  │◀──────▶│  │ frag, acks, auth/session   │  │
  │  └───────────┬───────────────┘  │packets │  └─────────────┬──────────────┘  │
  │  ┌───────────▼───────────────┐  │snap/   │  ┌─────────────▼──────────────┐  │
  │  │ NeuronClient (lib)         │  │deltas  │  │ Simulation (60 Hz, auth.   │  │
  │  │  session/replica/predict/  │◀─┼───────▶│  │ ECS, PvE AI, PvP, interest)│  │
  │  │  interp/controller(human)  │  │        │  └─────────────┬──────────────┘  │
  │  └────────────────────────────┘  │        │  ┌─────────────▼──────────────┐  │
  └────────────────────────────────┘        │  │ Persistence (ODBC, write-  │  │
                 ▲                            │  │ behind, accounts) ─────┐   │  │
  ┌──────────────┴─────────────────┐ encrypt.│  └────────────────────────┼───┘  │
  │ ERHeadless (exe)               │reliable │                           │      │
  │  N× NeuronClient sessions,     │  UDP    └───────────────────────────┼──────┘
  │  bot / scripted (no rendering) │◀────────▶          TCP 1433 (ODBC,  │
  └──────────────┬─────────────────┘                     Encrypt=yes)    ▼
                 ▼                            ┌───────────────────────────────────┐
   ┌──────────────────────────────────────┐  │ Microsoft SQL Server (EXTERNAL,   │
   │ NeuronCore (linked by ALL):          │  │ NETWORK SERVICE — not a container)│
   │ math(DirectXMath)·ECS·world(uint64)· │  │ self-hosted host/VM now           │
   │ sectors·net protocol·serde·sim rules │  │   → Azure SQL later               │
   └──────────────────────────────────────┘  └───────────────────────────────────┘
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
├── NeuronClient/  session/ replica/ predict/ control/ bots/
├── NeuronRender/  gfx/ scene/(3D) canvas/(2D) assets/(DDS,font,CMO)        [UWP]
├── ERServer/      netio/ simloop/ ai/(PvE) interest/ auth/ persist/(ODBC)
├── EarthRise.Client/  app/ ui/(HUD on CanvasRenderer)                      [UWP/MSIX]
├── ERHeadless/    host/ (N sessions; load + integration tests)
├── NeuronTools/   meshcook/ ddscheck/ fontpack/ testrunner/
├── assets/        .dds · monospace font bitmaps · your .cmo meshes
├── shaders/       .hlsl sources → MSBuild HLSL Compiler → CompiledShaders/*.h (embedded)
└── deploy/        ERServer Dockerfile (Server Core) · k8s manifests · dev scripts
```
> Generated `CompiledShaders/` headers live under each project that compiles shaders
> (per `$(ProjectDir)`), so they're picked up by `#include` at build time.

---

## 6. Coordinate System & World Model

### 6.1 Absolute position — `uint64_t` per axis, **1 unit = 1 metre** 🔒
```cpp
struct WorldPos { uint64_t x, y, z; };   // absolute metres, unsigned
```
- Extent ≈ **2⁶⁴ m ≈ 1949 light-years** per axis.
- **Sub-metre motion:** each moving entity carries a server-side `float` residual
  per axis; the integer `WorldPos` advances when it crosses 1 m. Clients
  predict/interpolate in float, so visuals stay smooth below 1 m/tick.
- Corner origin `(0,0,0)`; logical centre `2⁶³` for symmetric spawns.

### 6.2 Relative math
```cpp
inline int64_t axisDelta(uint64_t a, uint64_t b) { return int64_t(a - b); } // wrap-safe within ±2^63
```
Relative vectors use DirectXMath `float`/`double`; never raw `uint64_t` to the GPU.

### 6.3 Sectors & interest grid
`sector = pos >> S`, `local = pos & ((1<<S)-1)`, `key = morton3(...)`. **💡**
default **S = 14 → ~16 km sectors** (tunable to sensor range). Players subscribe to
nearby sectors; the server streams only those entities (§8.4).

### 6.4 Floating-origin rendering
Per frame, pick a render origin (camera sector corner); upload entities as
camera-relative `float3` metres; **rebase** when the camera travels far.

---

## 7. NeuronCore

### 7.1 Math — DirectXMath 🔒
Compute in `XMVECTOR`/`XMMATRIX`; store `XMFLOAT*` for tight ECS packing. **💡**
row-major, row-vector, right-handed (`*RH`). A `WorldPos` fixed-point layer bridges
uint64 ↔ float.

### 7.2 Other subsystems
- **ECS** (custom, data-oriented): 32-bit handles, packed archetypes, span systems;
  identical client/server.
- **Serialization:** versioned binary; bit-packing for the wire; same primitives
  for warm-restart snapshots.
- **Time:** fixed sim step **🔒 60 Hz (~16.67 ms)**; **snapshots 20 Hz**; bounded
  catch-up; ticks are canonical. (See `platform/TimerCore.h`.)
- **Shared sim rules:** pure functions for movement, build costs, yields, combat
  (PvE+PvP) — used by both client prediction and server authority.

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
`ReliableUnordered` (notifications) · `Bulk` (fragmented world sync).

### 8.3 Reliability & encryption
16-bit per-channel sequences; **ack + 32-bit ack-bitfield**; RTT/RTO retransmit;
dup detection; **fragmentation/reassembly** (~1200 B safe payload); connection
token (anti-spoof); keepalive/timeout; congestion backoff.
**Encryption (required):** the handshake performs a **CNG ECDH key agreement**;
packets then use **AES-GCM (CNG)** AEAD — protecting the **login exchange** (§14)
and, ideally, all reliable traffic.

### 8.4 State replication
Per tick, each player gets a snapshot of **only their subscribed sectors**,
**delta-compressed vs the last acked baseline**. Clients **interpolate** remote
entities (~100 ms back) and **predict + reconcile** their own units. Input is
**intents/commands**; the server validates everything.

### 8.5 Connection sequence
1. UDP handshake + **CNG ECDH** → shared key (anti-spoof connection token).
2. **Login** (§14, custom username/password) over the encrypted channel → verify vs
   SQL → **session token**.
3. Initial world sync (`Bulk`) → enter the tick/snapshot loop.

---

## 9. ERServer
- Single Win32 console exe (one shard, ~100 players) in a **Windows Server Core
  container** (§20); **stateless** (durable state in SQL Server).
- **Threading (💡):** IOCP net threads → decode/reliability/decrypt → enqueue; a
  **single-threaded 60 Hz simulation** owns state; a **persistence thread** does
  write-behind via ODBC. MPSC queues.
- **Tick:** commands → systems (movement, harvest, build, **PvE AI**, **PvP**) →
  advance → per-player interest snapshots → net → periodic persistence batch.
- **PvE NPCs** are server ECS entities (`ERServer/ai/`), distinct from *bots*.
- DB is **out of the tick hot path** — SQL latency never stalls the sim.

---

## 10. Clients

### 10.1 NeuronClient (render-agnostic) 🔒
No rendering / no UWP dep; links into the UWP app **and** ERHeadless. Modules:
**session** (encrypted reliable-UDP, login, command queue), **replica** (entity
mirror), **predict** (prediction/reconciliation/interpolation), **control**
(`IClientController` → intents: human / bot / scripted).

### 10.2 EarthRise.Client (UWP)
C++/WinRT shell (`IFrameworkView`+CoreWindow, no XAML). Loop: input → human
controller → step NeuronClient → hand state to NeuronRender. DX12 swap chain via
`CreateSwapChainForCoreWindow` (pass the **command queue**;
`winrt::get_unknown(window)`); `winrt::com_ptr`/`check_hresult`; suspend/resume.

### 10.3 ERHeadless (parallel clients & bots) 🔒
Win32 host running **many NeuronClient sessions in one process** (each own UDP
port), bot/scripted-driven, **no rendering** → integration tests, load tests
(~100+ bots, §17 M4), and in-world bots.
> **Bots ≠ PvE NPCs.** Bots are *client* sessions; PvE NPCs are *server* AI.

---

## 11. Rendering — Scene (3D) vs Canvas (2D), Darwinia Look
Separate subsystems 🔒 (own modules/PSOs/command lists; composited last).
- **SceneRenderer (3D):** HDR scene (low-poly + bright emissive silhouettes,
  **instanced** ships) → bright-pass + Gaussian **bloom** (additive) → additive
  **particles** → tone-map (+ optional scanline/vignette/grain).
- **CanvasRenderer (2D UI):** immediate-mode orthographic pass — batched quads /
  monospace bitmap text / lines / rects; HUD & menus build on it, fully decoupled.

Shaders are precompiled into embedded byte-array headers (no runtime HLSL on UWP);
see §12.4.

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
`shaders/*.hlsl` are built by the **MSBuild HLSL Compiler task** with:
- **Variable Name** → `g_p%(Filename)`
- **Header File Output** → `$(ProjectDir)CompiledShaders\%(Filename).h`

Each shader becomes a generated header declaring a bytecode array
`const BYTE g_p<Filename>[]`, `#include`d directly to build PSOs — **no runtime
shader file I/O** (ideal for UWP). Target DXIL/SM6 via `dxc` where available, else
`fxc`. Usage:
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
    recoverable `LootContainer` with a fraction of cargo. Server-authoritative.

**Entities:** `Base` (mobile; storage/shipyard/sensors/weapons; HP; safe-zone
emitter), `Ship` (💡 scout/harvester/fighter/builder), `NpcUnit`, `ResourceNode`,
`Projectile`, `LootContainer`, `Player`.

---

## 14. Accounts & Authentication 🔒 (real login at launch)
- **Accounts** in SQL Server: username, **salted PBKDF2 password hash via CNG**
  (per-user random salt, high iteration count — never plaintext), profile,
  created/last-login, status.
- **Registration & login** over the **encrypted** channel (§8.5): credentials sent
  only after the ECDH handshake; server verifies and issues an expiring **session
  token** bound to the connection.
- **Sessions:** token validation on reliable traffic; reconnect support; **one
  active session per account** (deny/kick duplicate); login binds the session to the
  player's `Base`/entity.
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
  **connection pooling**; **batched write-behind** on the persistence thread;
  TVP/`bcp` for big checkpoints; **`Encrypt=yes`**.
- **Schema (`/db/`, Azure-SQL-compatible):** accounts (§14), base state, build
  queues, ships, world objects, resource nodes, zones/PvP. Avoid features absent in
  Azure SQL DB (cross-DB queries, SQL Agent jobs → Elastic Jobs, FILESTREAM).
  Versioned migrations.
- **App→DB auth:** **🔒 SQL login now → managed identity / Entra ID on Azure SQL.**
- **Durability:** SQL transactions + log are authoritative (**no custom journal**);
  an optional in-memory **snapshot** enables fast warm-restart.
- **ERServer stateless** → restarts recover from SQL.

---

## 16. Build, Tooling, Testing
- **MSBuild** 🔒 (`EarthRise.sln`): UWP `.vcxproj` → MSIX; others standard. The
  **HLSL Compiler task** emits embedded shader headers (§12.4); asset cooking
  (CMO/DDS) runs as pre-build steps via the VS content pipeline.
- **Tests:** custom assert runner; units for math (DirectXMath), serialization,
  reliability (loss/reorder/dup), **crypto handshake**, and CMO/DDS parsers.
  **ERHeadless** = multi-client integration & load harness.
- **CI / web sessions:** `SessionStart` hook builds NeuronCore/NeuronClient/
  ERServer/ERHeadless/NeuronTools and runs tests against a **dev SQL Server reached
  over the network** (UWP packaging stays local).

---

## 17. Milestone Roadmap

**M0 — Foundations** *(S–M)* — `EarthRise.sln`; NeuronCore skeleton (DirectXMath,
ECS, world, serde, time, logging); NeuronClient + ERHeadless skeletons; test
runner; HLSL-Compiler shader-header build wired. **Done:** all targets build;
NeuronCore tests pass.

**M1 — Networked tech slice** 🔒 *(L)* ← first — ERServer (containerized) 60 Hz
loop; reliable-UDP handshake (+ CNG encryption stub); **ERHeadless drives ≥3
parallel bots**; UWP client renders base + a few ships with **3D Scene + 2D Canvas
HUD** as separate passes; **move the base**; server-authoritative replication +
interpolation + basic prediction. **Done:** 1 UWP + ≥3 bots see the base cross a
sector boundary under simulated loss; no `uint64_t` reaches the GPU; 3D/2D split
verified.

**M2 — Darwinia look** *(M–L)* — DDS + **CMO** loaders, monospace Canvas HUD, bloom
+ additive particles, instanced ships. **Done:** an instanced fleet (your CMO
meshes) with thrusters + glow over a legible HUD at target frame rate.

**M3 — Core 4X loop** *(L)* — nodes, harvesting, storage, build queue, sensor/fog.
**Done:** harvest → return → build a ship, server-authoritative.

**M4 — Scale & interest** *(L)* — sector subscriptions, delta compression;
**ERHeadless ~100 bots**. **Done:** 100-bot load test holds 60 Hz within bandwidth
budget (App. B).

**M5 — Accounts, auth & persistence** *(M–L)* — **CNG encrypted handshake + real
login (custom username/password)**; ODBC persist layer + schema/migrations +
write-behind + warm-restart; **SQL Server over the network (self-hosted)**; ERServer
stateless. **Done:** register/login works; kill & restart the ERServer container →
world + bases restore from SQL.

**M6 — Combat, deployment & polish** *(L)* — weapons/projectiles, PvE AI, **zones +
base safe-zones + loot-on-kill**; **Kubernetes production deploy (Windows nodes +
UDP LB)** and **Azure SQL migration**; economy/UI polish; Store-compliance pass.
**Done:** full 4X session playable end-to-end by players + bots on the prod
topology.

---

## 18. Risks & Mitigations

| # | Risk | Impact | Mitigation |
| --- | --- | --- | --- |
| R1 | UWP networking sandbox (caps, loopback) | High | §8.1; `ISocket`; headless Win32 dodges loopback; test Store path early. |
| R2 | `uint64_t` vs float GPU precision | Med | Floating origin + sector-local floats; sub-metre residual (§6). |
| R3 | Single-shard scaling to 100 | Med | Interest mgmt + delta compression; ERHeadless load tests (M4). |
| R4 | **External DB latency/availability** | Med | DB out of tick hot path; in-memory sim; write-behind; co-locate ERServer near SQL/Azure SQL. |
| R5 | **K8s + Windows containers + UDP** | Med | Windows node pool; **UDP-capable LoadBalancer**; one pod per shard + **client→pod affinity** (§20). |
| R6 | Credential/session security | High | CNG ECDH + AES-GCM; PBKDF2 hashing; tokens; `Encrypt=yes` to SQL. |
| R7 | Azure SQL feature parity | Low–Med | Keep schema Azure-SQL-compatible from day one (§15). |
| R8 | "Custom everything" scope | High | Only MS platform components; strict milestone scoping. |
| R9 | UWP no runtime HLSL | Low | Embedded shader headers from day one (§12.4). |
| R10 | Reliable-UDP / crypto correctness | Med | Loss/reorder/dup + handshake tests; ERHeadless harness. |
| R11 | CMO edge cases (skinning/anim) | Low–Med | Static meshes first; add skinning later; validate via meshcook. |

---

## 19. Open Questions & Future Considerations

**No open blocking questions — the architecture is fully specified for M0.**

Deferred to post-launch (not needed now):
- Federated **Entra ID** / social login (launch is custom username/password).
- **Multi-shard** topology + directory/matchmaking service (launch is single shard).
- Encrypting **all** traffic vs the auth exchange only (handshake supports either).
- Azure SQL **tier sizing**, read-replicas, geo-redundancy.

*(Resolved through v0.5: coordinate scale = m; CMO meshes; monospace fonts; STL;
zoned PvP; SQL = external network service (self-hosted Developer/Standard → Azure
SQL); real login (custom username/password); SQL login → managed identity on Azure;
dev Docker Desktop / prod Kubernetes; 60 Hz sim / 20 Hz snapshot; shader precompile to embedded headers.)*

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

## Appendix A — Packet format sketch
```
UDP datagram (post-handshake: AES-GCM encrypted payload + auth tag)
├── Header: protocol_id u32 · connection_token u32 · sequence u16 · ack u16 · ack_bits u32
└── Payload: 1..N messages { channel u8, msg_type u8, length u16, body … }
   (fragments: message_id, fragment_index, fragment_count)
```

## Appendix B — Tick & timing budget
| Quantity | Target |
| --- | --- |
| Sim tick | 60 Hz (~16.67 ms) |
| Snapshot send / client | 20 Hz |
| Client render | display-rate (60+ fps), decoupled |
| Interpolation delay | ~100 ms |
| Safe UDP payload | ~1200 bytes |
| Per-client downstream | TBD at M4 |

## Appendix C — Glossary
- **Shard** — one server process / one contiguous world.
- **Bot** — automated *client* session (NeuronClient), render-free.
- **PvE NPC** — *server-side* AI entity.
- **Safe zone** — radius around a base where PvP damage is off.
- **Loot-on-kill** — destroyed units drop a recoverable container.
- **Session token** — credential issued at login, validates reliable traffic.
- **Interest set / Baseline** — entities told to a player / last acked snapshot.
- **Floating origin** — per-frame render origin near the camera.
- **Sector** — fixed cubic cell for spatial queries & interest.
- **Canvas** — the 2D immediate-mode UI subsystem (separate from the 3D scene).
- **CMO** — VS "Compiled Mesh Object," output of the Mesh Content Pipeline.

---

*End of DRAFT v0.6 — architecture specified end-to-end; M0 underway (NeuronCore
scaffolded).*
