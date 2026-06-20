# agents.md

## Repository overview
EarthRiseGame is a multi-project Visual Studio C++ solution centered on a shared simulation/networking core.

Current top-level projects in the solution:
- `NeuronCore/` - shared core library for platform, ECS, world math, serialization, networking, and simulation.
- `NeuronClient/` - client-side session, replica, interpolation, and controller code.
- `NeuronRender/` - rendering library with D3D12 device, scene, and canvas/HUD systems.
- `ERServer/` - dedicated server host.
- `ERHeadless/` - headless bot client host for live/dev validation.
- `EarthRise/` - UWP client shell that wires client + render together.
- `Testing/` - MSTest projects for each major area.

## Architecture notes

### NeuronCore
`NeuronCore/NeuronCore.cpp` is currently a single translation unit that includes the subsystem headers so the static library validates the header-only M0 foundation. The file groups the code into:
- Platform: `Debug.h`, `TimerCore.h`, `Allocators.h`
- ECS: `Ecs.h`
- World: `WorldPos.h`
- Serialization: `BitStream.h`, `Serde.h`
- Networking: protocol, sequencing, replay, reliability, fragmentation, packet codec, crypto/socket abstractions, secure channel, handshake
- Simulation: components, fixed-step accumulation, movement, snapshot

Some platform-backed pieces already have `.cpp` implementations, especially crypto, sockets, and simulation support.

### Client
`NeuronClient/NeuronClient.cpp` describes the client library layout:
- `session/` - connection/session logic
- `replica/` - snapshot decode and projection into render-friendly state
- `interp/` - interpolation buffer
- `control/` - client controller interfaces

### Render
`NeuronRender/NeuronRender.cpp` documents three rendering areas:
- `gfx/` - D3D12 device, swap chain, command infrastructure
- `scene/` - 3D scene rendering
- `canvas/` - 2D HUD rendering

Shader sources live under `NeuronRender/shaders/`.

### Hosts
- `ERServer/ERServer.cpp` is the authoritative dedicated server host. It owns the fixed-step simulation loop, processes UDP datagrams, and broadcasts snapshots.
- `ERHeadless/ERHeadless.cpp` runs multiple bot clients against a live server and is useful for end-to-end validation.
- `EarthRise/App.cpp` is the UWP shell that drives networking, snapshot consumption, interpolation, and rendering inside an `IFrameworkView` loop.

## Working conventions
- Prefer minimal, localized changes.
- Follow the existing folder boundaries instead of introducing cross-project shortcuts.
- Do not create new subdirectories in the code tree just to group source or header files; use Visual Studio project Filters for logical grouping instead.
- Keep shared protocol/simulation rules in `NeuronCore` when both client and server depend on them.
- Keep rendering-specific code in `NeuronRender` and client presentation/replica code in `NeuronClient`.
- Match the surrounding style in each file. Existing code uses concise comments to describe milestones and subsystem intent.
- Do not assume all modules are fully implemented; several comments explicitly mark milestone-based or stubbed behavior.

## Implementation guidance for agents
- Start from the project that owns the behavior being changed, then verify whether shared types belong in `NeuronCore`.
- For networking changes, inspect both the shared protocol code in `NeuronCore` and the host/session code in `ERServer`, `ERHeadless`, or `NeuronClient`.
- For simulation changes, check snapshot encoding/decoding and any client replica/interpolation code impacted by the new state.
- For rendering changes, confirm whether the data originates in `NeuronClient` replica state or in `NeuronRender` rendering code.
- Be careful with project references and include paths; this solution is split into multiple static libraries and app/host entry points.

## Validation guidance
- Build the full solution after changes.
- Use the relevant MSTest project under `Testing/` when adding or changing covered behavior.
- If touching live connection flow, server loop, or snapshot behavior, also consider validating with `ERServer` + `ERHeadless` because those hosts reflect the intended end-to-end path.

## Known current-state signals
- `NeuronCore/NeuronCore.cpp` explicitly states that many subsystems are still header-only at the current milestone.
- `NeuronClient/NeuronClient.cpp` still contains a stub `CreateSession()` kept for compatibility.
- The test projects exist, but some current test files are placeholders, so lack of deep automated coverage should be assumed unless verified in the specific area being changed.
