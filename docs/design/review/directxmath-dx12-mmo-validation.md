# EarthRise — Code Validation: DirectXMath, DirectX 12, MMO Multithreading Readiness

> **Status:** Review v1 — 2026-06-22
> **Scope:** Code validation of the current tree after M3 (core 4X loop) and M4
> areas A–C (interest grid, version stamping, quantized delta codec) plus the
> playable-slice client work.
> **Update (post-review):** M4 areas **D–J** have since landed — tombstone eviction
> (D), priority/quota scheduler with cold-start (E), the job-pool partition/determinism
> seam (F), token-routing table (G), bounded time-dilation policy (H), §21 telemetry (I),
> and a contested-vs-dispersed scale harness (J), all with `testrunner` + `NeuronCoreTest`
> coverage. So the §3.2 "interest/delta path **built-but-unwired**" finding is **now wired**
> (per-client `BuildClientSnapshot` selects → ranks → caps → encodes → budgets through
> `ServerUniverse`); the §3.2 **IOCP / per-connection-thread-safety** findings (and the §4
> fix order) **still stand** — that integration is the remaining Windows-side M4 work. Source
> of truth for M4 status: [`../../implementation/M4-scale-interest.md`](../../implementation/M4-scale-interest.md).
> **Focus (as requested):** (1) DirectXMath adoption, (2) DirectX 12 performance,
> (3) scalability of the MMO setup for a future move to multithreading.
> **Complements:** [`networking-scale-review.md`](networking-scale-review.md) (v1,
> design-level). This review grades the *implementation as it now stands*.
>
> **Update 2 (2026-06-23, post full-codebase audit):** three findings here are now
> addressed in-tree. **DX-2 (double-buffering) is resolved** — `DeviceResources::kFrameCount`
> is now **3 (triple-buffered, §11.1)**; every swap-chain/RTV/allocator/fence/upload/
> timestamp size derives from the constant, so the §“double-buffered (`kFrameCount = 2`)”
> notes below (incl. the DX-2 row) are superseded. Two build-hygiene gaps the audit surfaced
> were also fixed: the dxc shader-header generation was gated `Debug|x64`-only (so Release
> wouldn’t regenerate `CompiledShaders/*.h`) and is now broadened to **`Release|x64` too**
> (`/Qembed_debug` stays Debug-only); and 10 M4/M5 headers present in `NeuronCore/` but absent
> from **`NeuronCore.vcxitems`** are now registered. The empty **`ERHeadlessTest`** project was
> seeded with bot-harness + record/replay-determinism smoke tests (§16.1). The §3.2
> **IOCP / per-connection thread-safety** items remain the open Windows-side work.

---

## 0. TL;DR

The codebase is in good shape and the deferrals are honest. Across the three
focus areas:

- **DirectXMath adoption — healthy and, importantly, *correct about alignment*.**
  SIMD (`XMVECTOR`/`XMMATRIX`) is used where it pays (camera, view-proj, lighting,
  picking in `App.cpp`/`RtsCamera.h`) and deliberately *avoided* in the
  serialized/ECS-stored sim structs, which correctly use the 12-byte `XMFLOAT3`.
  The main opportunity is that the **sim hot path is scalar by design** (for
  cross-platform determinism), which is the right call but leaves SIMD on the
  table for the contested-sector case.

- **DirectX 12 — clean, idiomatic, single-threaded.** Fences, per-frame
  allocators, double-buffered CPU-mapped upload buffers, and GPU-timestamp
  instrumentation are all done correctly. The structural ceiling is that
  **everything records on one command list / one thread**, and the renderer is
  **double-buffered (`kFrameCount = 2`)** with **hard entity/shape caps**
  (`kMaxEntities = 512`, `kMaxShapes = 128`) that will bite before "hundreds."

- **MMO multithreading readiness — the architecture is *designed* for it but the
  scalable paths are built-but-not-wired, and the core data structures are not
  yet thread-safe.** Two specific gaps: (a) the M4 interest/delta pipeline exists
  and is unit-tested but **the live broadcast still calls the O(players×entities)
  M3 path**; (b) the IOCP listener is compiled in but **`ERServer` still runs the
  single-threaded `Sleep(1)` Winsock loop**. The ECS's per-access hash lookup
  (`EntityOf`) is the dominant per-tick cost and the main thing that would have to
  change before parallelizing.

None of this is a defect against the current milestone — it's the validation map
for M4 (scale) and the eventual threading work.

---

## 1. DirectXMath adoption

### 1.1 What's right (keep it)

- **Alignment discipline is correct.** `Transform`/`Velocity`/snapshot records
  store `DirectX::XMFLOAT3` (12 B, no alignment requirement), *not* `XMVECTOR`
  (16-B aligned). This is the classic DirectXMath foot-gun and it was avoided:
  these structs live in `std::vector`-backed ECS dense arrays
  (`Ecs.h:80`), in `std::unordered_map` values, and on the snapshot wire — any
  of which would mis-align an `XMVECTOR` member and crash (or silently
  `movups`-degrade) under `/arch:AVX`. See `Components.h:71-79`,
  `UniversePos.h:74`. **Good.**
- **SIMD is used where it earns its keep.** `App.cpp` does view/proj/lighting and
  clip-space picking through `XMMATRIX`/`XMVECTOR`
  (`App.cpp:1451-1520`, `1177-1219`); `GameMath.h` wraps the common vector ops
  with `XM_CALLCONV`. `XMVECTORF32` is the right type for the `constexpr` basis
  vectors (`GameMath.h:74-88`).
- **The Linux test shim is scoped honestly.** `dxmath_shim/DirectXMath.h` provides
  only the `XMFLOAT3`/`XMFLOAT4X4` field layout so the *integer/struct* sim logic
  unit-tests on Linux; it deliberately does **not** stub the SIMD path
  (`dxmath_shim/DirectXMath.h:1-17`). This keeps the determinism tests honest
  about what they cover.

### 1.2 Findings

- **DM-1 (info / by-design): the sim hot path is scalar, not SIMD.**
  `IntegrateMovement` / `ClampSpeed` (`Movement.h:20-48`), the fleet-order
  steering, AI nearest-target scan, and harvest distances all do component-wise
  `float`/`double` arithmetic on `XMFLOAT3` and `UniversePos`, never `XMVECTOR`.
  This is *correct* for determinism (the same code must produce bit-identical
  results on the Windows server and the Linux test runner, `Movement.h:6-8`), and
  most of these touch `int64` positions that don't vectorize trivially anyway.
  **Action:** none required now, but when the contested-sector sim-time gate is
  added (M4), profile `MovementSystem`/`AiSystem`; if they show up, a
  *deterministic* SIMD batch (process N transforms with `XMVECTOR`, same rounding)
  is the lever — keep the scalar path as the determinism reference.

- **DM-2 (minor): `SceneRenderer` builds the per-instance world matrix by hand.**
  `SceneRenderer::Render` fills the 4×4 with scalar `cos`/`sin`
  (`SceneRenderer.cpp:399-416`) for every entity every frame. For a Y-only
  rotation this hand-rolled form is actually *cheaper* than a general
  `XMMatrixAffineTransformation`, so this is fine as-is — but it is the natural
  place a future job system would vectorize/parallelize (see MT-4). Flagging only
  so it's on the radar, not as a defect.

- **DM-3 (style): global `using namespace` + operator overloads in a header.**
  `GameMath.h:110` does `using namespace Neuron::Math;` at file scope, and the
  namespace defines `operator==`/`operator-` on `XMVECTOR`/`XMVECTORF32`
  (`GameMath.h:62-70`). Pulling operator overloads into the global namespace from
  a widely-included header invites ADL/overload-resolution surprises as more TUs
  include it. **Action:** drop the trailing `using namespace` and let call sites
  qualify or `using`-declare locally. Low priority, but cheap before more code
  depends on it.

---

## 2. DirectX 12 performance

### 2.1 What's right (keep it)

- **Frame sync is correct.** Per-frame command allocators + a single fence with
  per-frame fence values; `BeginFrame` waits only on *this* slot's fence before
  reusing its allocator (`DeviceResources.cpp:204-215`), and `WaitForGpu` drains
  on resize/teardown (`317-323`). No obvious CPU/GPU race.
- **Upload buffers are correctly double-buffered.** Both `SceneRenderer`
  (`SceneRenderer.cpp:305-323`) and `ParticleRenderer`
  (`ParticleRenderer.cpp:128-135, 254`) keep one persistently-mapped UPLOAD buffer
  *per in-flight frame* and index by `FrameIndex()`, so the CPU never writes a
  buffer the GPU may still be reading. This is the right pattern.
- **GPU timestamp instrumentation exists** (`DeviceResources.cpp:71-95, 220-282`)
  and PIX markers wrap every pass (`PixMarkers.h`), so the perf gate (§16.3) is
  measurable now rather than retrofitted.
- **Instanced draw with shape batching.** `SceneRenderer::Render` sorts entities
  by `shapeId` and issues one `DrawIndexedInstanced` per contiguous run
  (`SceneRenderer.cpp:388-458`) — the right call-count strategy.

### 2.2 Findings

- **DX-1 (medium): hard entity/shape caps will bite before "hundreds."**
  `kMaxEntities = 512` (`SceneRenderer.h:87`) and the scene silently truncates
  with `std::min(count, kMaxEntities)` (`SceneRenderer.cpp:380`). A 250-entity
  contested sector × multiple nearby fleets can exceed 512 visible. The
  `std::array<uint32_t, kMaxEntities> order` is also a 512-entry stack object
  sorted every frame (`SceneRenderer.cpp:388-391`). **Action:** make the cap a
  function of the interest budget (App. B ≤250 visible) rather than an independent
  magic number — this is the render-side mirror of the same note about
  `ReplicaSet::kMaxEntities` in the networking review §4. At minimum, log when
  truncation happens so it's not a silent pop-out.

- **DX-2 (low): double-buffering caps frame-pacing headroom.**
  `kFrameCount = 2` (`DeviceResources.h:30`). Triple-buffering (3) generally
  smooths pacing and lets the CPU run further ahead, at the cost of one extra
  back-buffer + allocator + upload-buffer set per renderer. Worth A/B-testing once
  the scene is heavier; it's a one-constant change but touches every per-frame
  array sizing, so do it deliberately.

- **DX-3 (low): `SceneRenderer::Initialize` spins up a throwaway queue + fence and
  blocks.** The geometry upload creates its own `ID3D12CommandQueue` and fence and
  does a full `WaitForSingleObjectEx(INFINITE)` (`SceneRenderer.cpp:286-299`).
  Fine as a one-time init cost, but it (a) duplicates the device's existing direct
  queue and (b) is a synchronous stall. If startup time matters, fold these
  one-time uploads onto a shared upload/copy queue with a single batched fence
  wait. Not hot-path.

- **DX-4 (low): root constants re-set per draw-run.** `DrawRun` re-sets the 16-float
  viewProj and 28-float lighting block on every shape run
  (`SceneRenderer.cpp:477-488`). viewProj/lighting are constant across the frame;
  setting them once per frame (or via a CBV) saves a handful of root-constant
  uploads per shape. Negligible at current shape counts; revisit only if shape
  runs grow large.

- **DX-5 (info): no PSO disk cache.** Shaders are compiled offline (good), but PSOs
  are created fresh each run with no `ID3D12PipelineLibrary` (`SceneRenderer.cpp`,
  `PostProcess.cpp`). Acceptable for the current PSO count; revisit if PSO
  permutations grow.

- **DX-6 (info): single command list, single thread.** All passes record into the
  one `m_cmdList` owned by `DeviceResources` (`DeviceResources.h:86`). This is the
  rendering analogue of the sim's single-thread ceiling (§3): when scene
  complexity grows, multi-threaded command recording (parallel command lists or
  bundles merged at submit) is the lever. Designed-for, not built. See MT-4.

---

## 3. MMO scalability & multithreading readiness

This is where the requested "in case we want to go multi-threading" lens matters
most. The headline: **the data-oriented design is the right foundation, the
parallel-friendly disciplines are already present in places, but the scalable
paths are built-but-not-wired and the core containers are not thread-safe.**

### 3.1 What's right (keep it)

- **Deferred structural mutation is already the house style.** Systems collect
  results and apply structural changes *after* iteration, never mutating the ECS
  mid-`ForEach`: `CombatSystem` gathers `hits`/`killed` then destroys
  (`ServerUniverse.h:830-852`), `BuildSystem` defers spawns
  (`974-987`), `AiSystem` snapshots targets first (`900-936`). This is exactly the
  discipline island-parallel sim needs — keep enforcing it.
- **Deterministic iteration + stable ordering.** `World::ForEach` walks ascending
  entity index (`Ecs.h:272-302`); interest cell lists are kept sorted
  (`Interest.h:237-246`); `SimHash` sorts by `netId` before hashing
  (`ServerUniverse.h:636`). Determinism is a *scheduling* property here, which is
  the precondition for "parallelize as partition + ordered merge."
- **The M4 interest grid is the right architecture.** Cell pub/sub with
  resident/subscriber lists turns broadcast from O(players×entities) into
  O(Σ subscriptions) (`Interest.h:59-225`), version stamping makes the diff
  "version > acked" (`Interest.h:330-360`), and per-client baselines are
  ack-advanced and memory-bounded (`Interest.h:368-431`). This directly
  implements the networking-review's Wall-1 recommendation.

### 3.2 Findings

- **MT-1 (high, "built but not wired"): the live broadcast still uses the M3
  O(players×entities) path.** `ServerHost::BroadcastSnapshots` calls
  `m_universe->BuildSnapshotFor(conn->PlayerNetId())` (`ServerHost.h:124`), which
  runs `DetectedSet` — a full `ForEach` over every entity, for every player, every
  broadcast (`ServerUniverse.h:410-465`). Meanwhile the scalable path —
  `UpdateInterest()` (grid maintenance, run every tick at `ServerUniverse.h:494`),
  `ChangedFor` (`529-538`), `RecordSent`/`AckBaseline` (`543-552`) — **exists and
  is unit-tested** (`InterestTests.cpp:235-256`, `NeuronCoreTest.cpp:429-440`) but
  **nothing in the server loop consumes it.** Net effect today: the server pays
  for grid maintenance *and* the O(N²) fog rebuild, and gets the scaling profile of
  the latter. **Action (M4):** switch `BroadcastSnapshots` to the
  `ChangedFor → encode delta → RecordSent` path and retire `BuildSnapshotFor` from
  the hot path. This is the single highest-leverage scalability change and it's
  mostly wiring, since the pieces exist.

- **MT-2 (high, "built but not wired"): `ERServer` runs the single-threaded
  `Sleep(1)` loop, not IOCP.** `ERServer.cpp:217-291` is a busy-poll Winsock loop
  (`while RecvFrom > 0 … Sleep(1)`), and `IocpUdpListener` — a complete,
  multi-worker IOCP receiver (`IocpUdpListener.cpp`) — is compiled into the project
  (`ERServer.vcxproj:95`) but **never instantiated by `main`** (it includes
  `WinsockSocket.h`, not the listener). The `Sleep(1)` comment even says "M4
  replaces this with IOCP-driven wakeups" (`ERServer.cpp:290`). **Action (M4):**
  wire the listener in — but only *after* MT-3 below, because the moment multiple
  IOCP workers run, the per-connection state becomes a data race.

- **MT-3 (high, the actual threading blocker): per-connection and world state are
  not thread-safe, and the affinity router is a stub.** The IOCP callback runs on
  an arbitrary worker thread and its own comment forbids touching per-connection
  state there: *"must hand off to the connection's affinitised lane … Do NOT
  mutate per-connection state here"* (`IocpUdpListener.cpp:165-171`). But the
  hand-off doesn't exist — `LaneForEndpoint` is `[[maybe_unused]]`
  (`IocpUdpListener.cpp:101`), and `ServerHost` routes by hashing an `"ip:port"`
  **string** into an `unordered_map` per datagram (`ServerHost.h:57, 227`) with no
  locking. `ServerConnection` holds mutable reliability/crypto sequence/nonce
  state that two workers handling the same peer would corrupt. **Action (before
  any multi-threaded receive):**
  1. Route by the existing 64-bit `connectionToken` into a generation-tagged slot
     array (like the ECS handles), not a string hash — the token already exists for
     this (`ServerHost.h:239-245`); the networking review §4 calls this out too.
  2. Build the per-connection serialization lane (single-consumer queue per
     connection / per worker lane) the IOCP comment assumes, so all mutation of one
     connection's state happens on one thread.
  3. Keep the *sim* single-threaded and fed from those lanes (decode/decrypt on
     net threads → enqueue intents → sim drains them), per the masterplan's §9
     "net threads → enqueue; sim owns state" model.

- **MT-4 (medium): the ECS per-access hash lookup is the dominant per-tick cost and
  a parallelization hazard.** `EntityOf(netId)` is an `unordered_map` lookup
  (`ServerUniverse.h:677-681`) and it's called *constantly* — e.g.
  `MakeSnapshotEntity` calls `GetComponent<Health>` and `GetComponent<OwnerId>`,
  each re-doing `IsAlive` + a storage lookup, for every entity in every snapshot
  (`ServerUniverse.h:690-703`); `StampReplication`, `SimHash`, and the combat/AI
  systems all re-resolve handles by net id repeatedly. At hundreds of entities ×
  multiple systems × per-client snapshots this is a lot of pointer-chasing, and
  shared `unordered_map`s are exactly what you *cannot* read concurrently while any
  thread writes. **Action:** (a) hoist the `EntityHandle` once per entity per
  system and pass it down instead of re-resolving by net id; (b) for the eventual
  parallel sim, the net-id→entity and the various `m_revealed`/`m_baselines` maps
  need either partitioning by island or a read-only snapshot taken before the
  parallel region. Profile-driven, but this is the structure that decides whether
  the sim *can* be parallelized cleanly.

- **MT-5 (medium): per-client snapshot encoding is serial and allocates per call.**
  `BroadcastSnapshots` loops connections sequentially (`ServerHost.h:122-131`), and
  each `BuildSnapshotFor`/`ChangedFor` allocates fresh `vector`/`unordered_set`
  (`ServerUniverse.h:419, 455-465, 531-538`). The masterplan's stated lever is a
  "read-only job pool over a frozen post-tick state" — that only works if encoding
  is (a) genuinely read-only and (b) allocation-free on the hot path. Today
  `ChangedFor`/`RecordSent` are *not* `const` (they touch `m_baselines`), and the
  encode allocates. **Action (M4):** split the per-client work into a read-only
  *select+encode* phase (parallelizable, no shared mutation) and a serial
  *commit* phase (`RecordSent`/baseline update), and pre-size/reuse the per-client
  scratch buffers (consistent with the §7.2 "no global-heap alloc in the tick"
  rule, which the net layer doesn't yet honor — networking review §4).

- **MT-6 (low): `EntityHandle` index space vs. "hundreds + churn."** Handles are
  20-bit index / 12-bit generation (`Ecs.h:25-29`) → ~1M live entities and 4096
  generations before wrap. Fine for capacity, but generation wrap (`Ecs.h:214`)
  silently rolls at 4096 destroy/reuse cycles on one slot; over a long-running
  shard with heavy projectile/NPC churn this could in theory alias a stale handle.
  Very low risk, but worth a debug assert or a wider generation field if entity
  churn is high in combat. *Track for the M4 soak test.*

### 3.3 Determinism note for the eventual parallel sim

The determinism harness (`SimHash`, record/replay) depends on the fixed system
order in `Step` (`ServerUniverse.h:483-495`) and ascending-index `ForEach`. Any
parallelization must preserve a deterministic *reduce* order — e.g. partition by
sector island, run islands on a job graph, and merge results in a fixed
(island-id, then entity-index) order. The "nearest target" tie-breaks in
`AiSystem` (`ServerUniverse.h:914-918`) and `CombatSystem` must stay stable under
that partitioning. Keep `SimHash` as the cross-run gate when this lands.

---

## 4. Prioritized summary

| # | Finding | Area | Severity | When |
| --- | --- | --- | --- | --- |
| MT-1 | Live broadcast still uses M3 O(players×entities) `BuildSnapshotFor`; M4 interest/delta path built + tested but unwired | MMO scale | **High** | M4 |
| MT-3 | Per-connection/world state not thread-safe; IOCP affinity lane is a stub — must land *before* IOCP | Threading | **High** | Before MT-2 |
| MT-2 | `ERServer` runs `Sleep(1)` Winsock loop; `IocpUdpListener` compiled but unused | Threading | **High** | M4 (after MT-3) |
| MT-4 | `EntityOf` hash lookup per component access dominates per-tick cost & blocks clean parallelism | MMO scale | Medium | M4 / before parallel sim |
| MT-5 | Per-client encode is serial + allocates; not yet a read-only/commit split | MMO scale | Medium | M4 |
| DX-1 | Hard `kMaxEntities=512` render cap with silent truncation; tie to interest budget | DX12 | Medium | M4 |
| DM-1 | Sim hot path scalar (by design for determinism); SIMD lever if it shows on the gate | DXMath | Info | Profile-driven |
| DX-2 | Double-buffered (`kFrameCount=2`); consider triple-buffering for pacing | DX12 | Low | When scene heavier |
| DX-3 | `SceneRenderer::Initialize` throwaway queue + blocking upload | DX12 | Low | Opportunistic |
| DX-4 | Root constants (viewProj/lighting) re-set per draw-run | DX12 | Low | If shape runs grow |
| DM-3 | `using namespace` + operator overloads in `GameMath.h` header | DXMath | Low | Cheap now |
| MT-6 | 12-bit generation could wrap under heavy entity churn | MMO scale | Low | M4 soak |
| DX-5 / DX-6 | No PSO disk cache; single command list/thread | DX12 | Info | Designed-for |

---

## 5. Bottom line

- **DirectXMath:** adopted correctly, with the one judgment call (scalar sim) being
  the right one for determinism. No corrective action required; one cleanup (DM-3).
- **DirectX 12:** idiomatic and correct; the limits are caps and single-threaded
  recording, both of which are "designed-for, not built." Address DX-1 with M4's
  interest budget.
- **MMO multithreading:** the design is genuinely parallel-friendly (deferred
  mutation, deterministic order, cell pub/sub), but **before any thread is added**,
  three things must be true that aren't today: the live path must use the M4
  interest/delta pipeline (MT-1), per-connection state must be serialized behind an
  affinity lane (MT-3), and the per-client encode must be a read-only/commit split
  (MT-5). Wiring IOCP (MT-2) without MT-3 first would introduce data races. The ECS
  handle-resolution pattern (MT-4) is the structural decision that most affects
  whether the sim itself can later be parallelized cleanly.

*Scope note: every "High" item is a known M4 (Scale & interest) deliverable, not a
regression — the machinery largely exists and the work is integration plus
thread-safety, not green-field. This review is the validation map for that
integration.*
