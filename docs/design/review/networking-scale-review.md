# EarthRise — MMO Networking Review (scale to hundreds of players)

> **Status:** Review v1 — 2026-06-21
> **Reviewer altitude:** MMO netcode architecture review of [`masterplan.md`](../masterplan.md)
> §8/§9/§17/App. A–B **and** the current M1a implementation in `NeuronCore/`,
> `ERServer/`, `NeuronClient/`.
> **Lens:** the masterplan commits to **~100 players, single shard** at launch but
> states throughout that the design must **grow well past that**. This review treats
> **"hundreds of concurrent players, single contiguous world"** as the real target and
> grades the plan + code against it.

---

## 0. TL;DR

The **transport and security design is genuinely strong** — modern, correct, and
well-reasoned (interest-scoped delta-from-∅ cold start, idempotent last-writer-wins
snapshot records on an unreliable channel, ack-advanced baselines, AEAD nonce
discipline, stateless cookie, replay window). The **M1a implementation is a clean,
honestly-scoped tech slice** and does not pretend to be more than that.

The risk to "hundreds of players" is **not the wire format** — it is the three things
the plan currently defers or describes only in prose:

1. **The per-client snapshot pipeline** (interest sets + baselines + the priority/quota
   scheduler) is the whole ballgame at scale and is today **prose, not an algorithm or
   data structure**. It needs a concrete design *before* M4, because it dictates server
   memory and per-tick CPU.
2. **The simulation is single-threaded by decision** (§9). That is fine at 100; at
   hundreds in a contested sector it is the **first hard wall**, and the plan's only
   answer (a read-only *snapshot* job pool) does not parallelize the **sim** itself.
3. **A single contiguous shard has no graceful-degradation story** for the inevitable
   single-sector pileup (the EVE "Jita"/"everyone in one system" problem). Multi-shard
   is explicitly *deferred to post-launch* (§19), which means the one mechanism that
   actually buys "hundreds in one fight" — **time dilation / sim load-shedding** — is
   not in the plan at all.

There is also **one concrete correctness gap in the §8.4 design** (interest eviction is
not self-healing on an unreliable channel — see Finding F1) that should be fixed in the
spec now, before M4 builds on it.

Nothing here blocks M2 (presentation). These are inputs to **M4 (Scale & interest)**,
which is where the plan must become much more specific, plus a few things to fix in the
**design doc today** while they are cheap.

---

## 1. What the plan gets right (keep it)

These are non-trivial and should be preserved as-is:

- **No bulk world sync; cold start = empty baseline (§8.4).** Treating a new client as a
  client whose acked baseline is ∅ and converging it through the ordinary interest+delta
  loop is exactly right and sidesteps the classic "join storm / world download" problem.
- **Snapshots as idempotent, per-`netId` last-writer-wins facts on the Unreliable channel
  (§8.4, `Snapshot.h`).** Decoupling state replication from the reliable-ordering
  machinery removes head-of-line blocking on the hot path and makes loss cost
  *time-to-converge*, not correctness. This is the right model and the code matches it.
- **AEAD/nonce/replay discipline** — direction‖64-bit packet-number nonce, rekey before
  wrap, sliding replay window checked *before* decrypt (`SecureChannel.h`,
  `ReplayWindow.h`), stateless cookie before ECDH (§8.5). This is production-grade and
  the right shape for a public UDP endpoint.
- **MTU-bounded by construction, no fragmentation/reassembly on the realtime path**
  (`Protocol.h`: `kMaxPayloadBytes = 1200`). Removing fragmentation is a real
  simplification *as long as* the large-reliable-payload escape hatch is acknowledged
  (Finding F4).
- **Server-authoritative intents; prediction deferred** (§8.4, §10.1) — correct ordering
  of risk for a command-driven RTS.
- **Observability is already named as a gate (§21).** "You cannot hold a budget you
  cannot measure" is the right instinct; the per-client byte/convergence counters listed
  are the correct ones.

Credit where due: this is a better-thought-through netcode design than most projects
have at this stage.

---

## 2. The three scaling walls (and what the plan says vs. what it needs)

### Wall 1 — Per-client snapshot CPU & memory (the real MMO bottleneck)

**At hundreds of players this, not bandwidth, is what kills the tick.** "Delta vs. the
client's last *acked* baseline" (§8.4) is correct, but it implies the server holds, *per
client*, enough state to diff against — and recomputes a prioritized, byte-budgeted diff
*per client, per snapshot*. Naïvely that is `O(players × visible-entities)` of both
memory and CPU every 50 ms.

- **At 100 players** this is ~fine and the plan's "read-only job pool over a frozen
  post-tick state" (§9) parallelizes the CPU half adequately.
- **At 500 players** in a 250-entity contested sector, per-client baselines and per-client
  diff work dominate the server. The plan currently has **no data structure** for: how a
  baseline is stored, how interest-set membership is diffed as players move, or how the
  "N most relevant keys this client still lacks" scheduler actually picks.

**Recommendation (design before M4):** specify the snapshot pipeline concretely. Proven
patterns that fit this design:

- **Per-entity, per-tick dirty/version stamping** so the diff is "entities whose version
  > client's acked version," not a field-by-field compare. (You already tag records with
  source `tick` for LWW — extend that into the server-side relevance/version model.)
- **Cell-based publish/subscribe interest**, not per-client pull: each sector cell holds a
  subscriber list; an entity mutation is enqueued once to its cell's subscribers. This
  turns broadcast from `O(players × entities)` into `O(sum of subscriptions)` and makes
  sector enter/leave an explicit event (which Finding F1 needs anyway).
- **A named priority function** for the quota scheduler: e.g.
  `priority = relevance(distance, IFF, is-target, is-own-fleet) × staleness(ticks since last sent)`,
  round-robined into the per-tick MTU byte budget. Today this is the phrase "closest/most
  important first" — make it a function with units so it can be tested and tuned.
- **A per-client baseline memory budget** in App. B (bytes/client × max clients), because
  that is what sizes the shard's RAM.

This is the single highest-leverage thing to add to the plan.

### Wall 2 — Single-threaded simulation

§9 commits to "a **single-threaded 30 Hz simulation** owns state." The only scaling lever
named is the **snapshot** job pool — which parallelizes *encoding*, not the *sim*. At
hundreds of entities with combat + PvE AI + projectiles + warp interdiction, one core
holding 33.3 ms is optimistic, and the determinism requirement ("client and server step
identically," §7.2) makes naïve threading hard.

**Recommendation:**
- **Design the sim for spatial/island parallelism now, even if you run it serial at
  launch.** Sector-partitioned systems that only interact within an island can run on a
  job graph deterministically (fixed reduce order). Retrofitting this after systems are
  written is far more expensive than reserving for it.
- **Budget the sim explicitly in App. B** (per-entity per-system µs × worst-case entity
  count) and add a **contested-sector sim-time** perf gate at M4, separate from the
  bandwidth gate. The current M4 gate measures bandwidth only.
- Keep determinism by making parallelism a *scheduling* property (partition + ordered
  merge), not a data-race.

### Wall 3 — Single contiguous shard, no graceful degradation

One world + EVE-style go-anywhere **guarantees** an occasional single-sector pileup. The
plan's interest budget caps what each *client* sees, but the **server still simulates
every entity in that sector on one thread** — and there is no mechanism to shed load when
it can't keep 30 Hz. Multi-shard is deferred to post-launch (§19), which is a reasonable
launch call, but it leaves a gap.

**Recommendation:** adopt **time dilation (TiDi)** as the explicit graceful-degradation
mechanism — the EVE answer to exactly this architecture. When a tick overruns its budget,
slow the authoritative clock (stretch the fixed step) instead of dropping ticks or
falling over. This is **cheap to design in now and expensive to retrofit** because it
touches the fixed-step accumulator (§7.2), the clock-sync handshake (§8.5 step 4), and
client interpolation (which must follow server time, not wall-clock). Note it as the
launch load-shedding strategy and multi-shard as the post-launch capacity strategy; they
are complementary, not alternatives.

---

## 3. Design-level findings to fix in the masterplan now (cheap today)

### F1 — Interest eviction is **not self-healing** on the Unreliable channel *(correctness)*

§8.4 says an entity leaving a client's interest set "is sent **once** as a leave/despawn
record," while snapshots ride the **Unreliable** channel and the server delta-encodes
against the **last acked** baseline. These two rules combine into a latent bug:

- If that single despawn datagram is lost, the next snapshot re-deltas from the acked
  baseline — but the entity is **no longer in the player's interest set**, so the server
  emits *nothing* about it. The client keeps a **ghost entity forever**.

The rest of the snapshot model is self-healing precisely because the server keeps
re-deltaing live entities; eviction is the one event that, once dropped, has nothing to
re-heal it. **Recommendation:** make despawn/leave reconciled against the acked baseline,
not fire-and-forget — i.e. the server keeps a per-client "pending removals" set and keeps
re-sending each removal until the client acks a **baseline that no longer contains that
`netId`**. (Equivalently: carry removals as part of baseline diffing, with a tombstone, so
they ride the same ack-advanced convergence guarantee as everything else.) Cheap to
specify now; painful to discover at M4 in a 200-entity fight.

### F2 — Snapshot record is fixed-width and uncompressed *(bandwidth at scale)*

`Snapshot.h` encodes each entity as `netId u32 · kind u8 · pos 3×i64 · localOffset 3×f32 ·
hp i32` ≈ **46 bytes**, full absolute `int64` position every time. App. B's "~16 B/delta"
target assumes compression that does not exist yet. For hundreds of entities this is the
difference between holding the budget and not.

**Recommendation (M4):** the wire format should send **sector-local quantized deltas**,
not absolute `int64` per axis — the entity is in the client's interest set, so its sector
is known and `int64` need never go on the wire (mirrors the existing "no `int64` reaches
the GPU/audio" rule). Bit-pack via the existing `BitStream.h` (already built, currently
unused by the snapshot path). Add changed-field masks so a stationary entity costs ~0.
This is a known M4 item — just make sure App. B's 16 B figure is treated as a *gate to
hit*, not an assumption.

### F3 — App. B's bandwidth math stops at "100, dispersed"; size for hundreds-contested

App. B re-derives for fleets (~180–250 entities, ~80 KB/s/client in a brawl) but still
frames the shard as "sized for ~100." For hundreds, also state **server aggregate egress**
(N clients × per-client) and the **per-tick encode CPU** at the contested-sector worst
case — those, not per-client kbit/s, are what fail first. **Recommendation:** add a
"hundreds, single contested sector" row to App. B with explicit entity-aggregation/LOD
*on* (fleet-as-cluster), and make **entity aggregation a committed M7 feature, not an
optional lever** — at hundreds it is mandatory.

### F4 — No path for **large reliable payloads** *(MTU invariant gap)*

The "everything fits one ~1200 B datagram, no fragmentation" invariant (§8.2/§8.4) holds
for snapshots and intents, but some **reliable gameplay payloads can exceed MTU**: a full
ship fit, a large cargo/inventory listing, a market order book page, mail, a territory
state blob. §8.4 routes *assets* out-of-band and *world state* through interest-deltas,
but these structured UI/reliable payloads fit neither bucket cleanly.

**Recommendation:** state the escape hatch explicitly — either (a) an **application-level
chunked reliable message** (sequence of MTU-sized reliable fragments reassembled above the
transport, keeping the transport fragmentation-free), or (b) **fetch large/cold UI data
over the out-of-band HTTP path** like assets. Pick one per data class and write it down;
otherwise the first >1200 B reliable message at M5/M7 forces an unplanned transport change.

### F5 — `ReliableOrdered` is a single per-channel stream → head-of-line risk *(at scale)*

Commands, chat, **and** events share `Channel::ReliableOrdered` (§8.2, `Protocol.h`). At
scale a burst of one (e.g. chat/event spam, or a dropped command's retransmit) head-of-line
-blocks the others on that connection. **Recommendation:** separate **gameplay commands**
from **chat/social/events** so a stall in one cannot delay the other — either distinct
reliable channels or sub-streams keyed within the channel. Cheap to note now; the channel
enum is a versioned wire change later.

---

## 4. Implementation-level notes (current M1a — mostly "correct for a slice")

The code does **not** overreach, and the comments honestly mark the deferrals
(`ServerWorld.h:4`, `Snapshot.h:15`, `IocpUdpListener.h:17`). The items below are flagged
so they are tracked toward M4, **not** as defects in M1a:

- **`ServerHost::BroadcastSnapshots` builds one full snapshot and sends identical bytes to
  every connection** (`ServerHost.h`), and `ServerWorld::BuildSnapshot` iterates **all**
  entities with no culling. This is the documented M1a behavior; at scale it is the
  `O(players × entities)` broadcast that Wall 1 replaces with per-client interest diffs.
  *Track: M4.*
- **Connections are keyed by `"ip:port"` strings in an `unordered_map`** (`ServerHost.h`),
  re-hashed per datagram. The 64-bit `connectionToken` already exists to be an O(1) route
  key. **Recommendation:** route by token into a **slot/index array** (generation-tagged,
  like the ECS handles) so the IOCP per-connection-affinity lanes (`IocpUdpListener.h`,
  currently a `[[maybe_unused]]` stub) can dispatch by lane without string hashing on the
  hot path. *Track: M4, alongside finishing the IOCP affinity router.*
- **`ReliableReceiver` keeps received sequences in an `unordered_set`, pruned to ~1024**
  (`Reliability.h`), and `ReliableSender` holds in-flight in an `unordered_map`. Fine
  functionally; at hundreds of connections these per-connection allocating containers add
  up. **Recommendation:** before M4 load testing, move per-connection reliability state to
  **fixed-size ring/bitset** structures (the ack-bitfield model already implies a sliding
  window) to bound per-connection memory and avoid hot-path heap traffic — consistent with
  the §7.2 "no global-heap alloc in the tick" rule, which currently applies to the sim but
  not the net layer.
- **`ReplicaSet::kMaxEntities = 512`** client cap (`Replica.h`) must be reconciled with the
  per-client interest budget (≤250 visible, App. B). Make the client cap a function of the
  interest budget, not an independent magic number, so they can't silently disagree.
- **No backpressure / send-quota on the server's outbound path yet.** When interest +
  priority scheduling lands, the per-tick **byte budget per client** must be enforced at
  the encoder (§8.4 says "filled to the safe-MTU byte budget … spill into later ticks") —
  ensure the implementation actually caps and spills rather than growing the datagram.
  *Track: M4 — it's the core of the scheduler.*

---

## 5. Process / validation recommendations

- **Raise the M4 load target to the real number.** M4 currently gates at "~100 bots"
  (§17). If "hundreds" is the intent, M4 (or a M4.5) must load-test at the **target
  player count** *and* the **single contested sector** worst case — the dispersed-100 case
  will pass while the failure mode hides in the pileup. R16 already calls for the
  contested-sector test; make it the *primary* M4 gate, not a footnote.
- **Add a soak/24-7 test** that ties §26 (rolling restart) to scale: hundreds of bots
  across a warm-restart, measuring reconnect thundering-herd (R22) at the real count.
- **Gate on sim tick time, not just bandwidth.** Add a per-tick p99 sim-time gate at the
  contested-sector entity count (Wall 2). Bandwidth and sim are separate failure modes.
- **Wire the §21 counters before M4, not during it** — cold-start convergence time,
  per-client downstream bytes, and per-tick encode time are the numbers that tell you
  which wall you hit first. They should exist when the first 200-bot test runs.

---

## 6. Prioritized recommendation summary

| # | Recommendation | Type | When | Risk if skipped |
| --- | --- | --- | --- | --- |
| F1 | Make interest eviction self-healing (reconcile despawns against acked baseline / tombstones) | Correctness | **Spec now** | Ghost entities in fights |
| 1 | Specify the per-client snapshot pipeline: cell pub/sub interest, per-entity version/dirty stamping, a named priority function, per-client baseline memory budget | Design | **Before M4** | Server CPU/RAM blows up at scale; M4 has nothing concrete to build |
| 2 | Adopt **time dilation** as the launch graceful-degradation mechanism | Design | **Spec now** (touches §7.2/§8.5/interp) | Single-sector pileup → server can't hold 30 Hz, no fallback |
| 3 | Design the sim for spatial/island parallelism; add a contested-sector **sim-time** perf gate | Design + gate | Before M6 systems land | Single core wall at hundreds; expensive retrofit |
| F2 | Quantized sector-local **delta** snapshot encoding (use `BitStream.h`, changed-field masks) | Impl | M4 | Bandwidth budget (App. B 16 B) unmet |
| F4 | Define the **large-reliable-payload** path (app-level chunking *or* out-of-band) | Design | Before M5/M7 | First >1200 B reliable msg forces transport change |
| F5 | Split gameplay commands from chat/events on reliable channels | Design | Before channel freeze | Head-of-line stalls under load |
| F3 | App. B: add "hundreds, contested" row + server aggregate egress; commit entity aggregation as M7 feature | Design | Before M4 | Wrong shard sizing |
| 4 | Route connections by `connectionToken` into a slot array; finish IOCP affinity router | Impl | M4 | String-hash + single-thread ceiling (~20–30 players) |
| 5 | Fixed-size ring/bitset per-connection reliability state; no hot-path heap | Impl | Before M4 load test | Per-connection alloc churn at hundreds |
| 6 | Raise M4 load target to the real number; make contested-sector the primary gate; soak across warm-restart | Process | M4 | "Passes at 100, dies at 400" |

---

*Scope note: M1a/M1b are complete and M2 (presentation) is active; none of the above
blocks M2. Items marked "spec now" (F1, time dilation, App. B rows, channel split) are
masterplan edits that are cheap today and load-bearing for M4 (Scale & interest), which is
where this design must stop being prose and become data structures, budgets, and gates.*
