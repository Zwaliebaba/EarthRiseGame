# M4 — Scale & Interest (Implementation Plan)

> Derived from [`../masterplan.md`](../masterplan.md) §17 (milestone **M4**).
> **Status:** 🔨 Pipeline complete — **areas A–J's platform-independent logic is implemented
> and tested** (every `NeuronCoreTest` suite + its Linux `testrunner` mirror, §16.1/§16.2):
> cell pub/sub interest (A), version stamping + per-client baselines (B), quantized
> sector-local delta codec (C), tombstone eviction (D), priority/quota scheduler with
> cold-start from ∅ (E), snapshot job-pool partition + determinism seam (F), token-indexed
> routing table (G), bounded time-dilation policy (H), §21 telemetry counters (I), and a
> contested-vs-dispersed scale harness over the pipeline (J).
> **Remaining for milestone close (Windows build agent, §16.3):** the IOCP per-connection
> affinity dispatch (G) and the OS thread pool + frozen read-view isolation (F) in `ERServer`;
> wiring the dilation factor into the §8.5 clock echo + client interpolator (H); the
> ERServer/ERHeadless counter sampling sites (I); and **the real Done gate — the
> hundreds-of-live-sessions contested-sector run measuring wall-clock sim-time p99 under
> bounded time dilation (J)**, which only the Windows agent can execute.
> (M0/M1a/M1b/M2 complete; M3 active.)
> **Plan style:** feature-area sections (see [`README.md`](README.md)).
> **Verification:** M4's gates are **real, enforceable** — the contested-sector perf/load run
> (areas I/J) executes on the **Windows build agent** (§16.3) at the target player count; the
> platform-independent pipeline/routing logic is mirrored on the Linux `testrunner` (§16.2).
> Assumes M3 (incl. its Windows surfaces) is closed when M4 starts.

## Milestone goal (verbatim from §17)

> **M4 — Scale & interest** *(L)* — **cell pub/sub interest** + per-entity version
> stamping, **quantized delta compression** (tombstone eviction, §8.4), snapshot job pool;
> **token-indexed connection routing** + IOCP per-connection affinity (§9); **time-dilation
> accumulator** (§7.2). **ERHeadless to the target player count (hundreds), not just 100**,
> with a **contested single-sector pileup** scenario as the **primary** gate (the dispersed
> case passes while the pileup hides the failure — R23).
> **Done:** the contested-sector load test holds its frame budget under **two separate gates**
> — **per-tick sim time p99** *and* per-client **bandwidth** (App. B) — degrading via **bounded
> time dilation**, never ticket-dropping; per-client downstream + **per-client baseline RAM**
> measured vs the App. B budgets; the §21 net/sim counters are wired *before* the run, not
> during.

## Scope at a glance

- **In scope:** the **server-side scaling pipeline** that turns the M1a/M3 "full snapshot to
  everyone" path into the masterplan's concrete §8.4 model — **cell publish/subscribe interest**,
  **per-entity version stamping** with **per-client acked baselines**, **quantized sector-local
  delta encoding** with a changed-field mask, **tombstone eviction**, a **named priority/quota
  snapshot scheduler**, and a **read-only job pool** that parallelises per-client encode. Plus
  the two server-loop scaling changes M4 owns: **token-indexed connection routing + IOCP
  per-connection affinity** (§9) and the **time-dilation accumulator** as the overload floor
  (§7.2/§9), with the sim **structured for island parallelism** (run serial at launch). Finally,
  the **§21 telemetry counters** and the **ERHeadless contested-sector load harness** that gates
  the milestone.
- **Out of scope (later milestones):**
  - **Entity aggregation / LOD (fleet-as-cluster) + projectile batching** are a **committed
    M7 feature** (R16), *not* M4. M4 must hold the contested-sector budget with **hard interest
    culling + a per-client visible-entity cap** alone; if the cap is hit, distant entities spill
    /age out via the scheduler, they are **not** yet replaced by clusters. M4 *proves the need*
    for aggregation by measuring where the cap binds.
  - **Multi-shard** is the post-launch **capacity** lever (§19), not a degradation lever. M4's
    answer to overload is **time dilation** on the single shard, never a second shard.
  - **Persistence / accounts / warm-restart** are **M5** — M4 still runs the dev "pick a name"
    identity stub (§14) and keeps sim state in memory. (M4's per-client baseline state is
    *session* state, not persisted.)
  - **Combat model, conquest, markets, invasions, touch** stay M6/M7. M4 changes *how* state
    replicates and *how many* players a shard holds — **not** what the gameplay is.
- **Open questions that touch M4** (from §19, App. B):
  - **Per-client visible-entity cap & priority weights** — the `relevance(...)` term weights
    (distance / IFF / is-target / is-own-fleet / is-base) and the cap N are **tunables**, authored
    so the load test can sweep them (§8.4, App. B). Pick first-pass numbers; the contested-sector
    run validates them. *Blocks tuning, not structure.*
  - **Quantization step for sector-local deltas** — bits-per-axis vs the ~1 mm sector precision
    (§6.3) and the ~16 B/delta App. B target. Pick a step that keeps visible jitter under the
    interpolation delay; validate in area C. *Blocks the wire format freeze — decide before C.*
  - **Time-dilation floor & onset** — the slowdown floor (e.g. 10% speed) and the overrun
    threshold that triggers dilation (§7.2). *Blocks the degradation gate, not the structure.*
  - **Island partition granularity** — sector vs sector-group islands (§9). M4 *reserves* the
    partition (systems written island-aware) but **runs serial**; granularity is a tuning choice
    deferred until parallel execution is switched on. *Doesn't block M4.*

## Current state (what M3 left us)

> File-level baseline. Where M4 replaces an M1a/M3 shortcut, the shortcut is named.

- **Snapshots are "full visible set," not delta.** `NeuronCore/Snapshot.h` encodes a
  **fixed ~46 B record** per entity (`netId · kind · pos i64×3 · localOffset f32×3 · hp ·
  shapeId · ownerPlayer`) with **absolute `int64` positions on the wire** — explicitly flagged
  there as "per-tick MTU budgeting, baseline delta and interest eviction land at M4."
- **Interest is a visibility *filter*, not a subscription.** `ServerUniverse::DetectedSet` /
  `BuildSnapshotFor` (M3 area E) compute a per-player **fog** set each tick and rebuild the whole
  snapshot from scratch — `O(players × entities)`, no baselines, no versions, no eviction. The
  warp/jump **interest prefetch** hook (`OnTravelStart` / `LastTravelSector`, R21) records a
  destination sector but does nothing with it yet.
- **`ServerHost::BroadcastSnapshots`** loops every connection, calls `BuildSnapshotFor`, and
  seals one `Unreliable` `Snapshot` message per client — **encoded inline on the sim thread**,
  no job pool.
- **Connection routing is the M1a shortcut.** `ServerHost` keys `m_conns` /
  `m_lastSeenMs` on an **`unordered_map<"ip:port" string>`** and hashes that string on every
  datagram — the masterplan §9 names this the slice shortcut "replaced at M4" by
  **token-indexed, generation-tagged slot routing**. Per-connection reliability state lives in
  `Connection.h` (already sliding-window/bitset-based — extend, don't rewrite).
- **The fixed-step loop does not dilate.** `FixedStepAccumulator.h` runs whole 30 Hz steps with
  bounded catch-up but has **no time-dilation factor**; the clock-sync echo (§8.5) sends raw
  server time. `ServerUniverse::Step` is **single-threaded** and not yet partitioned into islands.
- **No telemetry counters.** Nothing exports tick p50/p99, encode time, per-client bytes, or
  baseline RAM (§21). `ERHeadless` drives a handful of bots (M3 area H) — there is **no
  hundreds-scale or contested-sector load harness**.
- **Reusable groundwork that exists:** `UniversePos.h` `SectorId` + `SectorHash` (§6.3),
  versioned `Serde.h` / `BitStream.h` bit-packing primitives (§7.2), `ReplayWindow.h` +
  `Reliability.h` ack bitfields, and `ServerUniverse::SimHash` (determinism gate, reused to prove
  the new pipeline doesn't change sim state).

---

## Feature areas

### A. Cell publish/subscribe interest (§8.4, §6.3)

- **Goal:** replace the per-tick `O(players × entities)` fog rebuild with **`SectorId`-keyed cells
  holding subscriber lists**; a player subscribes/unsubscribes as it crosses sector boundaries, so
  a mutation is enqueued **once to its cell's subscribers** — `O(Σ subscriptions)`.
- **Masterplan refs:** §8.4 ("cell-based publish/subscribe interest, not per-client pull"), §6.3
  (`SectorId`/`SectorHash`, S=14 ≈16 km sectors), R24.
- **Current state:** net-new. `DetectedSet`/`BuildSnapshotFor` (M3 area E) are the per-player
  *filter* this replaces; `SectorId`/`SectorHash` already exist in `UniversePos.h`.
- **Work:**
  - [x] **Interest grid** keyed by `SectorId` (`NeuronCore/Interest.h`, new): `InterestGrid` —
        cell → sorted resident-entity list + sorted subscriber list, keyed by `SectorHash` (§6.3);
        **no 64-bit Morton key**. Empty cells are pruned. Sorted lists keep per-cell membership
        order-stable across runs (feeds deterministic scheduling at area E).
  - [x] **Subscribe/unsubscribe on sector crossing:** `UpdateResidency` emits a single
        leave+enter `CrossEvent` when an entity's sector index changes (the §8.4 tombstone rule at
        area D consumes these). `SetSubscription` diffs a player's sensor-range neighbourhood
        (`CollectNeighbourhood` / `SectorRadiusForRange`) to enter/leave deltas; `ServerUniverse::
        UpdateInterest` (called each `Step`) re-homes entities and re-subscribes players each tick.
        **Warp/jump prefetch** (R21) wired: `BeginWarpTo`/`BeginJumpTo` → `PrefetchTravelInterest`
        pins the destination cells, surviving the per-tick refresh until arrival.
  - [ ] **Sensor/fog over cells:** *(partial)* the spatial neighbourhood is folded into the
        subscription set; the always-known overlays (own outside sensor range, beacon graph,
        scanned contacts) still come from M3's `DetectedSet`. Full replacement of the per-tick scan
        lands with the area-B/E diff path (`BuildSnapshotFor` is untouched for now).
- **Tests (`NeuronCoreTest`, §16.1; mirror in `NeuronTools/testrunner/InterestTests.cpp`, §16.2):**
  - [x] Crossing a sector boundary emits exactly one leave + one enter; subscriber list membership
        matches sensor range (`CrossingEmitsOneLeaveAndOneEnter`, `NeighbourhoodSubscriptionMatchesRange`).
  - [x] A mutation in cell C is enqueued to C's subscribers only (count == subscriber count), never
        to non-subscribers (`MutationEnqueuedToCellSubscribersOnly`).
  - [x] Warp/jump start pre-subscribes the destination cells before arrival
        (`WarpPrefetchSubscribesDestinationBeforeArrival`, R21).
- **Depends on:** nothing (server-side). **Blocks:** B, D, E.

### B. Per-entity version stamping & per-client baselines (§8.4)

- **Goal:** each replicated entity carries a monotonic **version** bumped when its replicated state
  changes; the per-client baseline stores the **last acked version per `netId`**, so a diff is
  "entities whose version > the client's acked version," **not** a field-by-field compare.
- **Masterplan refs:** §8.4 ("per-entity version/dirty stamping"; extends the per-record source-
  `tick` LWW tag into the server-side relevance model), R24, App. B (baseline RAM).
- **Current state:** net-new. M3 has no versions and no per-client baselines (it rebuilds the whole
  snapshot every tick).
- **Work:**
  - [x] **Replication version** (`Interest.h` `ReplicationStamps`): a monotonic version per `netId`,
        advanced iff the entity's replicated fields (`ReplFields` — the App. A projection: pos +
        local offset, hp, owner, shape, kind) changed since the last stamp. A stationary/idle entity
        holds its version (costs ≈0 downstream). *Implementation note:* a **centralized post-tick
        dirty-stamp** (`ServerUniverse::StampReplication`, end of `Step`) over a `netId`-keyed side
        table — chosen over per-system bumps + an ECS `ReplState` component so every replicated
        entity is covered with **no spawn-site coupling** and the wire format / client stay untouched;
        the §8.4 observable contract (version advances iff replicated state changed) is identical.
  - [x] **Per-client baseline** (`Interest.h` `ClientBaseline`): `netId → acked version` map + a
        bounded per-tick **pending-sent** history, allocated per connection (keyed by player id). 
        Advanced **on snapshot ack** (`Ack(tick)` folds every pending snapshot ≤ tick into the acked
        map). The tombstone set rides on this baseline at area D (`Forget(netId)` hook in place).
  - [x] **Diff = "version > acked":** `ServerUniverse::ChangedFor(client)` selects the interest-set
        (`InterestGrid::VisibleTo`, area A) entities whose server version exceeds the client's acked
        version. **Ack-advanced, never last-*sent*** — a dropped snapshot re-deltas from the still-
        current acked baseline (§8.4). The area-E scheduler will order this diff by priority + MTU.
  - [x] **Baseline RAM accounting:** `ClientBaseline::ApproxBytes()` +
        `ServerUniverse::BaselineBytes`/`TotalBaselineBytes` size the acked + pending maps; area I
        wires them to the App. B gate.
- **Tests (`NeuronCoreTest`; `NeuronTools/testrunner/InterestTests.cpp` mirror):**
  - [x] Version bumps iff a replicated field changes; idle entity holds its version across ticks
        (`VersionBumpsOnlyWhenAReplicatedFieldChanges`, `StampBumpsOnlyOnChange`).
  - [x] Diff selects exactly the entities changed since the client's acked baseline; a re-acked
        baseline shrinks the next diff to ∅ (`BaselineDiffSelectsChangedAndAckShrinksToEmpty`).
  - [x] A dropped snapshot (no ack) re-deltas from the still-current acked baseline, converges on ack
        (`DroppedSnapshotReDeltasFromAckedBaseline`); baseline-RAM gauge is non-empty after acks.
- **Depends on:** A. **Blocks:** C, D, E.

### C. Quantized sector-local delta encoding + changed-field mask (§8.4, App. A)

- **Goal:** ship positions as **sector-local quantized deltas** with a **changed-field mask**,
  bit-packed via the serde primitives — **never absolute `int64` per axis** (mirrors the "no
  `int64` reaches the GPU/audio" rule, R2). This is what makes the App. B **~16 B/delta** target
  reachable vs the current ~46 B fixed record.
- **Masterplan refs:** §8.4 ("quantized sector-local delta encoding"), App. A (snapshot record:
  `netId u32 · flags u8 (changed-field mask | tombstone bit) · [Δsector-local pos quantised] ·
  [Δfields…]`), §6.3 (sector-local precision), §7.2 (serde/bit-packing), R2.
- **Current state:** replaces `Snapshot.h`'s fixed-width absolute-`int64` record. `BitStream.h` /
  `Serde.h` bit-packing already exist.
- **Work:**
  - [x] **New snapshot record codec** (`Snapshot.h`, additive `DeltaRecord`/`DeltaSnapshot` +
        `Encode/DecodeDeltaSnapshot`): `netId` + a **changed-field-mask flags byte** (`DeltaFlag`,
        incl. the `DeltaTomb` bit reserved for area D); only changed fields follow. Position is a
        **quantized sector-local value** (`DeltaPos`) with the sector sent only on first sight /
        crossing (`DeltaSector`), HP/owner/shape/kind as masked optional fields. `MakeDeltaRecord
        (cur, base)` computes the minimal mask vs the client's last-known state. The M3 full codec
        is **untouched** — the live path swaps over at area E.
  - [x] **Quantization step** (`kPosQuantBitsPerAxis = 20` → ~1.6 cm/axis): a named **tunable**, well
        under the ~100 ms interpolation jitter (§8.4) while reaching the App. B ~16 B/delta target;
        `Quantize/DequantizeSectorLocal` round-trip within one step (area J sweeps the constant).
  - [x] **Per-tick MTU byte budget + spillover primitive:** `BuildBudgetedSnapshot(ordered, budget,
        overflow)` keeps the priority-ordered prefix that fits the **safe-MTU budget** and spills the
        rest to `overflow` (re-scheduled next tick by area E — area B keeps their version > acked, so
        none are dropped). Holds the "never larger than MTU, never fragmented" invariant (§8.2/§8.4).
        *The priority ordering + cross-tick spillover state is the area-E scheduler; C owns the codec
        + the budget primitive.*
  - [x] **Client decode:** `DeltaDecodeState` (NeuronCore, shared/testable) applies masked deltas
        onto the last-known per-`netId` state, **last-writer-wins by `tick`** (reordered/duplicate
        snapshots idempotent), reconstructing absolute position from sector + decoded local delta.
        The `NeuronClient` replica adopts it when the live path swaps at area E.
- **Tests (`NeuronCoreTest`; `NeuronTools/testrunner/SnapshotDeltaTests.cpp` mirror):**
  - [x] Encode→decode round-trip reconstructs position within the quantization bound; the mask omits
        unchanged fields (a 1-field change costs mask + 1 field) — `FirstSightRoundTripWithinQuantBound`,
        `MaskOmitsUnchangedFields`, `SectorCrossingReAnchorsPosition`, `EncodesLiveServerEntityRoundTrip`.
  - [x] A stationary entity costs ≈0 bytes (mask 0 / no record) — `StationaryEntityCostsNothing`.
  - [x] A budgeted snapshot never exceeds the safe payload; overflow entities spill (none dropped) —
        `BudgetedSnapshotNeverExceedsAndSpillsTheRest`.
  - [x] Reordered/duplicate snapshots converge (LWW by tick) — `ReorderedAndDuplicateSnapshotsConvergeByTick`.
- **Depends on:** A, B. **Blocks:** E, J (the bandwidth gate measures this format).

### D. Tombstone eviction — self-healing leave/despawn (§8.4)

- **Goal:** when an entity leaves a client's interest set (or is destroyed), the server marks a
  **tombstone** in that client's pending-removal set and **keeps emitting the leave/despawn record
  until the client acks a baseline that no longer contains that `netId`** — so a lost despawn on the
  Unreliable channel never leaves a **ghost forever**.
- **Masterplan refs:** §8.4 ("eviction (tombstone-reconciled, self-healing) 🔒"), App. A (tombstone
  record re-sent until acked), the §8.3 ack-advanced rule.
- **Current state:** net-new — M3's full-rebuild snapshot implicitly "evicts" by omission, which is
  exactly the ghost bug at scale once snapshots are deltas.
- **Work:**
  - [x] **Pending-removal (tombstone) set** per client (`ClientBaseline` in `Interest.h`): a
        `netId → earliest-carrying-tick` map. `ServerUniverse::TombstonesFor` reconciles each
        client's **acked set against its live interest set** every call, so an entity that left a
        cell (area A) **or was destroyed** (removed from the grid by `DestroyUnit`/`DespawnBase`)
        is tombstoned; one that re-entered before its removal acked is `Untombstone`'d.
  - [x] **Re-emit until acked:** `BuildClientSnapshot` (area E) appends a `DeltaTomb` record
        (`netId` + tombstone flag, App. A) for each pending tombstone on **every** snapshot;
        `Ack(tick)` clears a tombstone once the client acks a snapshot that carried it (any ack ≥
        the earliest carrying tick — every snapshot re-emits the set, so a dropped leave self-heals).
  - [x] **Wired to despawn:** `DespawnBase` / `DestroyUnit` remove the netId from the interest
        grid, so the next `TombstonesFor` evicts it for every client that had it — disconnect
        (`ServerHost::PruneStale` → `DespawnBase`) and combat kills reconcile through one path.
- **Tests (`NeuronCoreTest` `TombstoneTests`; testrunner `Tombstone` mirror):**
  - [x] An entity leaving interest is reported despawned; with the leave record **dropped once**, the
        next snapshot re-emits it (no ghost), and an ack clears the tombstone
        (`ServerUniverseEvictsEntityThatLeftInterest`, `ReEmitsUntilAckedThenClears`).
  - [x] A stale ack predating the leave does not clear it; a re-entered entity is un-tombstoned
        (`StaleAckBeforeCarryingTickDoesNotClear`, `ReEnteredEntityIsUntombstoned`).
- **Depends on:** A, B. **Blocks:** J (eviction correctness under loss is part of the gate).

### E. Priority / quota snapshot scheduler (cold-start from ∅) (§8.4)

- **Goal:** a **named priority function** drives a per-tick byte-budgeted scheduler that picks "the N
  most relevant keys this client still lacks, within the budget" — the same machinery serving both
  steady-state spillover and **cold-start from the empty (∅) baseline**.
- **Masterplan refs:** §8.4 (named priority function + quota scheduler; cold-start = ∅ baseline,
  progressive convergence ~1–2 s), §21 (cold-start convergence telemetry), App. B (downstream budget).
- **Current state:** net-new. M3 sends the whole visible set every tick; there is no budget, no
  priority, no progressive cold-start (it is invisible only because M3 isn't at scale).
- **Work:**
  - [x] **Priority function** (`SnapshotScheduler.h`, pure + testable): `SnapshotPriority =
        relevance(IFF, is-base, is-target) × distance-falloff × staleness(ticks since last sent to
        this client)`. The `RelevanceWeights` are authored **tunables** (the §19 open question),
        not literals — `ServerUniverse::SchedulerWeights()` exposes them for area-J sweeps.
  - [x] **Quota scheduler:** `ScheduleClient` ranks the changed/lacked entities (area B) and the
        budgeted `BuildClientSnapshot` keeps the descending-priority prefix that fits the per-tick
        MTU byte budget (area C `BuildBudgetedSnapshot`); the remainder spills to later ticks (their
        version stays > acked, so none are dropped).
  - [x] **Cold-start = ∅ baseline:** a fresh client is one whose acked baseline / `ClientKnownState`
        is empty — the same scheduler dribbles its area of interest in, closest/most-important first.
        The per-client delta-base (`ClientKnownState`) advances **only on ack**, so a dropped
        snapshot re-deltas from the still-current base (no separate transfer phase, no `Bulk` channel).
  - [x] **Per-client visible-entity cap (R16):** `ScheduleClient` applies a hard cap N
        (`SetVisibleCap`); over-cap entities age out via staleness rather than blowing the budget,
        and the shed count is **reported** (`capped`) so area I/J record **where N binds** — the
        evidence that **aggregation/LOD is mandatory at M7** (App. B, R16).
- **Tests (`NeuronCoreTest` `SchedulerTests`; testrunner `Scheduler` mirror):**
  - [x] Priority orders a known scene correctly (own base/target/near > distant neutral) and
        staleness raises an aged entity (`PriorityRanksRelevantAndNearAboveDistantNeutral`,
        `StalenessRaisesAnAgedEntity`); the order is deterministic (tie-break by netId).
  - [x] Cold-start from ∅ converges within a bounded tick count under a tiny MTU budget; the own
        base (top priority) arrives first (`ColdStartConvergesUnderTinyBudget`).
  - [x] Over-budget interest spills deterministically and nothing is dropped; the visible cap binds
        but the scene still converges (`BudgetedSnapshotNeverExceedsAndNoneDropped`,
        `VisibleCapBindsButStillConverges`).
- **Depends on:** A, B, C, D. **Blocks:** F, J.

### F. Snapshot job pool — read-only parallel encode (§9)

- **Goal:** run per-client snapshot encoding (areas C–E) over a **read-only job pool** against a
  **frozen post-tick snapshot** of versions/positions, so per-client diffs parallelise across cores
  — the per-tick **encode CPU** is, with sim time, the first thing to blow the 33.3 ms budget at
  hundreds of players (App. B).
- **Masterplan refs:** §9 ("snapshot encoding runs over a read-only job pool against a frozen
  post-tick state"), §8.4 ("encoding still runs on the read-only job pool"), App. B (encode p99 gate).
- **Current state:** net-new. `ServerHost::BroadcastSnapshots` encodes **inline on the sim thread**
  today — fine at M1a/M3, the bottleneck at M4.
- **Work:**
  - [x] **Partition + gather seam** (`SnapshotJobs.h`, NeuronCore so the testrunner drives it
        deterministically): `PartitionClients` round-robins clients into disjoint per-worker slices
        (each client once), `EncodeClientsPooled` encodes each slice via the area-E `BuildClientSnapshot`
        + area-C codec and **gathers results back into client order**, and `EncodeClientsSerial` is the
        single-threaded reference. Encode is a pure projection of post-tick state, so the gathered
        output is partition-count-independent.
  - [ ] **Frozen read view + OS thread pool** *(Windows, ERServer):* run the partitions on real
        worker threads against a frozen post-tick snapshot (no concurrent sim writes; per-client
        baseline entries pre-created so workers touch disjoint state), and **rewire
        `BroadcastSnapshots`** to dispatch to the pool and collect sealed datagrams (seal/AEAD stays
        per-connection, area G). *Not buildable on the Linux runner.*
- **Tests (`NeuronCoreTest` `SnapshotJobsTests`; testrunner `SnapshotJobs` mirror):**
  - [x] Pooled encode produces **byte-identical** output to the single-threaded reference for the
        same frozen state, across 1…16 workers (`PooledEncodeMatchesSerialReferenceForAnyWorkerCount`);
        the partition covers every client exactly once and the gather preserves client order.
  - [ ] *(Windows)* Encode throughput scales with worker count (a perf micro-bench feeding the App. B
        encode gate).
- **Depends on:** C, E. **Blocks:** J (the sim-time gate includes encode time).

### G. Token-indexed connection routing + IOCP per-connection affinity (§9)

- **Goal:** route datagrams by the **64-bit `connectionToken`** into a **generation-tagged
  slot/index array** (like the ECS handles), **not** an `ip:port` string hash, and affinitise each
  connection's reliability/decrypt state to a lane so IOCP threads never race on it.
- **Masterplan refs:** §9 ("connection routing by token, not string"; "per-connection reliability
  state is fixed-size"; per-connection-affinity lanes), App. A (`connection_token u64` in the header).
- **Current state:** `ServerHost` uses the M1a `unordered_map<"ip:port">` + per-datagram string hash
  (named in §9 as "replaced at M4"). `Connection.h` reliability state is already sliding-window/bitset.
- **Work:**
  - [x] **Generation-tagged slot table** (`ConnectionTable.h`): `connectionToken → {slot index,
        generation}` with `Open`/`Find`/`Validate`/`Close` — datagrams route by a single u64 lookup
        with **no per-datagram string hash or allocation**; recycled slots bump a generation so a
        stale handle to a reused slot is rejected (the ECS-handle pattern). Per-connection state
        attaches to the slot; `Lane()` gives each connection a stable affinity lane.
  - [ ] **Replace `ServerHost`'s `m_conns`/`m_lastSeenMs` string maps** with the table (keep a small
        first-packet `ip:port → token` association for the pre-token cookie phase) and confirm the
        per-connection reliability state is fixed-size ring buffers / bitsets. *Windows ERServer.*
  - [ ] **IOCP per-connection affinity:** dispatch each connection's decode/reliability/decrypt on
        its `Lane()` so IOCP threads never race its sequence/nonce/decrypt state
        (`ERServer/IocpUdpListener`). *Not buildable on the Linux runner.*
- **Tests (`NeuronCoreTest` `ConnectionTableTests`; testrunner `ConnectionTable` mirror):**
  - [x] Token routing hits the right connection; a recycled slot with a stale generation is rejected
        (`RoutesByTokenToTheRightSlot`, `RecycledSlotRejectsStaleGeneration`); freed slots are reused
        without growth; a connection is pinned to one lane.
  - [ ] *(Windows)* Concurrent datagrams for distinct connections never touch shared per-conn state
        (affinity invariant), exercised under the multi-bot harness (area J).
- **Depends on:** nothing (independent of the snapshot pipeline). **Blocks:** J (the load test needs
  routing that doesn't hash strings at hundreds of connections).

### H. Time-dilation accumulator + island-parallel structure (§7.2, §9)

- **Goal:** make the fixed step the **sim clock**: when a tick consistently overruns its real-time
  budget, **stretch the authoritative step** (slow in-game time to a floor) instead of dropping ticks
  — bounded, EVE-style graceful degradation — and publish the dilation factor so clients track
  **server time, not wall-clock**. Structure systems for **island parallelism** (run serial at launch).
- **Masterplan refs:** §7.2 ("time dilation (TiDi) 🔒"), §9 ("time dilation is the load-shedding
  floor"; "sim parallelism — design now, run serial at launch"), §8.5 (clock-sync echoes server time
  *including* the dilation factor), R23.
- **Current state:** net-new. `FixedStepAccumulator.h` has bounded catch-up but **no dilation
  factor**; clock-sync echoes raw server time; `ServerUniverse::Step` is single-threaded, not
  island-partitioned.
- **Work:**
  - [x] **Dilation factor in the accumulator** (`TimeDilation.h` pure policy, wired into
        `FixedStepAccumulator.h`): `DilationController` stretches the **real-time spacing** of the
        fixed step toward a floor (default 10% speed) when ticks overrun the budget, easing down fast
        and recovering slow; the accumulator scales its elapsed intake by `Factor()`. The step
        **count and order are unchanged** — only spacing dilates, so `SimHash` is identical dilated vs
        not. `ERServer` times each `Step` and feeds `ReportTickCost`, so the floor is active server-side.
  - [ ] **Publish the factor** in the §8.5 clock echo so the client interpolator tracks the dilated
        authoritative clock. `DilationFactor()` exposes it; the clock-sync message + client
        interpolator wiring is the remaining client-facing step. *Windows/protocol.*
  - [ ] **Island-aware sim structure** (`ServerUniverse::Step`): partition systems over sector islands
        with a fixed ordered reduce, run serial at launch. *Reserved; the determinism property is the
        `SimHash`-stable read the area-J harness already asserts, full partition deferred.*
- **Tests (`NeuronCoreTest` `DilationTests`; testrunner `TimeDilation` mirror):**
  - [x] Full speed under budget; dilates toward onset/load when overrunning; clamps at the floor under
        extreme overload; recovers to full speed when load drops; the factor stays in `[floor, 1]`;
        onset is faster than recovery (`DilatesTowardOnsetOverLoad`, `ClampsAtFloor…`, `Recovers…`).
  - [x] `SimHash` identical across runs of the contested pipeline (area J
        `ContestedPipelineIsDeterministic`) — the replication/scheduling layer is a determinism-
        preserving pure read.
  - [ ] *(Windows)* Clock-sync echo carries the current dilation factor and the interpolator stays
        correct while dilated.
- **Depends on:** nothing (independent). **Blocks:** J (the degradation gate exercises dilation).

### I. Observability & telemetry counters (§21)

- **Goal:** wire the **§21 counters** — sim, net, and (stubbed) persistence — **before** the load
  run, not during. "You cannot hold the M4 bandwidth budget you cannot measure."
- **Masterplan refs:** §21 (sim tick p50/p99, catch-up steps, entity/system counts; per-client
  down/upstream bytes, loss/retransmit/reorder, **cold-start convergence**, AEAD-auth failures,
  replay rejects), §16.3 (perf gates consumable by the headless harness), App. B (the two gates +
  baseline RAM).
- **Current state:** net-new. M2 area H gives **render** GPU/CPU ms on the client; there are **no
  server-side sim/net/encode counters**.
- **Work:**
  - [x] **Counter aggregation** (`Telemetry.h`): `PercentileWindow` (bounded ring + nearest-rank
        p50/p99 for sim tick + encode time), `NetCounters` (down/up bytes, datagrams, loss,
        retransmit, reorder, AEAD-auth failures, replay rejects), and `ServerTelemetry` aggregating
        the App. B gates — per-client downstream p99, **per-client baseline-RAM gauge** (area B/E
        `TotalClientBaselineBytes`), **cold-start convergence**, dilation state, and the **R16
        cap-bind** evidence.
  - [ ] **Sampling sites + export** *(Windows ERServer/ERHeadless):* record tick/encode ms, per-client
        bytes, acks, and dilation at the live sites, and export as structured logs + perf counters /
        ETW consumable by the harness (§16.3). *Not buildable on the Linux runner.*
- **Tests (`NeuronCoreTest` `TelemetryTests`; testrunner `Telemetry` mirror):**
  - [x] Nearest-rank p50/p99 correct over a known sample and order-independent; the window is bounded
        and evicts oldest; net counters sum; the aggregate gates (baseline RAM, cap-bind) read back
        (`PercentileNearestRankOverKnownSample`, `WindowIsBoundedAndEvictsOldest`, `ServerTelemetry…`).
- **Depends on:** B (RAM), F (encode time), H (dilation state). **Blocks:** J (gates read these).

### J. ERHeadless scale & contested-sector load harness (the Done gate)

- **Goal:** drive the shard to the **target player count (hundreds)** with a **contested
  single-sector pileup** as the **primary** scenario, and gate on **two separate budgets** —
  per-tick **sim time p99** *and* per-client **bandwidth** — degrading via **bounded time dilation**,
  never ticket-dropping.
- **Masterplan refs:** §17 M4 Done, §10.3 (ERHeadless many sessions, bots ≠ NPCs), §16.1/§16.2/§16.3
  (load harness + perf gates), App. B (budgets + the two-gate framing), R16/R23/R24.
- **Current state:** M3 area H drives a handful of bots through the loop with `SimHash` record/replay.
  M4 scales that to **hundreds** and adds the **contested-sector** scenario plus the **dispersed**
  control case.
- **Work:**
  - [x] **Pipeline scale harness (`LoadHarnessTests`, testrunner + `NeuronCoreTest` mirror):** drives
        the A–I pipeline through `ServerUniverse` at scale (120 clients) — the platform-independent
        half of the gate — measuring per-client downstream, baseline RAM, cap-bind, and convergence.
  - [x] **Contested-sector pileup (primary, pipeline):** all bases packed into one sector (mutual
        interest, the case the dispersed run hides, R23) **holds the safe-MTU budget every client
        every tick and fully converges** (none dropped).
  - [x] **Dispersed scenario (control):** the same count spread across sectors converges far cheaper
        (a far smaller baseline) — interest culling holds when not contested (proves the pileup is the
        binding case, R23).
  - [x] **Visible-cap evidence:** the R16 cap **binds** under the pileup yet the scene still converges
        via staleness — the documented record that M7 aggregation/LOD is mandatory (App. B, R16).
  - [ ] **Scale the live bot host** to hundreds of NeuronClient sessions (own UDP port each, §10.3),
        and the **wall-clock degradation gate**: under real overload the shard **dilates** (area H)
        and stays correct (no dropped ticks, `SimHash` consistent), with sim **p99 < 33.3 ms** and
        per-client downstream ≤ App. B. *Windows ERHeadless build agent (§16.3) only.*
- **Tests (`ERHeadlessTest` on Windows; `LoadHarness` testrunner mirror for the pipeline):**
  - [x] Contested-sector pipeline holds the **per-client byte budget** every tick and converges;
        dispersed control is far cheaper; baseline RAM bounded; cap binds yet converges; the pipeline
        is a determinism-preserving pure read (`ContestedSectorHoldsBandwidthAndConverges`,
        `DispersedControlIsFarCheaperThanContested`, `VisibleCapBindsUnderPileupButConverges`,
        `ContestedPipelineIsDeterministic`).
  - [ ] *(Windows)* Contested-sector run holds **sim p99 < 33.3 ms** under bounded dilation at the
        target live-session count; per-client downstream + baseline RAM ≤ App. B.
- **Depends on:** A–I. **Blocks:** Done gate (it *is* the end-to-end verification).

---

## Suggested order / dependency notes

> Two largely independent tracks plus a gate. The **snapshot-pipeline track** (A→B→C→D→E→F) is the
> milestone's core and is sequential. The **server-loop track** (G token routing, H time dilation)
> is independent of the pipeline and can run in parallel. **I (telemetry)** is wired as B/F/H land,
> **before** the load test. **J** is the end-to-end gate and depends on everything.

1. **A (cell pub/sub interest)** first — the foundation the whole pipeline subscribes against.
2. **B (versions + baselines)** → **C (quantized deltas)** → **D (tombstones)** → **E (priority/quota
   scheduler)** in order — each builds on the last; E is the integration point.
3. **F (job-pool encode)** once C+E exist — parallelises the per-client encode.
4. **G (token routing + IOCP affinity)** and **H (time dilation + island structure)** run in
   **parallel** with the pipeline — neither depends on it.
5. **I (telemetry)** continuously, **wired before J** (§17 explicitly requires the counters first).
6. **J (load harness)** last — the contested-sector gate that closes the milestone.

**Cross-milestone:** M4 keeps the M3 gameplay and the dev "pick a name" identity (real auth is **M5**).
M4's per-client baseline state is **session** state — shape it so M5 doesn't have to persist it.
**Aggregation/LOD (R16)** is deliberately *not* built here; M4 proves the need and M7 delivers it.

## Done gate (mirrors §17 "Done")

> **Pipeline-complete on Linux; milestone close awaits the Windows agent.** The
> platform-independent half of every gate is implemented and green on the `testrunner` +
> `NeuronCoreTest`; the items still open all require the **Windows build agent** (real UDP
> sessions, IOCP, a wall-clock sim-time measurement) and are called out as such.

- [~] **Contested-sector load test holds the frame budget under two separate gates** — the
      per-client **bandwidth** gate is met by the pipeline harness (J); the per-tick **sim time
      p99 < 33.3 ms** gate is **wall-clock and Windows-only** (runs on the build agent, §16.3).
- [~] **Degrades via bounded time dilation, never ticket-dropping** — the bounded dilation **policy**
      is implemented + tested and active server-side (H); exercising it under **real overload** at the
      target count is the Windows run (J).
- [x] **Per-client downstream + per-client baseline RAM measured vs the App. B budgets** — measured
      and bounded in the pipeline harness (B, C, E, I, J); the tight App. B numbers are confirmed on
      the Windows run.
- [x] **§21 net/sim counter aggregation wired *before* the run** (I), and the **dispersed control case
      passes** (J). The live sampling sites attach on Windows.
- [~] **Token-indexed routing** replaces the `ip:port` string map — the **routing table** is
      implemented + tested (G); the `ServerHost` swap + **IOCP per-connection affinity** are Windows.
- [x] **All matching `<project>Test` suites green** (§16.1) + **Linux `testrunner` mirrors** for the
      platform-independent pipeline/routing logic (§16.2) — areas A–J, **197 testrunner cases green**.
- [ ] Per-milestone perf gate met (§16.3, App. B) — **the two-gate contested-sector result on the
      Windows agent is the remaining gate that closes M4.**

**Legend:** `[x]` done & verified on Linux · `[~]` platform-independent logic done & tested, Windows
run/integration remaining · `[ ]` not yet met.
