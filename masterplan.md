# EarthRise — Master Implementation Plan

> **Status:** DRAFT v0.2 — for review
> **Date:** 2026-06-18
> **Scope:** A space 4X MMO with a custom C++23 engine (**NeuronCore**), a
> containerized Windows dedicated server, a UWP/DirectX 12 client, and a headless
> client/bot host — in the visual style of *Darwinia*.

---

## Changelog

**v0.2 (this revision)** — incorporates review feedback:
- Server now **runs in a Windows Server Core container** (§19 Deployment).
- Build is **MSBuild** throughout; CMake dropped (§3, §15).
- Core library renamed **NeuronCore**; new shared render-agnostic client library
  **NeuronClient**; new DX12 render library **NeuronRender**; headless client/bot
  host **NeuronHeadless** (§4, §5, §10).
- **Headless clients & bots** are first-class for parallel-client testing (§10.3).
- Client rendering **splits 3D Scene from 2D Canvas/UI** as separate subsystems
  (§11).
- Math is **DirectXMath-based** (§7.1).
- **PvE and PvP** are both in scope and specified (§13).
- **Meshes are provided by you** — locked; only the source format is open (§18).

---

## 0. How to read this document

This is a **proposal**. Three kinds of statements appear:

- **🔒 Locked** — decided (see §2). I build on these.
- **💡 Proposed** — my recommendation; the default unless you say otherwise.
- **❓ Open** — needs your input before locking. Collected in §18.

Everything is custom-built in C++23. Permitted external dependencies: `cppwinrt`
(client), `SQLite` (server), and `DirectXMath` (math, part of the Windows SDK).
See §2 for the full allow-list.

---

## 1. Vision & Design Pillars

**EarthRise** is a persistent, single-shard space MMO. Each player commands one
**mobile home base** in a single contiguous universe: gather resources, build
ships, explore, expand, and fight (PvE **and** PvP) — the real-time 4X loop shared
by ~100 concurrent players at launch.

**Pillars**

1. **One universe, one shard.** No instancing. A single open world addressed by
   `uint64_t` per axis.
2. **The base is a unit, not a tile.** It moves; reach and visibility define
   territory.
3. **Darwinia look & feel.** Dark void, glowing neon silhouettes, heavy bloom,
   additive particles, minimalist bitmap-font HUD.
4. **Server-authoritative.** Client predicts/interpolates; the server is truth.
5. **Custom everything** (bar the three allowed deps). We own engine, netcode,
   renderer, serialization, and tools.
6. **Testable by construction.** A render-agnostic client library lets us run
   many headless clients/bots in parallel for automated and load testing.

---

## 2. Locked Decisions & Constraints

### 🔒 Decisions

| Topic | Decision |
| --- | --- |
| **Server OS** | Windows only; **runs in a Windows Server Core container**. Winsock + IOCP. |
| **Build system** | **MSBuild** (Visual Studio solution). UWP packaged as MSIX. |
| **Network transport** | Custom **reliable UDP**. |
| **Persistence** | **SQLite** (+ snapshots + journal). |
| **Math** | **DirectXMath**-based. |
| **Core library** | **NeuronCore** (shared engine). |
| **Shared client library** | **NeuronClient** (render-agnostic; used by UWP app *and* headless). |
| **Headless clients / bots** | First-class via **NeuronHeadless**; for parallel-client & load testing and in-world bots. |
| **Client rendering** | **3D Scene** and **2D Canvas (UI)** are separate render subsystems. |
| **Combat** | Both **PvE** and **PvP** in scope. |
| **Meshes** | **You provide** client meshes (source format TBD — §18). |
| **First milestone** | Networked tech slice (§16, M1). |

### 🔒 Hard constraints (from the brief)

- **C++23** (MSVC, `/std:c++latest`). Client: **UWP** + **C++/WinRT** + **DX12**.
- Universe: one open world; positions are **`uint64_t` x, y, z**.
- One **movable base** per player; gather resources, build ships; 4X.
- ~**100 concurrent players** at launch.
- Textures **`.dds`**; fonts are **bitmap textures you provide**.

### Dependency allow-list

| Allowed | Used by | Notes |
| --- | --- | --- |
| C++/WinRT (`cppwinrt`) | UWP client | App model, WinRT interop |
| SQLite (amalgamation) | server | Single-file, compiled into NeuronServer |
| **DirectXMath** | all (math) | Header-only, ships with the Windows SDK |
| Windows SDK / Win32 | all | Winsock, threads, file I/O |
| DirectX 12, DXGI | UWP client | Rendering |
| `dxc` (build-time only) | tooling | HLSL → DXIL offline |

### ❓ Assumptions (correct me in §18)

- **A1. STL allowed.** "No third-party libs" = no *third-party* libraries; the C++
  Standard Library (`std::vector`, `<thread>`, `<atomic>`, `<chrono>`, `<span>`…)
  is used. Tell me if you want a from-scratch foundation instead.
- **A2.** The graphical client is a real **UWP/MSIX** package (headless clients
  and the server are plain Win32 console exes).

---

## 3. Technology Stack

| Layer | Choice |
| --- | --- |
| Language | C++23, MSVC |
| Build | **MSBuild** (one `EarthRise.sln`); UWP → MSIX |
| Math | **DirectXMath** (XMVECTOR/XMMATRIX; XMFLOAT* storage) |
| Server | Win32 console exe in a **Windows Server Core container**; Winsock UDP + IOCP |
| Transport | Custom reliable-UDP protocol (§8) |
| Client app model | `CoreApplication` + `IFrameworkView` (CoreWindow), C++/WinRT, no XAML |
| Client rendering | DX12 + DXGI flip-model swap chain; **Scene (3D)** and **Canvas (2D)** split |
| Shaders | HLSL → DXIL, precompiled offline (runtime HLSL compile unavailable to UWP) |
| Persistence | SQLite (WAL) + binary snapshots + event journal |
| Serialization | Custom versioned binary (wire + disk) |
| Headless/bots | **NeuronHeadless** runs many NeuronClient sessions, render-free |
| Tests | Tiny custom assert runner + headless loopback harness |

---

## 4. High-Level Architecture

```
                         EarthRise — single shard
  ┌────────────────────────────────┐        ┌──────────────────────────────────┐
  │  UWP CLIENT (EarthRise.Client)  │        │  SERVER (NeuronServer)            │
  │  app shell: IFrameworkView      │        │  in Windows Server Core container │
  │  ┌───────────────────────────┐  │        │  ┌────────────────────────────┐  │
  │  │ NeuronRender (DX12)        │  │ custom │  │ Net (Winsock UDP + IOCP):  │  │
  │  │  ├ SceneRenderer  (3D)     │  │reliable│  │ reliability, ordering,     │  │
  │  │  └ CanvasRenderer (2D UI)  │  │  UDP   │  │ fragmentation, acks        │  │
  │  └───────────┬───────────────┘  │◀──────▶│  └─────────────┬──────────────┘  │
  │  ┌───────────▼───────────────┐  │packets │  ┌─────────────▼──────────────┐  │
  │  │ NeuronClient (lib)         │  │        │  │ Simulation (fixed tick,    │  │
  │  │  session, replica, predict,│  │snap/   │  │ authoritative, ECS,        │  │
  │  │  interp, controller(human) │  │deltas  │  │ PvE AI, PvP, interest)     │  │
  │  └───────────┬───────────────┘  │◀──────▶│  └─────────────┬──────────────┘  │
  └──────────────┼─────────────────┘        │  ┌─────────────▼──────────────┐  │
                 │                            │  │ Persistence: SQLite +      │  │
  ┌──────────────┼─────────────────┐        │  │ snapshots + journal        │  │
  │ NeuronHeadless (exe)           │ custom │  └────────────────────────────┘  │
  │  N× NeuronClient sessions,     │reliable│             ▲                     │
  │  controller = bot / scripted   │  UDP   │             │ (mounted volume)    │
  │  (no rendering)                │◀──────▶│      ┌──────┴───────┐             │
  └──────────────┬─────────────────┘        │      │ world.db etc │             │
                 │                            └──────┴──────────────┴───────────┘
                 ▼
      ┌──────────────────────────────────────────────────────────┐
      │ NeuronCore (static lib, linked by ALL):                   │
      │  math (DirectXMath) · ECS · world/uint64 + sectors ·      │
      │  net protocol+reliability · serde · shared sim rules      │
      └──────────────────────────────────────────────────────────┘
```

**Targets**

| Target | Type | Links | Purpose |
| --- | --- | --- | --- |
| **NeuronCore** | static lib | — | Shared engine: math, ECS, world model, netcode protocol, serde, shared sim rules |
| **NeuronClient** | static lib | NeuronCore | Render-agnostic client: session, world replica, prediction/interp, controller interface, bot AI |
| **NeuronRender** | static lib (UWP) | NeuronCore | DX12 renderer: SceneRenderer (3D) + CanvasRenderer (2D), runtime asset loaders |
| **NeuronServer** | Win32 console exe | NeuronCore | Authoritative dedicated server; runs in a container |
| **EarthRise.Client** | UWP app (MSIX) | NeuronClient, NeuronRender | The graphical game client |
| **NeuronHeadless** | Win32 console exe | NeuronClient | Hosts many client sessions (bots/scripted); parallel & load testing, in-world bots |
| **NeuronTools** | Win32 console exes | NeuronCore | Asset cookers, shader build, test runner |

---

## 5. Repository Layout

```
/EarthRiseGame
├── masterplan.md
├── EarthRise.sln                     ← MSBuild solution
├── docs/                             protocol spec, ADRs, design notes
├── NeuronCore/        (static lib)
│   ├── math/          DirectXMath wrappers; WorldPos fixed-point helpers
│   ├── ecs/           handles, archetype storage, systems
│   ├── world/         WorldPos (uint64), sectors, interest grid, spatial queries
│   ├── net/           packet format, reliability, (de)frag, channels
│   ├── serde/         versioned binary read/write, bit-packing
│   ├── sim/           shared rules: movement, build costs, combat (PvE/PvP) math
│   └── platform/      time, logging, file I/O (Win32)
├── NeuronClient/      (static lib, render-agnostic)
│   ├── session/       transport client, connection, outgoing command queue
│   ├── replica/       client world mirror (replicated entity state)
│   ├── predict/       prediction, reconciliation, interpolation
│   ├── control/       IClientController (human | bot | scripted) → intents
│   └── bots/          bot AI controllers
├── NeuronRender/      (static lib, UWP/DX12)        ← graphical client only
│   ├── gfx/           device, swap chain, PSOs, descriptor heaps, frame ring
│   ├── scene/         SceneRenderer (3D): meshes, instancing, particles, bloom
│   ├── canvas/        CanvasRenderer (2D UI): quads, bitmap text, lines/rects
│   └── assets/        runtime loaders: DDS, bitmap-font, mesh
├── NeuronServer/      (Win32 console exe; containerized)
│   ├── netio/         Winsock + IOCP, connection table
│   ├── simloop/       fixed-tick authoritative loop, command intake
│   ├── ai/            PvE NPC behaviors (server-side AI entities)
│   ├── interest/      per-player relevance sets, delta/snapshot builder
│   └── persist/       SQLite, snapshot writer, journal, recovery
├── EarthRise.Client/  (UWP app, MSIX)               ← NeuronClient + NeuronRender
│   ├── app/           IFrameworkViewSource/View, lifecycle, input → controller
│   └── ui/            HUD/screens built on CanvasRenderer
├── NeuronHeadless/    (Win32 console exe)            ← NeuronClient
│   └── host/          spins up N sessions (bots/scripted); load + integration tests
├── NeuronTools/       (Win32 console exes)
│   ├── meshcook/  ddscheck/  fontpack/  shaderbuild/  testrunner/
├── assets/            source art: .dds, font bitmaps, your meshes
├── shaders/           .hlsl + build → shaders/bin (DXIL)
├── deploy/            Dockerfile (Windows Server Core), run/compose scripts
└── third_party/       sqlite amalgamation
```

---

## 6. Coordinate System & World Model

### 6.1 Absolute position — `uint64_t` per axis

```cpp
struct WorldPos { uint64_t x, y, z; };   // absolute, unsigned, per the brief
```

- **💡 Scale (default): 1 unit = 1 millimeter** (`kUnitsPerMeter = 1000`):

  | Unit | Extent / axis | Precision | Feel |
  | --- | --- | --- | --- |
  | 1 mm | ≈ 1.95 light-years | mm | tight, dense |
  | 1 cm | ≈ 19.5 light-years | cm | medium |
  | 1 m | ≈ 1949 light-years | m | galaxy-scale |

  ❓ **Open (§18):** which scale? Default mm.

- Corner origin `(0,0,0)`; unsigned matches the brief. Logical center `2⁶³`
  available for symmetric spawns.

### 6.2 Relative math without overflow

```cpp
inline int64_t axisDelta(uint64_t a, uint64_t b) { return int64_t(a - b); } // wrap-safe within ±2^63
```

Relative vectors use DirectXMath `float` (render/physics) or `double` (long range)
— never raw `uint64_t` arithmetic feeding the GPU.

### 6.3 Sectors & interest grid

```
sector = pos >> S ; local = pos & ((1<<S)-1) ; key = morton3(sx,sy,sz)
```

**💡** `S = 20` → ~1.05 km sectors at mm scale (tunable). Players subscribe to
nearby sectors; the server streams only entities in those sectors (§8.5) — the key
to 100 players in one open world.

### 6.4 Floating-origin rendering

The GPU never sees a `uint64_t`. Each frame the client picks a **render origin**
(camera sector corner) and uploads entities as small camera-relative `float3`
metres: `(pos − origin) / kUnitsPerMeter`; **rebase** when the camera travels far.

---

## 7. NeuronCore (shared library)

### 7.1 Math — DirectXMath-based 🔒

- Compute with `XMVECTOR`/`XMMATRIX` (SIMD); **store** components as `XMFLOAT2/3/4`
  / `XMFLOAT4X4` (unaligned) so they pack into ECS component arrays. Load → compute
  → store at use sites. This keeps SIMD fast *and* component layout tight.
- **💡** Adopt DirectXMath conventions: **row-major**, row-vector (`v * M`),
  **right-handed** world space using the `*RH` helpers (one-line switch to LH).
- A thin `WorldPos` fixed-point layer (§6) sits on top of DirectXMath for the
  uint64 ↔ float bridge.

### 7.2 Other NeuronCore subsystems

- **ECS (custom, data-oriented):** entities = 32-bit handles (index+generation);
  components in packed archetype arrays; systems iterate contiguous spans. Same
  ECS on client and server so layouts match.
- **Serialization:** versioned binary; bit-packing for the wire; same primitives
  for disk snapshots.
- **Time:** fixed sim step **💡 20 Hz (50 ms)**; tick numbers are canonical.
- **Shared sim rules:** pure functions for movement, build costs, resource yields,
  and **combat (PvE + PvP) damage** — defined once so prediction (client) and
  authority (server) agree.

---

## 8. Networking — Custom Reliable UDP

### 8.1 Transport per side (verified vs. current MS docs)

- **Server / NeuronHeadless:** raw **Winsock** UDP + **IOCP**.
- **UWP client:** Winsock is usable in UWP *or* `DatagramSocket`; both sit behind a
  thin `ISocket` so the reliability code is identical. **💡 default = Winsock UDP**,
  `DatagramSocket` fallback.
- **Manifest capabilities:** `internetClient` + `internetClientServer` and/or
  `privateNetworkClientServer` — else client networking silently fails.
- **⚠️ Loopback isolation (local dev):** UWP packages are blocked from loopback by
  default. To reach a locally/containerized server, add an exemption:
  `CheckNetIsolation.exe LoopbackExempt -a -n=<PackageFamilyName>` (VS auto-adds it
  for debug; must test without it before Store submission). Headless clients (Win32)
  are **not** subject to this — handy for local netcode iteration.

### 8.2 Channels

| Channel | Delivery | Carries |
| --- | --- | --- |
| `Unreliable` | fire-and-forget | high-frequency state snapshots |
| `ReliableOrdered` | acked, in-order | commands, chat, build/trade events |
| `ReliableUnordered` | acked, any order | one-off notifications |
| `Bulk` | acked, fragmented | large transfers (initial world sync) |

### 8.3 Reliability

16-bit per-channel sequences; **ack + 32-bit ack-bitfield**; RTT/RTO retransmit;
duplicate detection; **fragmentation/reassembly** (safe payload ≈1200 B);
handshake with **connection token** (anti-spoof); keepalive/timeout; light
congestion backoff. Encryption deferred (handshake leaves room for it).

### 8.4 State replication

Per tick, the server builds each player a snapshot of **only their subscribed
sectors** (§6.3), **delta-compressed against the last acked baseline**. Clients
buffer + **interpolate** remote entities (~100 ms back) and **predict +
reconcile** their own base/ships. Input is **intents/commands**, never absolute
state; the server validates everything.

### 8.5 Security

Server-authoritative; validate & rate-limit all commands; never trust
client-reported positions; connection tokens; bound every numeric field on ingest.

---

## 9. Server (NeuronServer)

- **Process:** single Win32 console exe (one shard, ~100 players) in a **Windows
  Server Core container** (§19).
- **Threading (💡):** IOCP net I/O threads → decode/reliability → enqueue; a
  **single-threaded fixed-tick simulation** owns all game state (race-free
  authority); a **persistence thread** does write-behind to SQLite. MPSC queues
  for handoff.
- **Tick:** intake commands → run systems (movement, harvesting, building,
  **PvE AI**, **combat/PvP**) → advance tick → build per-player interest snapshots
  → hand to net → periodic checkpoint.
- **PvE NPCs** are server-side ECS entities driven by `server/ai/` (distinct from
  *bots*, which are client sessions — see §10.3).
- **Capacity:** 100 in one shard now; sector-based sharding noted but out of scope.

---

## 10. Clients

### 10.1 NeuronClient (shared, render-agnostic) 🔒

The reusable client brain, with **no rendering and no UWP dependency** so it links
into both the UWP app and headless hosts:

- **session** — reliable-UDP client, connection lifecycle, outgoing command queue.
- **replica** — local mirror of replicated entities (the client's view of the
  world).
- **predict** — prediction, server reconciliation, remote-entity interpolation.
- **control** — `IClientController` produces intents; implementations: **human**
  (UWP input), **bot** (AI), **scripted** (tests).

### 10.2 EarthRise.Client (UWP graphical app)

UWP/C++WinRT app shell (`IFrameworkView` + CoreWindow, no XAML). Owns the loop:
poll input → drive a human `IClientController` → step NeuronClient → hand world
state to **NeuronRender**. DX12 bring-up uses `CreateSwapChainForCoreWindow` with
the **command queue** (D3D12 requirement) and `winrt::get_unknown(window)`;
`winrt::com_ptr`, `winrt::check_hresult`. Handles UWP suspend/resume.

### 10.3 NeuronHeadless (parallel clients & bots) 🔒

A Win32 console host that instantiates **many NeuronClient sessions in one process**
(each with its own UDP source port), every session driven by a **bot** or
**scripted** controller — **no rendering**. Uses:

- **Automated integration tests** — N clients connect, act, and assert on
  replicated state (e.g., everyone sees a base move across a sector boundary).
- **Load testing** — spin up ~100+ bot clients to validate tick rate and bandwidth
  (§16 M4).
- **In-world bots** — populate the live world with AI players.

> **Bots ≠ PvE NPCs.** Bots are automated *client sessions* (NeuronClient). PvE
> NPCs are *server* AI entities (§9). They share AI utility code via NeuronCore
> where sensible, but live on opposite sides of the wire.

---

## 11. Rendering — Scene (3D) vs. Canvas (2D), the Darwinia Look

The two render subsystems are **separate by design** 🔒 (separate modules,
pipeline states, command recording; composited at the end):

### 11.1 SceneRenderer (3D)
World rendering into an HDR target → the Darwinia pipeline:
1. **Scene pass (HDR):** low-poly meshes with bright emissive silhouettes on
   near-black; **instanced** draws for many ships.
2. **Bright-pass + bloom:** threshold → downsample → separable Gaussian → additive
   composite (the signature glow).
3. **Particles (additive):** thrusters, weapons fire, resource sparks, explosions.
4. **Tone-map** to the back buffer; optional subtle scanline/vignette/grain.

### 11.2 CanvasRenderer (2D UI)
An **immediate-mode 2D "canvas"** drawn in its own pass over the 3D scene:
orthographic, its own PSOs, batched **textured quads / bitmap-font text / lines /
rects**. The HUD and menus (`EarthRise.Client/ui/`) are built on this API. Because
it's decoupled, UI never entangles with scene state, and the scene can be swapped
or disabled independently.

Shaders authored in HLSL, **compiled offline to DXIL** (UWP can't compile HLSL at
runtime), loaded as bytecode into PSOs.

---

## 12. Asset Pipeline

- **Textures `.dds`:** custom parser (`DDS_HEADER` [+ `DXT10`]) → `DXGI_FORMAT`,
  BC1–BC7 + mips, uploaded via upload heap.
- **Fonts (your bitmaps):** paired with glyph metrics (❓ format §18); rendered as
  batched quads by CanvasRenderer.
- **Meshes (you provide) 🔒:** `tools/meshcook` converts your source format
  (❓ §18) into a compact binary mesh consumed by SceneRenderer.
- **Shaders:** `shaders/*.hlsl` → `dxc` (build step) → DXIL in `shaders/bin/`.

---

## 13. Gameplay Systems (4X) — incl. PvE & PvP 🔒

4X mapped onto "one mobile base + ships in one universe":

- **eXplore** — sensor/fog range around base & ships; discover resource fields,
  anomalies, NPCs, other players.
- **eXpand** — the base is **mobile**; expand by relocating, projecting
  ship/sensor range, and (later) outposts/claims.
- **eXploit** — resource nodes → harvested by ships → stored at base → spent via a
  **build queue** producing ships/modules.
- **eXterminate** — combat in two modes:
  - **PvE** — server-side AI entities: hostile factions/creatures, world hazards;
    behaviors (patrol, aggro, flee, defend); attack player bases/ships; PvE
    objectives (defend/hunt/salvage). Lives in `NeuronServer/ai/`.
  - **PvP** — player-vs-player combat between bases/ships; server-authoritative
    targeting and damage; weapon systems shared via NeuronCore sim rules.

**Entities:** `Base` (mobile; modules: storage, shipyard, sensors, weapons; HP),
`Ship` (💡 starter set: scout, harvester, fighter, builder), `NpcUnit`,
`ResourceNode`, `Projectile`, `Player`.

❓ **Open (§18):** PvP **rules** — where PvP is allowed (zoned vs. everywhere),
safe zones around bases, loot-on-kill — and combat feel (arcade vs. tactical).

---

## 14. Persistence

SQLite (WAL) durable store (accounts, base state, build queues, owned ships, world
objects, resource nodes) + periodic **binary snapshots** + append-only **event
journal** (recovery = latest snapshot + replay journal). All writes on the
persistence thread (write-behind). The DB + snapshots live on a **mounted container
volume** (§19) so state survives restarts. ❓ accounts/identity scope — §18.

---

## 15. Build, Tooling, Testing

- **Build = MSBuild** 🔒: one `EarthRise.sln`. `EarthRise.Client` is an
  AppContainer/UWP `.vcxproj` → **MSIX**; libraries and console exes are standard
  `.vcxproj`. Shader build + asset cooking run as pre-build steps / on demand.
- **Tests:** tiny custom assert runner (no gtest); unit tests for math (over
  DirectXMath), serialization, and the reliability layer (simulated
  loss/reorder/dup). **NeuronHeadless** provides the multi-client integration &
  load harness (§10.3).
- **CI / web sessions:** a `SessionStart` hook can build NeuronCore / NeuronClient
  / NeuronServer / NeuronHeadless / NeuronTools and run tests in cloud sessions
  (UWP packaging stays local). I'll wire this when we start coding.

---

## 16. Milestone Roadmap

Estimates are relative (S/M/L/XL).

### M0 — Foundations *(S–M)*
`EarthRise.sln`; NeuronCore skeleton (DirectXMath math, ECS, world model, serde,
time, logging); NeuronClient + NeuronHeadless skeletons; custom test runner;
shader/asset build steps.
**Done:** all targets build; NeuronCore unit tests pass in CI.

### M1 — Networked tech slice 🔒 *(L)*  ← first milestone
NeuronServer (containerized) runs the fixed-tick loop; reliable-UDP handshake;
**NeuronHeadless drives several parallel bot clients**; the UWP client renders the
**base + a few ships** with **SceneRenderer (3D)** and a **CanvasRenderer (2D) HUD**
as separate passes; player **moves the base**; positions replicate
(server-authoritative) with interpolation + basic prediction.
**Done:** 1 UWP client + ≥3 headless bots on one containerized server all see the
base move smoothly across a sector boundary under simulated packet loss; nothing
renders a raw `uint64_t` (floating origin verified); 3D/2D split verified.

### M2 — Darwinia look *(M–L)*
DDS loader, bitmap-font Canvas HUD, bloom + additive particles, instanced ships,
tone-map/composite.
**Done:** an instanced fleet with thruster particles and glowing silhouettes at
target frame rate over a legible bitmap-font HUD.

### M3 — Core 4X loop *(L)*
Resource nodes, harvesting, base storage, build queue producing ships; sensor/fog
exploration.
**Done:** fly a harvester → gather → return → build a ship from stored resources,
fully server-authoritative.

### M4 — Scale & interest management *(L)*
Sector subscriptions, delta compression vs. acked baselines; **NeuronHeadless spins
~100 bot clients** for load.
**Done:** 100-bot load test holds tick rate within the per-client bandwidth budget
(Appendix B).

### M5 — Persistence & deployment hardening *(M)*
SQLite store, snapshots, journal, recovery; container volume; accounts per §18.
**Done:** kill & restart the **container** mid-play → world and every base restore.

### M6 — Combat (PvE + PvP) & polish *(L)*
Weapons/projectiles, PvE NPC AI, PvP rules per §18, base defense, economy tuning,
UI polish, Store-compliance pass (test without loopback exemption).
**Done:** a full 4X session playable end-to-end by multiple players + bots.

---

## 17. Risks & Mitigations

| # | Risk | Impact | Mitigation |
| --- | --- | --- | --- |
| R1 | UWP networking sandbox (capabilities, loopback) | High | §8.1; `ISocket` abstraction; headless Win32 clients dodge loopback for iteration; test Store path early. |
| R2 | `uint64_t` precision vs. float GPU | Med | Floating-origin rebasing + sector-local floats (§6.4). |
| R3 | Single-shard scaling to 100 | Med | Interest mgmt + delta compression; NeuronHeadless load tests from M4. |
| R4 | **Windows container** UDP + host/image OS-version match | Med | Pin Server Core tag to host build; publish UDP port; validate in M1; document in `deploy/`. |
| R5 | "Custom everything" scope | High | STL assumed (A1); only 3 deps; ruthless milestone scoping. |
| R6 | UWP can't runtime-compile HLSL | Low | Offline DXIL build from day one. |
| R7 | Reliable-UDP correctness | Med | Loss/reorder/dup unit tests + headless harness. |
| R8 | DirectXMath alignment in ECS components | Low | Store `XMFLOAT*` (unaligned), compute in `XMVECTOR` (§7.1). |

---

## 18. Open Questions (need your input)

Ranked by impact. None block me refining the plan, but they shape detail:

1. **Coordinate scale (§6.1):** unit = **mm / cm / m**? Default mm.
2. **STL allowed? (A1)** Keep `std::`, or from-scratch foundation?
3. **Mesh source format (§12):** what format will you provide meshes in
   (glTF / FBX / OBJ / custom)? Determines the `meshcook` importer.
4. **Font descriptor (§12):** can you supply glyph metrics (`.fnt`/JSON/CSV), or
   are the bitmaps **fixed-grid monospace**?
5. **PvP rules (§13):** zoned vs. open PvP, safe zones around bases, loot-on-kill;
   combat feel (arcade vs. tactical).
6. **Accounts/identity (§14):** real login at launch, or dev "pick a name" for now?
7. **Sim tick rate (§7.2):** 20 Hz OK, or 30 Hz?
8. **Container specifics (§19):** preferred Windows Server Core base tag /
   orchestration (plain `docker run`, Compose, Kubernetes)?

---

## 19. Deployment & Containerization (NeuronServer)

🔒 The server runs in a **Windows Server Core container**.

- **Image:** runtime = `mcr.microsoft.com/windows/servercore:<tag>` (**tag must
  match the container host's Windows build**; Windows containers require
  kernel/version compatibility — process isolation on a matching host, else
  Hyper-V isolation). ❓ exact tag/orchestration — §18.
- **Build → run:** MSBuild produces a self-contained NeuronServer output on a build
  agent; `deploy/Dockerfile` **COPY**s that published output into the Server Core
  runtime image (avoids shipping heavy SDK build images). The console exe is the
  entrypoint and logs to stdout.
- **Networking:** publish the **UDP** game port (`docker run -p <port>:<port>/udp`);
  ensure the container network passes inbound UDP. Reliable-UDP needs the host:port
  reachable by clients (NAT/firewall aware).
- **Persistence:** SQLite DB + snapshots/journal on a **mounted volume** so state
  survives restarts/redeploys (ties to §14, M5).
- **Config:** env vars / config file — port, tick rate, world seed, snapshot
  interval, max players. Restart policy + log/heartbeat for health.

---

## Appendix A — Packet format sketch

```
UDP datagram
├── Header (fixed)
│   ├── protocol_id        u32   magic/version guard
│   ├── connection_token   u32   anti-spoof, assigned at handshake
│   ├── sequence           u16
│   ├── ack                u16
│   └── ack_bits           u32   acks the 32 packets before `ack`
└── Payload: 1..N messages
    └── Message { channel u8, msg_type u8, length u16, body … }
```
Fragments carry `{message_id, fragment_index, fragment_count}`.

## Appendix B — Tick & timing budget (initial targets)

| Quantity | Target |
| --- | --- |
| Sim tick rate | 20 Hz (50 ms) |
| Snapshot send / client | 10–20 Hz |
| Client render | display-rate (60+ fps), decoupled |
| Interpolation delay | ~100 ms |
| Safe UDP payload | ~1200 bytes |
| Per-client downstream | TBD at M4 |

## Appendix C — Glossary

- **Shard** — one server process hosting one contiguous world.
- **Bot** — an automated *client* session (NeuronClient), render-free.
- **PvE NPC** — a *server-side* AI entity in the simulation.
- **Interest set** — the entities a player is currently told about.
- **Baseline** — last snapshot a client acked; deltas diff against it.
- **Floating origin** — per-frame render origin near the camera for float precision.
- **Sector** — fixed-size cubic cell used for spatial queries & interest.
- **Canvas** — the 2D immediate-mode UI render subsystem (separate from the 3D scene).

---

*End of DRAFT v0.2 — please review §2 (decisions) and §18 (open questions). I'll
fold answers into v0.3.*
