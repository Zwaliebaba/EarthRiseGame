# EarthRise — Master Implementation Plan

> **Status:** DRAFT v0.1 — for review
> **Date:** 2026-06-18
> **Scope:** A space 4X MMO with a custom C++23 engine, a Windows dedicated
> server, and a UWP/DirectX 12 client, in the visual style of *Darwinia*.

---

## 0. How to read this document

This is a **proposal**, not a final spec. Three kinds of statements appear:

- **🔒 Locked** — decisions you already made (see §2). I build on these.
- **💡 Proposed** — my recommendation. Default unless you say otherwise.
- **❓ Open** — I need your input before it can be locked. Collected in §18.

Everything is custom-built in C++23. The **only** third-party dependencies are
`cppwinrt` (client) and `SQLite` (server persistence). See §2 for the full
allow-list and what I'm *assuming* is permitted (e.g. the C++ Standard Library).

---

## 1. Vision & Design Pillars

**EarthRise** is a persistent, single-shard space MMO. Each player commands one
**mobile home base** in a single contiguous universe. From that base they gather
resources, build ships, explore, expand their reach, and fight — the classic 4X
loop, but real-time and shared with up to ~100 concurrent players at launch.

**Pillars**

1. **One universe, one shard.** No instancing, no zones you load into. A single
   open world addressed by `uint64_t` coordinates per axis.
2. **The base is a unit, not a map tile.** It moves. Territory is defined by where
   you are and what you can see/reach, not by fixed plots.
3. **Darwinia look & feel.** Dark void, glowing neon vector-style silhouettes,
   heavy bloom, additive particles, minimalist bitmap-font HUD. Readable,
   atmospheric, low-poly-by-design — *not* photorealism.
4. **Server-authoritative.** The client predicts and interpolates; the server is
   the single source of truth. Cheating is a protocol problem, not a trust problem.
5. **Custom everything.** We own the engine, the netcode, the renderer, the
   serialization, and the tools. We learn and control the whole stack.

---

## 2. Locked Decisions & Constraints

### 🔒 Decisions (from your answers)

| Topic | Decision |
| --- | --- |
| **Server OS** | **Windows only.** Win32 console app, Winsock + IOCP. No OS-abstraction layer. |
| **Network transport** | **Custom reliable UDP** — our own reliability/ordering layer over UDP. |
| **Persistence** | **SQLite** permitted as the single extra dependency, alongside snapshots. |
| **First milestone** | **Networked tech slice** (see §16, M1). |

### 🔒 Hard constraints (from the brief)

- Language: **C++23** (MSVC, `/std:c++latest`).
- Client: **UWP** app using **C++/WinRT** and **DirectX 12**.
- Universe: one open world; positions are **`uint64_t` x, y, z**.
- One **movable base** per player; gather resources, build ships; 4X functionality.
- Target ~**100 concurrent players** at launch.
- Textures: **`.dds`**. Fonts: **bitmap textures you provide**.
- **No third-party libraries** except `cppwinrt` (and now `SQLite`).

### Dependency allow-list

| Allowed | Used by | Notes |
| --- | --- | --- |
| C++/WinRT (`cppwinrt`) | client | App model, WinRT interop |
| SQLite | server | Durable store (vendored as the single-file amalgamation, compiled into the server) |
| Windows SDK / Win32 | both | Winsock, threads, file I/O |
| DirectX 12, DXGI | client | Rendering |
| HLSL compiler `dxc` (build-time only) | tooling | Shaders precompiled offline to DXIL |

### ❓ Assumptions I'm making (correct me in §18)

- **A1.** The **C++ Standard Library / STL** is allowed (containers, `<thread>`,
  `<atomic>`, `<chrono>`, `<filesystem>`, `<span>`, etc.). "No libraries" means no
  *third-party* libraries, not "reimplement `std::vector`." If you want a
  from-scratch foundation (custom allocators/containers), that's a different and
  much larger effort — tell me.
- **A2.** **MSVC + Visual Studio** is the toolchain (required anyway for UWP/MSIX
  packaging). CMake optional for `core`/`server`/`tools`.
- **A3.** The client is a real **side-loaded / Store-style UWP package** (MSIX),
  not a Win32 desktop app. This matters a lot for networking (§8) — flagged as a
  risk in §17.

---

## 3. Technology Stack

| Layer | Choice |
| --- | --- |
| Language | C++23, MSVC |
| Build | Visual Studio solution (+ optional CMake for non-UWP targets) |
| Client app model | `CoreApplication` + `IFrameworkView` (CoreWindow), C++/WinRT, **no XAML** |
| Client rendering | Direct3D 12 + DXGI flip-model swap chain (`CreateSwapChainForCoreWindow`) |
| Shaders | HLSL → DXIL, **precompiled offline** (runtime HLSL compile is unavailable to UWP) |
| Server | Win32 console exe, Winsock UDP + IOCP, fixed-tick simulation |
| Transport | Custom reliable-UDP protocol (our design, §8) |
| Persistence | SQLite (WAL mode) + periodic binary snapshots + event journal |
| Serialization | Custom versioned binary (network + disk) |
| Math/ECS/etc. | Custom (shared `core` library) |
| Tests | Tiny custom assert-based runner (no gtest) |

---

## 4. High-Level Architecture

```
                        EarthRise — single shard
  ┌──────────────────────────────┐         ┌──────────────────────────────┐
  │   CLIENT  (UWP / C++/WinRT)   │         │   SERVER (Win32 console)     │
  │                              │         │                              │
  │  CoreWindow + IFrameworkView │         │  Winsock UDP + IOCP threads  │
  │  ┌────────────────────────┐  │  custom │  ┌────────────────────────┐  │
  │  │ DX12 Renderer (Darwinia│  │ reliable│  │ Net layer (reliability,│  │
  │  │  look: bloom, neon,    │  │   UDP   │  │  ordering, frag, acks) │  │
  │  │  instancing, particles)│  │◀───────▶│  └───────────┬────────────┘  │
  │  └────────────────────────┘  │ packets │              │ commands      │
  │  ┌────────────────────────┐  │         │  ┌───────────▼────────────┐  │
  │  │ Prediction / interp.   │  │snapshots│  │ Simulation (fixed tick,│  │
  │  │ Input, HUD, asset I/O  │  │◀───────▶│  │  authoritative, ECS)   │  │
  │  └────────────────────────┘  │  deltas │  └───────────┬────────────┘  │
  │            │                 │         │              │ state         │
  │  ┌─────────▼──────────────┐  │         │  ┌───────────▼────────────┐  │
  │  │  shared CORE library   │  │         │  │  shared CORE library   │  │
  │  └────────────────────────┘  │         │  └───────────┬────────────┘  │
  └──────────────────────────────┘         │              │               │
                                            │  ┌───────────▼────────────┐  │
            ┌───────────────┐               │  │ Persistence: SQLite +  │  │
            │  shared CORE   │ ── compiled ──┤  │ snapshots + journal    │  │
            │ math, ECS,     │   into both   │  └────────────────────────┘  │
            │ protocol, sim  │               └──────────────────────────────┘
            │ rules, serde   │
            └───────────────┘
```

**Three build targets + a shared library**

- **`core`** (static lib): math, ECS, world/coordinate model, serialization,
  protocol definitions, and **shared simulation rules**. Compiled into both
  client and server so they agree on data layout and game logic.
- **`server`** (Win32 console exe): networking, authoritative simulation loop,
  interest management, persistence.
- **`client`** (UWP app): rendering, input, prediction/interpolation, HUD,
  asset loading.
- **`tools`** (Win32 console exes): asset converters (DDS inspection, mesh
  cooker, font-atlas packer), shader build step, test runner.

---

## 5. Repository Layout

```
/EarthRiseGame
├── masterplan.md                  ← this document
├── docs/                          design notes, protocol spec, ADRs
├── core/                          shared static library (C++23)
│   ├── math/         vectors, matrices, quaternions, fixed-point world coords
│   ├── ecs/          entities, components, systems, archetype storage
│   ├── world/        WorldPos, sectors, interest grid, spatial queries
│   ├── net/          packet format, reliability, (de)fragmentation, channels
│   ├── serde/        versioned binary read/write, bit-packing
│   ├── sim/          shared game rules (movement, build costs, combat math)
│   └── platform/     time, logging, file I/O wrappers (Win32)
├── server/                        Win32 console dedicated server
│   ├── netio/        Winsock + IOCP, connection table, send/recv queues
│   ├── simloop/      fixed-tick authoritative loop, command intake
│   ├── interest/     per-player relevance sets, delta/snapshot builder
│   └── persist/      SQLite access, snapshot writer, recovery
├── client/                        UWP / C++/WinRT / DX12 app
│   ├── app/          IFrameworkViewSource/IFrameworkView, lifecycle, input
│   ├── gfx/          device, swap chain, PSOs, descriptor heaps, frame ring
│   ├── render/       passes: scene, bloom, particles, composite, HUD
│   ├── assets/       DDS loader, bitmap-font renderer, mesh loader
│   ├── netcli/       client transport, prediction, reconciliation, interp
│   └── ui/           immediate-mode HUD widgets (Darwinia-style)
├── tools/                         asset cookers, shader build, test runner
├── assets/                        source art (.dds, font bitmaps, meshes)
├── shaders/                       .hlsl sources + build script → /shaders/bin
└── third_party/                   sqlite amalgamation; cppwinrt is via SDK/NuGet
```

---

## 6. Coordinate System & World Model

This is the most distinctive requirement, so it gets its own section.

### 6.1 Absolute position — `uint64_t` per axis

```cpp
struct WorldPos {            // absolute, unsigned, per the brief
    uint64_t x, y, z;
};
```

- **💡 Scale (proposed default): 1 unit = 1 millimeter** (`kUnitsPerMeter = 1000`),
  a compile-time constant. Resulting universe size per axis:

  | Unit | Universe extent / axis | Precision | Feel |
  | --- | --- | --- | --- |
  | 1 mm | 2⁶⁴ mm ≈ 1.8×10¹⁶ m ≈ **1.95 light-years** | millimetre | tight, dense |
  | 1 cm | ≈ 19.5 light-years | centimetre | medium |
  | 1 m  | ≈ 1949 light-years | metre | galaxy-scale |

  ❓ **Open (§18):** which scale do you want? It changes how "big" space feels and
  how much sub-unit precision ships have. I default to **mm** for crisp local
  control; pick **m** if you want a galaxy-spanning feel.

- Origin is corner-based at `(0,0,0)`; unsigned matches the brief and avoids sign
  handling. A logical "center" constant (`2⁶³`) is available if we ever want
  symmetric spawn placement.

### 6.2 Relative math without overflow

Distances and directions need signed deltas. We compute them as `int64_t` and
guarantee correctness while two points are within half the address space (always
true in practice — entities that interact are close):

```cpp
inline int64_t axisDelta(uint64_t a, uint64_t b) {
    return static_cast<int64_t>(a - b);   // wrap-safe within ±2^63
}
```

Local/relative vectors use `float` (rendering, physics) or `double` (long-range
math) — never the raw `uint64_t` for arithmetic that feeds the GPU.

### 6.3 Sectors & the interest grid

Space is partitioned into a uniform grid of **sectors** for spatial queries and
network interest management:

```
sector_coord = pos >> S          // top (64-S) bits
local_offset = pos & ((1<<S)-1)  // low S bits, fits in a float comfortably
sector_key   = morton3(sx,sy,sz) // Z-order packed key for hashing/locality
```

- **💡 Proposed `S`:** sized so one sector is a sensible simulation/interest cell.
  At mm scale, `S = 20` → ~1.05 km sectors; tunable. Each player subscribes to the
  sectors near their base/ships/camera; the server only streams entities in those
  sectors (§8.5). This is what keeps 100 players affordable in one open world.

### 6.4 Floating-origin rendering

`float32` loses precision at large magnitudes, so the GPU never sees `uint64_t`
world positions. Each frame the client picks a **render origin** (the camera's
sector corner). Every visible entity is uploaded as a small camera-relative
`float3` in metres: `(entity.pos − renderOrigin) / kUnitsPerMeter`. When the
camera travels far enough, we **rebase** the origin. This keeps full float
precision wherever the player is actually looking.

---

## 7. Shared `core` Library

The contract both client and server compile against.

- **Math** — `Vec2/3/4`, `Mat4`, `Quat`, fixed-point `WorldPos` helpers, SIMD
  later if profiling demands. Right-handed, column-major (DX-friendly).
- **ECS (custom, data-oriented)** — entities are 32-bit handles
  (index + generation); components stored in tightly-packed arrays grouped by
  archetype; systems iterate contiguous component spans. No virtual-per-entity
  dispatch. Same ECS drives client and server so component layouts match.
- **Serialization** — versioned binary writer/reader with explicit schema
  versions; bit-packing for network payloads; the *same* primitives used for
  disk snapshots and wire messages.
- **Time** — fixed simulation step **💡 20 Hz (50 ms)** decoupled from client
  render rate; monotonic clocks; tick numbers are the canonical timeline.
- **Shared sim rules** — pure functions for movement integration, build costs,
  resource yields, combat damage. Defined once so prediction (client) and
  authority (server) compute identically.

---

## 8. Networking — Custom Reliable UDP

### 8.1 Why our own layer

UDP gives us low latency and no head-of-line blocking; we add exactly the
reliability we need (and nothing we don't). We layer **multiple channels** over a
single socket so movement, events, and bulk transfers don't block each other.

### 8.2 Transport on each side (verified against current MS docs)

- **Server:** raw **Winsock** UDP socket(s) with **IOCP** for scalable async I/O.
- **Client (UWP):** Winsock *is* usable inside a UWP app, **or** WinRT's
  `Windows.Networking.Sockets.DatagramSocket`. To keep the reliability/protocol
  code identical on both ends, the transport sits behind a thin `ISocket`
  interface; **💡 default backend = Winsock UDP** on both, with `DatagramSocket`
  as a fallback if Winsock-in-UWP causes friction.
- **Manifest capabilities (required):** `internetClient` plus
  `internetClientServer` and/or `privateNetworkClientServer`. Without these, all
  client networking silently fails.
- **⚠️ Local testing — loopback isolation.** UWP packages are **blocked from
  loopback by default**, so a locally-running server won't be reachable from the
  client until exempted:
  ```
  CheckNetIsolation.exe LoopbackExempt -a -n=<PackageFamilyName>
  ```
  Visual Studio auto-adds this for the deployed debug build ("Allow local network
  loopback"). We must also test *without* the exemption before any Store
  submission. This is captured as a dev-workflow step and a risk (§17).

### 8.3 Channels

| Channel | Delivery | Carries |
| --- | --- | --- |
| `Unreliable` | fire-and-forget | high-frequency state snapshots |
| `ReliableOrdered` | acked, in-order | commands, chat, build/trade events |
| `ReliableUnordered` | acked, any order | one-off notifications |
| `Bulk` | acked, fragmented | large transfers (initial world sync) |

### 8.4 Reliability mechanics

- 16-bit sequence numbers per channel; **ack + 32-bit ack-bitfield** per packet
  (acks the last 33 packets cheaply — Quake3/GAFE-style).
- RTT/RTO estimation for retransmit timing; duplicate detection.
- **Fragmentation/reassembly** for messages > MTU (target safe payload ≈1200 B).
- Connection handshake with a **connection token** (anti-spoofing); keepalive +
  timeout; graceful disconnect.
- Lightweight congestion control (send-rate backoff under loss).
- 💡 Encryption deferred (post-launch); design leaves room for a handshake-keyed
  stream cipher.

### 8.5 State replication model

- Server simulates at fixed tick, then per player builds a **snapshot of only the
  entities in their subscribed sectors** (§6.3 interest management).
- **Delta compression against the last acked baseline** (Source-engine style):
  send only what changed since the snapshot the client confirmed receiving.
- Client buffers snapshots and **interpolates** remote entities ~100 ms in the
  past for smoothness; **predicts** its own base/ship movement and
  **reconciles** when the authoritative state arrives.
- All player input travels as **intents/commands** ("set base heading",
  "queue ship build"), never as absolute state. The server validates every one.

### 8.6 Security posture

Server-authoritative; validate and rate-limit all commands; never trust
client-reported positions; connection tokens to deter spoofing; sanity-bound
every numeric field on ingest.

---

## 9. Server

- **Process:** single Win32 console exe (one shard, ~100 players to start).
- **Threading model (💡 proposed):**
  - *Net I/O threads* — IOCP completion handlers: receive, decrypt-later,
    reliability bookkeeping, enqueue decoded messages.
  - *Simulation thread* — single-threaded fixed-tick loop that drains command
    queues, runs ECS systems, advances the world. Single-threaded sim keeps the
    authority simple and race-free; we parallelize *within* a tick later only if
    profiling requires it.
  - *Persistence thread* — write-behind snapshots/journal to SQLite so disk I/O
    never stalls the sim.
  - Cross-thread handoff via lock-free/MPSC queues; the sim owns all game state.
- **Tick loop:** intake commands → run systems (movement, harvesting, building,
  combat) → advance tick → build per-player interest snapshots → hand to net
  threads → periodically checkpoint to persistence.
- **Capacity target:** 100 concurrent players in one shard. Design notes for
  future **sector-based sharding** (hand sectors to additional processes) are
  recorded but **out of scope for launch**.

---

## 10. Client — UWP / C++/WinRT / DirectX 12

### 10.1 App model (verified)

A lean game shell, **no XAML**:

1. Implement `IFrameworkViewSource` → returns our `IFrameworkView`.
2. `IFrameworkView` provides `Initialize`, `SetWindow(CoreWindow)`, `Load`,
   `Run`, `Uninitialize`. We hook `CoreWindow` input and lifecycle events here.
3. Drive our own loop in `Run` (process events, simulate-predict, render).

### 10.2 DX12 bring-up (verified)

- Create device + a **direct `ID3D12CommandQueue`**.
- Create the swap chain with **`IDXGIFactory2::CreateSwapChainForCoreWindow`**,
  passing the **command queue** as `pDevice` (D3D12 requirement) and the
  `CoreWindow` via `winrt::get_unknown(...)`. Flip-model, 2–3 back buffers.
- C++/WinRT idioms: `winrt::com_ptr<T>` (not WRL `ComPtr`), `.put()/.get()`,
  `winrt::check_hresult(...)`.
- Descriptor heaps (RTV/DSV/CBV-SRV-UAV), a per-frame ring of command
  allocators, upload heaps for dynamic data, a small **PSO cache**.

### 10.3 Input, HUD, lifecycle

- Keyboard/mouse/gamepad via `CoreWindow` events; map to game intents.
- Custom **immediate-mode HUD** drawn with DX12 + the bitmap font (§12.2).
- Handle UWP suspend/resume (release/recreate device resources as needed).

---

## 11. Rendering & the Darwinia Look

Darwinia's identity = **dark space, glowing neon outlines, bloom, additive
particles, clean minimal UI.** The pipeline is built around that:

1. **Scene pass (HDR):** render ships/base/structures as low-poly meshes with
   bright emissive edges/silhouettes on a near-black background. **Instanced**
   draws for many similar ships.
2. **Bright-pass + bloom:** threshold the HDR target, down-sample, separable
   Gaussian blur, composite back additively → the signature glow.
3. **Particles (additive):** thrusters, weapons fire, resource sparks, explosions
   — GPU-friendly additive blending, no depth writes.
4. **Composite & tone-map:** combine, optional subtle **scanline/vignette/grain**
   for the retro-digital vibe (toggleable).
5. **HUD pass:** bitmap-font text + simple vector-ish primitives, drawn last.

All shaders authored in HLSL and **compiled offline to DXIL** at build time
(UWP can't compile HLSL at runtime), loaded as bytecode into PSOs.

❓ **Open (§18):** art source — do meshes come from you, or do we go *fully*
procedural/primitive (Darwinia itself is famously simple geometry)? Affects §12.3.

---

## 12. Asset Pipeline

All loaders are custom; formats are fixed by the brief.

### 12.1 Textures — `.dds`
Custom DDS parser: read `DDS_HEADER` (+ `DDS_HEADER_DXT10` when present), map to
`DXGI_FORMAT`, support block-compressed formats (BC1–BC7) and mip chains, upload
via an upload heap to a default-heap texture. No external texture lib.

### 12.2 Fonts — your bitmap textures
You provide font bitmaps. We pair each with a small **glyph-metrics descriptor**
(per-glyph UV rect, advance, offset; ❓ format TBD — see §18) and render text as
batched textured quads. Crisp, fast, on-style for a minimalist HUD.

### 12.3 Meshes
Custom compact binary mesh format produced by a `tools/` cooker from whatever
source format we settle on (❓ §18). Positions/normals/edges tuned for the
neon-silhouette look.

### 12.4 Shaders
`shaders/*.hlsl` → `dxc` (build step) → DXIL in `shaders/bin/` → loaded at runtime.

---

## 13. Gameplay Systems (4X)

The 4X loop mapped onto "one mobile base + ships in one universe":

- **eXplore** — sensor/fog range around the base and ships; discover resource
  fields, anomalies, other players. The universe is open; visibility is earned.
- **eXpand** — the base is **mobile**; "expanding" means relocating, projecting
  sensor/ship range, and (later) deploying outposts/claims.
- **eXploit** — resource nodes harvested by ships; resources stored at the base;
  spent from a **build queue** to produce ships/modules.
- **eXterminate** — ship weapons, PvE threats, PvP; base defense and HP.

**Core entities:** `Base` (mobile; modules: storage, shipyard, sensors; HP),
`Ship` types (💡 starter set: *scout*, *harvester*, *fighter*, *builder*),
`ResourceNode`, `Projectile`, `Player`.

**Economy (💡 starter):** small set of resource types → build costs → ships/
modules; tech/upgrades layered in later. Exact numbers are a balancing exercise
post-M3.

❓ **Open (§18):** PvP rules (full-loot? safe zones near a base?), combat feel
(arcade vs. tactical), and how aggressive the early economy should be.

---

## 14. Persistence (SQLite + snapshots)

- **SQLite (WAL mode)** as the durable store: player accounts, base state
  (position, modules, HP, inventory), build queues, owned ships, persistent world
  objects, and resource-node state.
- **Periodic binary snapshots** of live simulation state for fast restart.
- **Append-only event journal** between snapshots → crash recovery = load latest
  snapshot + replay journal.
- All writes happen on the **persistence thread** (write-behind); the sim never
  blocks on disk.
- ❓ **Open (§18):** do we need **accounts/login** in scope now, or a dev-only
  "pick a name" identity until later?

---

## 15. Build, Tooling, Testing

- **Build:** Visual Studio solution with four projects (`core`, `server`,
  `client`, `tools`). `client` is an **AppContainer/UWP** `.vcxproj` packaged as
  **MSIX**. `core`/`server`/`tools` optionally also build via CMake.
- **Shader build:** pre-build step invokes `dxc` over `shaders/*.hlsl`.
- **Asset cooking:** `tools/` exes convert/validate DDS, cook meshes, pack font
  metrics; run as pre-build or on demand.
- **Testing:** a **tiny custom test runner** (assert macros + a registry +
  `main`) — no gtest. Unit tests for math, serialization, the reliability layer
  (simulated loss/reorder/dup), and shared sim rules. A **headless loopback
  harness** spins the server + N synthetic clients for netcode/load testing.
- **CI / web sessions:** a `SessionStart` hook can ensure the toolchain builds
  `core`/`server`/`tools` and runs tests in cloud sessions (UWP packaging stays
  local). I can set this up when we start coding.

---

## 16. Milestone Roadmap

Each milestone has a crisp **acceptance test**. Estimates are relative
(S/M/L/XL), not calendar dates.

### M0 — Foundations *(S–M)*
Repo, VS solution, `core` skeleton (math, ECS, time, logging, serde), custom test
runner, shader/asset build steps wired.
**Done when:** all targets compile; `core` unit tests pass in CI.

### M1 — Networked tech slice 🔒 *(L)*  ← **first milestone**
Server runs the fixed-tick loop; reliable-UDP handshake; client connects, renders
the **base + a few ships** in Darwinia style; player **moves the base**; positions
replicate (server-authoritative) to **several simultaneous clients** with
interpolation and basic prediction.
**Done when:** 3+ clients on one server each see each other's base move smoothly
across a sector boundary, surviving simulated packet loss; nothing renders raw
`uint64_t` (floating origin verified).

### M2 — Asset pipeline & the Darwinia look *(M–L)*
DDS loader, bitmap-font HUD, bloom + additive particles, instanced ship rendering,
composite/post FX.
**Done when:** a fleet of instanced ships with thruster particles and glowing
silhouettes renders at target frame rate with a legible bitmap-font HUD.

### M3 — Core 4X loop *(L)*
Resource nodes, harvesting ships, base storage, build queue producing ships;
sensor/fog exploration.
**Done when:** a player can fly a harvester to a node, gather, return, and build a
new ship from stored resources — fully server-authoritative.

### M4 — Scale & interest management *(L)*
Sector subscriptions, delta compression against acked baselines, bandwidth/CPU
budgeting toward 100 players.
**Done when:** a 100-synthetic-client load test holds tick rate and stays within
the per-client bandwidth budget (§Appendix B).

### M5 — Persistence & identity *(M)*
SQLite store, snapshots, journal, crash recovery; accounts/identity per §18.
**Done when:** kill the server mid-play and restart → world and every base state
restore correctly.

### M6 — Combat, polish & balancing *(L)*
Weapons/projectiles, base defense, PvE/PvP per §18, economy tuning, UI polish,
Store-compliance pass (test without loopback exemption).
**Done when:** a full 4X session is playable end-to-end by multiple players.

---

## 17. Risks & Mitigations

| # | Risk | Impact | Mitigation |
| --- | --- | --- | --- |
| R1 | **UWP networking sandbox** (capabilities, loopback isolation, background limits) | High | Verified approach in §8.2; abstract `ISocket`; document loopback-exempt dev workflow; test Store path early. **If UWP proves too limiting, a Win32 desktop client reusing the same renderer is the fallback** (see §18 Q). |
| R2 | **`uint64_t` precision vs. float GPU** | Med | Floating-origin rebasing + sector-local floats (§6.4); never feed raw world coords to the GPU. |
| R3 | **Single-shard scaling to 100** | Med | Interest management + delta compression (§8.5); load-test from M4; sharding design noted for later. |
| R4 | **"Custom everything" scope** | High | STL assumed allowed (A1); SQLite/cppwinrt carry the riskiest bits; ruthless milestone scoping. |
| R5 | **UWP can't runtime-compile HLSL** | Low | Offline DXIL build step from day one (§11/§12.4). |
| R6 | **Reliable-UDP correctness** | Med | Dedicated tests with simulated loss/reorder/dup; loopback load harness (§15). |
| R7 | **Art production** | Med | Lean on procedural/primitive geometry in the Darwinia spirit until real assets exist (§18). |

---

## 18. Open Questions (need your input)

These don't block me from refining the plan, but they shape several sections.
Ranked by impact:

1. **Coordinate scale (§6.1):** 1 unit = **mm / cm / m**? (Tight local control vs.
   galaxy-scale feel.) My default: **mm**.
2. **UWP vs. Win32 fallback (§17 R1):** is the client **strictly UWP/Store**, or is
   a Win32 desktop build an acceptable fallback if UWP networking/packaging
   becomes a blocker? (Strongly affects risk.)
3. **STL allowed? (A1)** "No third-party libs" = keep STL, or do you want a
   from-scratch foundation (custom containers/allocators)?
4. **Accounts/identity (§14):** real login at launch, or dev-only "pick a name"
   until later?
5. **Art assets (§11/§12.3):** will you supply ship/base **meshes** (and in what
   format), or go fully procedural/primitive in the Darwinia style?
6. **Font descriptor format (§12.2):** alongside the bitmaps, can you provide glyph
   metrics (a `.fnt`/JSON/CSV), or are these **fixed-grid monospace** atlases?
7. **Gameplay feel (§13):** PvP rules (full-loot? safe zone around a base?), combat
   style (arcade vs. tactical), and early-economy pacing.
8. **Simulation tick rate (§7):** 20 Hz default OK, or do you want faster (30 Hz)?

---

## Appendix A — Packet format sketch (draft)

```
UDP datagram
├── Header (fixed)
│   ├── protocol_id        u32   magic/version guard
│   ├── connection_token   u32   anti-spoof, assigned at handshake
│   ├── sequence           u16   per-packet sequence
│   ├── ack                u16   latest received sequence
│   └── ack_bits           u32   acks for the 32 packets before `ack`
└── Payload: 1..N messages
    └── Message
        ├── channel        u8    (Unreliable | ReliableOrdered | ...)
        ├── msg_type       u8    (Command | Snapshot | Event | Fragment | ...)
        ├── length         u16
        └── body           variable (bit-packed / delta-encoded)
```
Fragments carry `{message_id, fragment_index, fragment_count}` for reassembly.

## Appendix B — Tick & timing budget (initial targets)

| Quantity | Target |
| --- | --- |
| Sim tick rate | 20 Hz (50 ms) |
| Snapshot send rate / client | 10–20 Hz |
| Client render rate | display-rate (60+ fps), decoupled from sim |
| Interpolation delay | ~100 ms |
| Safe UDP payload | ~1200 bytes (avoid IP fragmentation) |
| Per-client downstream budget | TBD at M4 (drives interest-set sizing) |

## Appendix C — Glossary

- **Shard** — one server process hosting one contiguous world.
- **Interest set** — the entities a given player is currently told about.
- **Baseline** — the last snapshot a client acked; deltas are diffed against it.
- **Floating origin** — per-frame render origin near the camera to preserve float
  precision in a huge world.
- **Sector** — a fixed-size cubic cell of the universe used for spatial queries
  and interest management.

---

*End of DRAFT v0.1 — please review §2 (am I building on the right decisions?) and
§18 (open questions). I'll fold your answers into v0.2.*
