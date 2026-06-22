# M4 — Scale & Interest (Implementation Plan)

> Derived from [`../masterplan.md`](../masterplan.md) §17 (milestone **M4**).
> **Status:** 🔨 In progress — **areas A (cell pub/sub interest), B (version stamping +
> per-client baselines) and C (quantized sector-local delta codec) implemented** with their
> `NeuronCoreTest` cases + Linux `testrunner` mirror (§16.2); areas D–J not started.
> (M0/M1a/M1b/M2 complete; M3 active.) Drafted from `_template.md` as the next milestone
> after M3, per [`README.md`](README.md).
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
  - [ ] **Pending-removal (tombstone) set** per client (in the area-B baseline): populated on a
        cell **leave** event (area A) or entity destruction (`DestroyUnit`/`DespawnBase`).
  - [ ] **Re-emit until acked:** a tombstone record (`netId` + tombstone flag, App. A) rides every
        snapshot to that client until the client acks a baseline without that `netId`, then the entry
        is cleared (one record until acked, then absent).
  - [ ] **Wire it to base/ship/NPC despawn** so disconnect (`ServerHost::PruneStale` →
        `DespawnBase`) and combat kills (M3 area F) reconcile cleanly across the new clients.
- **Tests (`NeuronCoreTest`/`ERServerTest`; testrunner mirror):**
  - [ ] An entity leaving interest is reported despawned; with the leave record **dropped once**, the
        next snapshot re-emits it (no ghost), and an ack clears the tombstone.
  - [ ] A destroyed entity tombstones for all subscribers; a reconnecting/late ack still converges.
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
  - [ ] **Priority function** (`Interest.h`, pure + testable): `priority(e, client) =
        relevance(distance, IFF, is-target, is-own-fleet, is-base) × staleness(ticks since last sent
        to this client)`. Weights are **tunables** (the §19 open question), not literals (§16.3, §21).
  - [ ] **Quota scheduler:** round-robin the changed/lacked entities (area B) into the per-tick MTU
        byte budget (area C) in descending priority; the remainder spills to later ticks.
  - [ ] **Cold-start = ∅ baseline:** a fresh client is just one whose acked baseline is empty — the
        same scheduler dribbles its area of interest in, closest/most-important first, converging
        under the ~100 ms interpolation delay (no separate transfer phase, no `Bulk` channel).
  - [ ] **Per-client visible-entity cap (R16):** a hard cap N; when interest exceeds it, the lowest-
        priority entities age out via staleness rather than blowing the budget. M4 **measures** where N
        binds — that's the evidence that **aggregation/LOD is mandatory at M7** (App. B, R16).
- **Tests (`NeuronCoreTest`; testrunner mirror):**
  - [ ] Priority orders a known scene correctly (own base/target/near > distant neutral); units check
        on `relevance`/`staleness`.
  - [ ] Cold-start from ∅ converges within a bounded tick count under the MTU budget; high-priority
        entities arrive first.
  - [ ] Over-budget interest spills deterministically; the visible cap is never exceeded.
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
  - [ ] **Frozen read view:** after the tick advances, freeze the version/position/interest state the
        encoder reads (no concurrent sim writes during encode) — the job pool is **read-only** by
        construction, preserving determinism (encode is a pure projection of post-tick state).
  - [ ] **Job pool** (`ERServer`, or a NeuronCore-side thread-pool seam so the testrunner can drive
        it deterministically): partition clients across workers; each worker runs the area-E scheduler
        + area-C codec for its clients; results gathered for the net layer to seal/send.
  - [ ] **Rewire `BroadcastSnapshots`** to dispatch to the pool and collect sealed datagrams (the
        seal/AEAD still happens per-connection, area G).
- **Tests (`ERServerTest`/`NeuronCoreTest`; testrunner mirror):**
  - [ ] Pooled encode produces byte-identical output to a single-threaded reference for the same
        frozen state (determinism preserved).
  - [ ] Encode throughput scales with worker count (a perf micro-bench feeding the App. B encode gate).
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
  - [ ] **Generation-tagged slot table** keyed by `connectionToken` (replace `m_conns`/`m_lastSeenMs`
        string maps in `ServerHost.h`): `token → {slot index, generation}`; datagrams carry the token
        (App. A header) and dispatch by lane with **no hot-path hashing/allocation**. Keep a small
        first-packet `ip:port → token` association only for the pre-token cookie phase.
  - [ ] **Fixed-size per-connection reliability state:** confirm/convert sequence/ack/replay state to
        **ring buffers / bitsets** (no per-message hash containers), extending the §7.2 no-global-heap-
        in-tick rule to the net layer (`Connection.h`/`Reliability.h`/`ReplayWindow.h`).
  - [ ] **IOCP per-connection affinity:** decode/reliability/decrypt for a connection runs on one lane
        (per-connection-affinitised or lock-free per-conn queue) so IOCP threads never race a
        connection's sequence/nonce/decrypt state (`ERServer/IocpUdpListener`).
- **Tests (`ERServerTest`; testrunner mirror for the routing table):**
  - [ ] Token routing hits the right connection; a recycled slot with a stale token/generation is
        rejected (stale-handle safe).
  - [ ] No per-datagram heap allocation on the routing hot path (allocation-counting assert).
  - [ ] Concurrent datagrams for distinct connections never touch shared per-conn state (affinity
        invariant), exercised under the multi-bot harness (area J).
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
  - [ ] **Dilation factor in the accumulator** (`FixedStepAccumulator.h`): when the accumulator can't
        drain (tick overruns budget consistently), stretch the **real-time spacing** of the fixed step
        toward a floor (e.g. down to 10% speed); the step **count and order are unchanged** (determinism
        preserved — only spacing dilates). Recover when load drops.
  - [ ] **Publish the factor** in the clock-sync echo (§8.5) so interpolation/prediction track the
        dilated authoritative clock; the client interpolator consumes server time, not wall-clock.
  - [ ] **Island-aware sim structure** (`ServerUniverse::Step`): partition systems over sector
        **islands** that only interact within an island, with a **fixed, ordered reduce** (determinism
        = scheduling property, not a data race). **Execute serially at launch**; reserve the partition
        so retrofitting after combat/economy systems exist is avoided. (Per-island dilation is a later
        refinement; M4 dilates the shard.)
- **Tests (`NeuronCoreTest`; testrunner mirror):**
  - [ ] Under a synthetic overrun, the accumulator dilates to the floor and recovers; **`SimHash`
        (M3) is identical** dilated vs un-dilated for the same input log (only timing differs).
  - [ ] Clock-sync echo carries the current dilation factor; the interpolator stays correct while a
        sector is dilated.
  - [ ] Island partition + ordered reduce produces the same `SimHash` as the serial path (parallel
        structure is determinism-preserving even when later run concurrently).
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
  - [ ] **Sim counters:** tick time **p50/p99**, **encode time p99** (area F), catch-up/dilation
        state (area H), entity/system counts per tick.
  - [ ] **Net counters:** per-client downstream/upstream bytes, loss/retransmit/reorder, **cold-start
        convergence time** (∅ → interest set, area E), AEAD-auth failures, replay-window rejects.
  - [ ] **Per-client baseline RAM** (area B) reported as an explicit gauge (App. B gate).
  - [ ] **Export** as structured logs + lightweight counters (MS-only: perf counters / ETW / log
        lines), **consumable by the ERHeadless harness** so the perf gate is automated (§16.3).
- **Tests (`ERServerTest`/`ERHeadlessTest`; testrunner mirror for the counter math):**
  - [ ] p50/p99 aggregation is correct over a known sample; byte counters sum to the bytes actually
        sent; baseline-RAM gauge matches the allocated map sizes (area B).
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
  - [ ] **Scale the bot host** to hundreds of NeuronClient sessions (each own UDP port, §10.3); reuse
        the M3 phase-machine bots, packed into one contested sector.
  - [ ] **Contested-sector pileup scenario (primary):** drive ~hundreds of bases+fleets into one
        sector so the case the dispersed run *hides* (R23) is the gate — measure sim p99, encode p99,
        per-client downstream, per-client baseline RAM (area I) against App. B.
  - [ ] **Dispersed scenario (control):** the same count spread across sectors, to show interest
        culling holds the budget when *not* contested (proves the pileup is the binding case, R23).
  - [ ] **Degradation assertion:** under overload the shard **dilates** (area H) and stays correct —
        **no dropped ticks**, `SimHash` consistent; bandwidth and baseline-RAM stay within App. B.
  - [ ] **Counters wired before the run** (area I), and a documented record of where the **visible cap
        binds** — the evidence that M7 aggregation/LOD is mandatory, not optional (R16).
- **Tests (`ERHeadlessTest`; testrunner where platform-independent):**
  - [ ] Contested-sector run holds **sim p99 < 33.3 ms** *and* per-client downstream ≤ App. B,
        degrading via dilation (the **primary** gate).
  - [ ] Per-client baseline RAM ≤ App. B budget at the target count.
  - [ ] Dispersed control run passes comfortably (interest culling effective).
  - [ ] Eviction/cold-start correctness under simulated loss/reorder/dup at scale (areas C–E).
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

- [ ] **Contested-sector load test holds the frame budget under two separate gates** — per-tick
      **sim time p99 < 33.3 ms** *and* per-client **bandwidth** ≤ App. B (J, area I counters).
- [ ] **Degrades via bounded time dilation, never ticket-dropping** — overload slows in-game time to
      the floor; `SimHash` stays consistent; no ticks dropped (H, J).
- [ ] **Per-client downstream + per-client baseline RAM measured vs the App. B budgets** (B, C, I, J).
- [ ] **§21 net/sim counters wired *before* the run** (I), and the dispersed control case passes (J).
- [ ] **Token-indexed routing + IOCP per-connection affinity** replace the `ip:port` string map (G).
- [ ] All matching `<project>Test` suites green (§16.1) + Linux `testrunner` mirrors for the
      platform-independent pipeline/routing logic (§16.2).
- [ ] Per-milestone perf gate met (§16.3, App. B) — the two-gate contested-sector result is the gate.
