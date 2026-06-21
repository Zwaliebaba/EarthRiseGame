# EarthRise

> A persistent, single-shard space **4X MMO** with a custom C++ engine — fantasy:
> **_Homeworld × EVE_**. Each player is a **fleet commander** who directs a small fleet
> of ships (RTS-style) from one **mobile mothership-base** through a single contiguous,
> contested universe: explore, harvest, craft, trade, expand, and wage **PvE _and_
> territorial PvP** in a real-time loop shared by ~100 concurrent players at launch and
> **designed to scale to hundreds**.

> **Status:** pre-release / in active development. Milestones **M0, M1a, M1b complete**;
> **M2 (Darwinia look + audio)** is the current engineering milestone. See
> [`docs/masterplan.md`](docs/masterplan.md) §17 for the roadmap.

---

## Table of contents

- [Highlights](#highlights)
- [Gameplay](#gameplay)
- [Technology](#technology)
- [Repository structure](#repository-structure)
- [Architecture at a glance](#architecture-at-a-glance)
- [Getting started](#getting-started)
- [Building](#building)
- [Running](#running)
- [Testing](#testing)
- [Documentation](#documentation)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)

---

## Highlights

- **Custom engine, Microsoft-platform only.** No third-party game libraries — just C++23
  and Microsoft platform components (C++/WinRT, DirectXMath, DirectX 12, Winsock on the
  server / WinRT `DatagramSocket` on the client, ODBC/SQL Server, Windows CNG,
  XAudio2/X3DAudio).
- **Server-authoritative.** A dedicated Windows server (`ERServer`) owns a fixed-step
  **30 Hz** simulation; clients send **intents**, never authoritative state.
- **Encrypted reliable-UDP** transport: CNG **ECDH** handshake with a **pinned server key**,
  **AES-GCM** AEAD, replay protection, and a stateless-cookie DoS guard.
- **Interest-scoped, delta-compressed replication** with no bulk universe sync — a new client
  converges from an empty baseline over the ordinary snapshot loop.
- **One contiguous universe** in signed `int64` metre coordinates with floating-origin
  rendering, designed to **scale to hundreds of players** (cell pub/sub interest,
  island-parallel sim, time-dilation under overload — see the
  [networking review](docs/design/review/networking-scale-review.md)).
- **Testable by construction:** a headless host (`ERHeadless`) runs many bot client
  sessions for integration, load, and record/replay determinism testing.
- **The Darwinia look** — dark void, neon glow, bloom, additive particles, minimalist
  bitmap-font HUD.

## Gameplay

EarthRise is a real-time 4X built on a few pillars (full design in
[`docs/masterplan.md`](docs/masterplan.md) §13 and [`docs/design/`](docs/design/)):

- **Fleet command from a mobile home.** You command a fleet (launch cap **6–12 ships**)
  from a slow, powerful **mothership-base** — a capital unit that is *disabled, not
  destroyed*, never permanently lost.
- **A persistent, contested sandbox.** One universe, one shard, **tiered security
  (high → low → null)** with player-claimable **territory** in nullsec. Risk scales with
  reward.
- **Depth through fitting & economy.** Role + module **fitting** with **damage-type
  counters**; a **player-driven crafting economy** where destruction creates demand; hybrid
  tech-tree progression.
- **A living universe.** Dynamic NPC **faction invasions** and procedural
  **anomalies/expeditions** give PvE a pulse.
- **Travel.** Sublight combat movement, **interdictable warp** to scanned points, and a
  long-haul **jump-beacon network** (fuel + cooldown) whose owned beacons are chokepoints.

## Technology

| Layer | Choice |
| --- | --- |
| Language / build | **C++23**, MSVC; **MSBuild** (`EarthRise.slnx`); x64 |
| Client app model | **UWP** (`CoreApplication` + `IFrameworkView`, C++/WinRT, no XAML) → MSIX |
| Rendering | **DirectX 12** (FL 12_0, SM6/DXIL), DXGI flip-model; 3D Scene + 2D Canvas split; HDR forward + bloom |
| Audio | **XAudio2 (2.9)** + **X3DAudio**, WAV/PCM-16 (client-only `NeuronAudio`) |
| Server | **ERServer** — Win32 console in a Windows Server Core container; **Winsock UDP + IOCP**; 30 Hz sim / 20 Hz snapshot |
| Transport | Custom **encrypted reliable-UDP** (CNG ECDH + AES-GCM) |
| Database | **SQL Server over the network** (self-hosted → Azure SQL); ODBC Driver 18 |
| Math | **DirectXMath** |
| Tests | Microsoft Native Unit Test (one `*Test` project per project) + a Linux-compatible headless runner |

Build/style conventions live in [`CODE_STANDARDS.md`](CODE_STANDARDS.md); contributor and
agent guidance in [`agents.md`](agents.md).

## Repository structure

Source files are kept **flat** in each project; logical grouping is done with **Visual
Studio Filters**, not on-disk subdirectories.

```
EarthRiseGame/
├── EarthRise.slnx          Solution (XML format), x64
├── NeuronCore/             Shared items (.vcxitems): math · ECS · universe/sector · serde · net · sim
├── NeuronClient/           Static lib: session · replica · interpolation · control
├── NeuronRender/           Static lib [UWP/DX12]: DeviceResources · SceneRenderer · CanvasRenderer
│   └── shaders/            .hlsl → dxc (SM6/DXIL) → generated headers (untracked)
├── NeuronAudio/            Static lib [client-only]: XAudio2 voice graph · X3DAudio · WAV reader
├── ERServer/               Exe: IOCP UDP listener · fixed-step sim loop · snapshot broadcast
├── ERHeadless/             Exe: N bot/scripted sessions (load + integration + replay) [no audio]
├── EarthRise/              Exe [UWP/MSIX]: IFrameworkView shell wiring client + render
├── Testing/                MSTest, one per project (NeuronCoreTest, …, ERServerTest, ERHeadlessTest)
├── NeuronTools/            Linux `testrunner` over the dependency-free asset/parser headers
├── Config/
│   ├── db/                 SQL schema + ordered, forward-only migrations
│   └── deploy/             ERServer Dockerfile (Server Core) · docker-compose.dev.yml
└── docs/                   masterplan.md (design source-of-truth) · design/ · implementation/
```

`NeuronCore` is a **shared items project** compiled directly into its consumers
(`EarthRise`, `NeuronClient`, `ERServer`), not a standalone `.lib`. `NeuronAudio` links
`NeuronCore` but **not** `NeuronRender`, and is **not** linked by `ERHeadless`/server.

## Architecture at a glance

```
  ┌──────────────────────────────┐   encrypted    ┌──────────────────────────────┐
  │ UWP CLIENT (EarthRise)        │  reliable UDP  │ ERServer (Win32 console)      │
  │  NeuronRender (DX12 3D + 2D)  │ ◀────────────▶ │  Winsock UDP + IOCP           │
  │  NeuronClient (session/replica│  snapshots /   │  CNG handshake · AEAD · acks  │
  │   /interp/control)            │  intents       │  30 Hz authoritative sim (ECS)│
  │  NeuronAudio (XAudio2/X3D)    │                │  persistence (ODBC, outbox)   │
  └──────────────────────────────┘                └───────────────┬──────────────┘
  ┌──────────────────────────────┐   encrypted                    │ TCP 1433 (ODBC)
  │ ERHeadless (N bot sessions)   │ ◀────────────▶                 ▼
  │  no rendering, no audio       │                ┌──────────────────────────────┐
  └──────────────────────────────┘                │ SQL Server (external network) │
        shared by all: NeuronCore (math · ECS ·    │ self-hosted now → Azure SQL   │
     universe int64 · sectors · net protocol · sim)└──────────────────────────────┘
```

- **Server-authoritative:** clients send validated **intents/commands**; the server owns
  state and streams **interest-scoped, delta-compressed** snapshots back.
- **Shared simulation rules** live in `NeuronCore`, so client and server step identically
  (deterministic ECS) — the basis for prediction, replay testing, and warm-restart.

See [`docs/masterplan.md`](docs/masterplan.md) §4–§9 for the full architecture and §8 for
the networking design.

## Getting started

### Prerequisites

- **Windows 10/11** (x64) — required for the client, server, and full test suite.
- **Visual Studio 2022** with:
  - *Desktop development with C++* and *Universal Windows Platform development* workloads
  - Windows 10/11 SDK (for DirectX 12, XAudio2/X3DAudio, Winsock, CNG, ODBC)
  - MSVC toolset configured for **C++23** (`/std:c++latest`)
- **ODBC Driver 18 for SQL Server** and a reachable **SQL Server** instance (only needed
  once persistence/auth come online at M5; not required to build or run the headless slice).
- *(Optional)* **Docker Desktop** (Windows containers) to run `ERServer` containerized —
  see [`Config/deploy/`](Config/deploy/).

> Most networking and simulation logic is platform-independent and additionally validated
> on Linux via the `NeuronTools/testrunner` (built with `make`), so non-Windows CI can
> catch core regressions without a Windows build.

### Clone

```sh
git clone https://github.com/Zwaliebaba/EarthRiseGame.git
cd EarthRiseGame
```

## Building

Open **`EarthRise.slnx`** in Visual Studio 2022 and build the solution (x64), or from a
Developer command prompt:

```sh
msbuild EarthRise.slnx /p:Configuration=Debug /p:Platform=x64
```

Notes:
- HLSL shaders under `NeuronRender/shaders/` are compiled to **embedded SM6/DXIL headers**
  by `dxc` at build time; the generated `CompiledShaders/*.h` are not in source control.
- The UWP client (`EarthRise`) produces an **MSIX** package and deploys via Visual Studio.

## Running

A typical headless end-to-end loop (no GPU/UWP needed) — the path the automated tests
exercise:

1. **Start the server:** run `ERServer` (it opens the fixed-step sim loop and a UDP
   endpoint).
2. **Attach bots:** run `ERHeadless`, which drives multiple `NeuronClient` bot sessions
   against the server and is the supported way to validate connection flow, the server
   loop, and snapshot behavior.
3. **Run the UWP client:** deploy and launch `EarthRise` from Visual Studio to render the
   universe with mouse+keyboard input.

> **UWP loopback:** for local client↔server testing on one machine, UWP is sandboxed off
> loopback by default. Visual Studio adds a loopback exemption in Debug; test the
> non-exempt path before Store. `ERHeadless` (Win32) is exempt. See `docs/masterplan.md`
> §8.1.

Containerized server (dev): see [`Config/deploy/docker-compose.dev.yml`](Config/deploy/).
SQL Server is **never** containerized — it is reached over the network.

## Testing

Every project has a matching **`*Test`** Microsoft Native Unit Test project under
[`Testing/`](Testing/) (`NeuronCoreTest`, `NeuronClientTest`, `NeuronRenderTest`,
`NeuronAudioTest`, `ERServerTest`, `ERHeadlessTest`). New features land with their tests in
the same commit.

- **In Visual Studio:** Test Explorer → run all, after building the solution.
- **Command line:**
  ```sh
  vstest.console.exe Testing\NeuronCoreTest\x64\Debug\NeuronCoreTest.dll
  ```
- **Linux CI / no Windows build:** the dependency-free
  [`NeuronTools/testrunner`](NeuronTools/) (`make`) exercises the platform-independent
  math, serialization, reliability (loss/reorder/dup), crypto handshake (incl.
  MITM/nonce/replay), and asset parsers.
- **End-to-end / load / determinism:** `ERServer` + `ERHeadless` provide multi-client
  integration, load tests, and an input-log **record/replay determinism** harness.

> Some test files are currently placeholders; don't assume deep coverage in an area
> without checking. See `agents.md` → *Known current-state signals*.

## Documentation

- **[`docs/masterplan.md`](docs/masterplan.md)** — the **design source-of-truth**: locked
  decisions, architecture, subsystem specs, risks, and the milestone roadmap.
- **[`docs/design/`](docs/design/)** — per-subsystem design docs (combat balance, tech
  tree, economy, NeuronAudio/NeuronRender architecture, UI/HUD, touch controls, menu UI).
- **[`docs/design/review/`](docs/design/review/)** — architecture/code reviews, e.g. the
  [MMO networking review for scaling to hundreds of players](docs/design/review/networking-scale-review.md).
- **[`docs/implementation/`](docs/implementation/)** — ordered, testable work plans for the
  active milestone.
- **[`agents.md`](agents.md)** — repository overview, layout conventions, and guidance for
  contributors and AI agents.
- **[`CODE_STANDARDS.md`](CODE_STANDARDS.md)** — language, naming, and style conventions.

## Roadmap

Milestones (full detail in `docs/masterplan.md` §17):

| Milestone | Theme | Status |
| --- | --- | --- |
| **M0** | Foundations (engine skeleton, ECS, fixed-step time, build) | ✅ Complete |
| **M1a** | Networked transport (headless): handshake, reliable-UDP, replication | ✅ Complete |
| **M1b** | Client tech slice (DX12 engine foundation, camera, input) | ✅ Complete |
| **M2** | Darwinia look + audio (DDS/CMO, HUD, bloom/particles, NeuronAudio) | 🔨 **Active** |
| **M3** | Core 4X loop, fleet command & navigation | 📝 Planned |
| **M4** | Scale & interest (cell pub/sub, delta compression, load test at scale) | ⏳ |
| **M5** | Accounts, auth & persistence (SQL, warm-restart) | ⏳ |
| **M6** | Combat model & production deployment (Kubernetes, Azure SQL) | ⏳ |
| **M7** | Sandbox: conquest, economy, PvE content & onboarding | ⏳ |

## Contributing

This is an early-stage project. Before contributing, please read
[`agents.md`](agents.md) and [`CODE_STANDARDS.md`](CODE_STANDARDS.md). In short:

- Keep changes **minimal and localized**; respect project boundaries.
- Keep source files **flat**; group with Visual Studio **Filters**, not subdirectories.
- Put shared protocol/simulation rules in **`NeuronCore`**; rendering in **`NeuronRender`**;
  client presentation/replica in **`NeuronClient`**.
- **Every feature ships with its `*Test` cases in the same commit.** A feature isn't done
  until its tests pass.
- The **masterplan stays authoritative on decisions** — implementation plans sequence work
  and reference it by `§` number rather than re-deciding anything. If a real decision
  changes, update the masterplan first.

## License

No license has been declared for this repository yet. Until a `LICENSE` file is added, all
rights are reserved by the project owner; please open an issue if you need clarification on
usage.
