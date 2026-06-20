# agents.md

## Repository overview
EarthRiseGame is a multi-project Visual Studio C++ solution (`EarthRise.slnx`) centered on a shared simulation/networking core.

Current top-level projects in the solution:
- `NeuronCore/` - shared core for platform, ECS, world math, serialization, networking, and simulation. Packaged as a **shared items project** (`NeuronCore.vcxitems`); its sources compile directly into each consumer rather than building a standalone `.lib`.
- `NeuronClient/` - client-side session, replica, interpolation, and controller code (static library).
- `NeuronRender/` - rendering library with D3D12 device, scene, and canvas/HUD systems (static library).
- `NeuronAudio/` - **client-only** audio library: XAudio2 (2.9) voice graph + four submix buses, custom WAV/PCM-16 reader, X3DAudio 3D (static library). Links `NeuronCore` + `NeuronTools` (`WavParse.h`), **not** `NeuronRender`. ERHeadless/NeuronClient do **not** link it.
- `ERServer/` - dedicated server host (executable).
- `ERHeadless/` - headless bot client host for live/dev validation (executable).
- `EarthRise/` - UWP client shell that wires client + render together (executable; this is the `EarthRise.Client` referred to in `docs/`).
- `Testing/` - MSTest projects, one per major area (`NeuronCoreTest`, `NeuronClientTest`, `NeuronRenderTest`, `NeuronAudioTest`, `ERServerTest`, `ERHeadlessTest`).

Outside the solution, `NeuronTools/` holds platform-independent asset parsers (`WavParse.h`, `DdsParse.h`, `CmoParse.h`, `FontAtlasLayout.h`) consumed by the runtime loaders and `*check` tools, plus `testrunner/` — a dependency-free harness (built with `make`) that exercises the parser logic on Linux CI without a Windows build.

Supporting files outside the solution:
- `Config/db/` - SQL schema and ordered, forward-only migrations.
- `Config/deploy/` - ERServer `Dockerfile` and dev `docker-compose.dev.yml`.
- `CODE_STANDARDS.md` - language/style conventions. `docs/` - design and implementation docs; `docs/masterplan.md` is the design source-of-truth.

## Source layout convention
All projects keep their source and header files **flat** in the project directory. There are **no on-disk subdirectories** for grouping code; logical grouping is done with **Visual Studio project Filters** (`*.vcxproj.filters`). For example, `NeuronClient` keeps `Session.h`, `SessionImpl.h`, `Replica.h`, `ReplicaManager.h`, `Interpolator.h`, and `IClientController.h` flat, organised under `Session` / `Replica` / `Control` filters. Preserve this layout — do not reintroduce source subdirectories.

## Architecture notes

### NeuronCore
`NeuronCore.vcxitems` is a shared items project consumed by `EarthRise`, `NeuronClient`, and `ERServer`; its sources are compiled into each consumer rather than into a separate library. `NeuronCore/NeuronCore.cpp` is the translation unit that includes the subsystem headers so the header-only subsystems are validated; platform-backed subsystems carry their own `.cpp` files (`CngCrypto.cpp`, `WinsockSocket.cpp`, `SimComponents.cpp`). The headers group into:
- Platform: `Debug.h`, `TimerCore.h`, `Allocators.h`
- ECS: `Ecs.h`
- World/math: `WorldPos.h`, `GameMath.h`, `MathCommon.h`
- Serialization: `BitStream.h`, `Serde.h`
- Networking: protocol, sequencing, replay, reliability, fragmentation, packet codec, crypto/socket abstractions, secure channel, handshake, connection
- Simulation/server: components, fixed-step accumulation, movement, snapshot, command, server world/host

### Client
`NeuronClient/` (static library) holds the client library. Files are flat, grouped by VS Filters:
- Session (`Session.h`, `SessionImpl.h`) - encrypted reliable-UDP client session
- Replica (`Replica.h`, `ReplicaManager.h`) - snapshot decode and floating-origin projection into render-friendly state
- `Interpolator.h` - interpolation buffer
- Control (`IClientController.h`) - client controller interface

### Render
`NeuronRender/` (static library) keeps its renderer files flat. `NeuronRender.cpp` is a stub TU; each class compiles from its own `.cpp`:
- `DeviceResources` - D3D12 device, swap chain, command infrastructure
- `SceneRenderer` - 3D scene rendering
- `CanvasRenderer` - 2D HUD rendering

Shader sources live under `NeuronRender/shaders/`; compiled shader headers are generated at build time and are not in source control.

### Hosts
- `ERServer/ERServer.cpp` is the authoritative dedicated server host. It owns the fixed-step simulation loop, processes UDP datagrams (`IocpUdpListener`), and broadcasts snapshots.
- `ERHeadless/ERHeadless.cpp` runs multiple bot clients against a live server and is useful for end-to-end validation.
- `EarthRise/App.cpp` is the UWP shell that drives networking, snapshot consumption, interpolation, and rendering inside an `IFrameworkView` loop.

## Working conventions
- Prefer minimal, localized changes.
- Follow the existing folder boundaries instead of introducing cross-project shortcuts.
- Keep source files flat within each project; use Visual Studio project Filters for logical grouping instead of new subdirectories.
- Keep shared protocol/simulation rules in `NeuronCore` when both client and server depend on them.
- Keep rendering-specific code in `NeuronRender` and client presentation/replica code in `NeuronClient`.
- Match the surrounding style in each file. Existing code uses concise comments to describe milestones and subsystem intent.
- Do not assume all modules are fully implemented; several comments explicitly mark milestone-based or stubbed behavior.

## Implementation guidance for agents
- Start from the project that owns the behavior being changed, then verify whether shared types belong in `NeuronCore`.
- For networking changes, inspect both the shared protocol code in `NeuronCore` and the host/session code in `ERServer`, `ERHeadless`, or `NeuronClient`.
- For simulation changes, check snapshot encoding/decoding and any client replica/interpolation code impacted by the new state.
- For rendering changes, confirm whether the data originates in `NeuronClient` replica state or in `NeuronRender` rendering code.
- Be careful with project references and include paths. `NeuronCore` is a shared items project compiled into its consumers; `NeuronClient` and `NeuronRender` are static libraries; `EarthRise`, `ERServer`, and `ERHeadless` are app/host executables.
- When adding a file, add it to the owning project (`.vcxproj` / `.vcxitems`) and place it under the appropriate Filter in the matching `.filters` file.

## Validation guidance
- Build the full solution (`EarthRise.slnx`) after changes.
- Use the relevant MSTest project under `Testing/` when adding or changing covered behavior.
- If touching live connection flow, server loop, or snapshot behavior, also consider validating with `ERServer` + `ERHeadless` because those hosts reflect the intended end-to-end path.

## Known current-state signals
- Several `NeuronCore` subsystems are still header-only at the current milestone; `.cpp` implementations land milestone by milestone.
- `NeuronClient/NeuronClient.cpp` still contains a stub `CreateSession()` kept for ERHeadless compatibility.
- The test projects exist, but some current test files are placeholders, so lack of deep automated coverage should be assumed unless verified in the specific area being changed.
