# 4X Interaction, Command & Procedural Seeding (Implementation Plan)

> Derived from [`../masterplan.md`](../masterplan.md) §8.4 (intents / seed-procedural
> replication), §13 (4X loop & entities), §15 (persistence), §20 (config), and
> [`../design/universe-worldgen.md`](../design/universe-worldgen.md) §5 (the seed-procedural
> model). Companion to [`M3-core-4x-loop.md`](M3-core-4x-loop.md) (the loop) and
> [`M5-accounts-persistence.md`](M5-accounts-persistence.md) (the DB).
> **Status:** ⏳ Not started — this is a planning doc for review, not yet implemented.
> **Plan style:** feature-area sections (see `README.md`).

## Why this plan exists

The 4X **simulation** is largely built and green (288 cases on the Linux `testrunner`).
What is missing is the **bootstrap that makes a *real* server actually playable** plus a
couple of **interaction wire-paths** that exist as sim methods but have no command route.
This plan closes those gaps and lays down the **procedural-seeding architecture** the project
wants: **the authoritative universe lives in the database, seeding is mostly procedural, and
only the seed *parameters* live in a `.universe` file.**

---

## What already exists (verified in-tree)

The interaction/command verbs you asked about are **all implemented as server-authoritative
sim logic**, and the command transport is connected:

| Interaction | Logic | Wire path |
| --- | --- | --- |
| **Build ship** | `BuildQueue`/`BuildStep` (`Economy.h`) → `BuildSystem`/`EnqueueBuild` (`ServerUniverse.h`) | `IntentType::Build` → `ApplyFleetCommand` → `ApplyIntentToUnit` ✅ |
| **Collect resources (mining)** | `ResourceNodeTag` + `HarvestStep`/`DepositAll` → `HarvestSystem`/`OrderHarvest` (node→mine→return→deposit auto-pilot) | `IntentType::Harvest` ✅ |
| **Attack ship** | Full M6 model: `DefenseLayers`/`ResistProfile`/`Fitting`/`Projectile`/`EwarStatus` → `CombatSystem`/`ProjectileSystem` | `IntentType::Attack`/`Guard`/`Orbit`/`KeepRange` ✅ |
| **Navigate** | warp + jump-beacon network, fuel + interdiction (`Navigation.h`) | `IntentType::Warp`/`Jump` ✅ |
| **PvE** | NPC AI (patrol/aggro/flee/defend), `SpawnNpcSite`, loot-on-kill, base disable-not-destroy | AI is server-driven ✅ |

Transport: the client sends a versioned `FleetCommand` (`MsgType::FleetCommand` = 45);
`ServerHost::OnDatagram` decodes it and calls `ServerUniverse::ApplyFleetCommand`, which
**ownership-validates every targeted unit** before applying (`§8.4` — never
client-authoritative).

Seeding **logic** also exists: `SpawnResourceNode`, `SpawnFieldNodes` (reads resource fields
from a cooked dataset and rings out typed nodes), `SpawnNpcSite`, `SpawnDemoSeed`,
`LoadUniverse`/`LoadUniverseFromCooked`. The authored dataset
`Config/universe/sol-frontier.universe` already declares regions, a beacon graph, and
resource fields.

## The gaps this plan closes

1. **🔴 `ERServer` boots an empty universe → mining/jump/PvE impossible on a real server.**
   `ERServer.cpp` constructs `ServerUniverse universe;` and steps it, but **never** calls
   `LoadUniverse*`, `SpawnFieldNodes`, `SpawnNpcSite`, or `SpawnDemoSeed`, and `ServerConfig`
   has **no universe-data field**. So a live shard has no resource nodes (nothing to mine —
   the seeding you flagged), no beacons (nothing to jump), and no NPC site. Players spawn with
   a base + 1 ship + 600 starting ore and then hit a wall. (`SpawnDemoSeed` runs only in tests.)
2. **🟠 Loot-claim has no command/wire path.** `ClaimLoot()` is implemented and tested, but
   there is no `IntentType` for it and `ServerHost` does not route it — kills drop loot nobody
   can recover.
3. **🟡 Resource-node divergence is not persisted.** `PersistState` (`WarmRestart.h`) captures
   bases/ships/builds/npcs but **not** resource nodes, even though a `ResourceNodes` table
   exists in `Config/db/schema.sql`. Depletion is lost on restart.
4. **⚪ Legacy `MoveCommand`** still drives base steering (documented deferred cleanup —
   `M3-core-4x-loop.md` area B). Not required here.
5. **⚪ Windows integration unverified** — `EarthRise` input→`FleetCommand`, `ERHeadless`
   Winsock bots, MSTest projects. Out of reach in the Linux environment; tracked, not closed
   here.

---

## Seeding architecture (the decided direction)

> **Authoritative universe state lives in the DB. Seeding is mostly procedural. Only the
> seed *parameters* live in `.universe`.**

This refines — does not contradict — the seed-procedural model already in
`universe-worldgen.md` §5 (baseline regenerated from a seed; only **divergences** persisted).
The split:

| Layer | Source of truth | In `.universe`? | In DB? |
| --- | --- | --- | --- |
| **Seed parameters** (region count/security mix, beacon-graph shape rules, field-density & composition weights, yield/spawn-rate ranges, RNG master-seed policy) | authored game data | ✅ **only this** | the chosen master-seed + epoch row |
| **Generated baseline** (concrete node placements, beacon coordinates, NPC-site placements) | `hash(masterSeed ⊕ regionId ⊕ epoch)` deterministic generator | ❌ (reproduced, never stored in full) | ❌ |
| **Divergence** (node depletion, cleared sites, captured beacons, base/ship/build/loot state) | the sim | ❌ | ✅ **authoritative** |

**Boot decision tree (per shard):**

```
  ERServer boot
    │
    ├─ DB has a Shard row (seed + epoch)?  ──yes──►  load seed → regenerate baseline
    │                                                   → RestoreState replays divergences
    │                                                   (warm-restart, M5 path)
    └─ no (fresh shard) ──► read .universe params → pick/derive master seed
                            → procedural generate baseline → persist Shard row (seed+epoch)
                            → first SimSnapshot
```

The generator must be **seed-reproducible and deterministic** (it rides the existing
deterministic-ECS guarantee, §7.2), so the same seed always rebuilds the same baseline — that
is what lets the DB store only the *seed* + *divergences* instead of every node.

### Epoch policy (resolves worldgen §5 open question)

> **The master seed is permanent for the shard's lifetime. "Epoch" is a localized,
> monotonic respawn counter — never a global reset.**

A persistent single-shard sandbox (the EVE half of the fantasy) must never wipe: territory,
the economy and player landmarks accrue value, so a global epoch bump (= regenerating the
whole baseline) is off the table in production. Concretely:

- **`masterSeed` is chosen once** at shard creation, written to the `Shard` row, and **never
  changes.** Bumping it would be a map wipe.
- **Static layout is epoch-0, fixed forever.** Regions, the beacon-graph *topology*, and
  station/landmark placement derive from `hash(masterSeed ⊕ regionId)` only — stable for the
  shard's life so player mental maps and the coordinates of claimed structures never shift.
- **Epoch is scoped to the resource field** (the smallest naturally-cycling unit) and is a
  **respawn cursor**: when a field mines out and its respawn timer elapses, increment *that
  field's* epoch and regenerate its nodes from `hash(masterSeed ⊕ fieldId ⊕ epoch)`. This is
  the *same* mechanism as "node respawn" — they are one feature.
- **Persistence stays tiny:** per field, store `{epoch, perNodeRemaining, respawnTimer}` — a
  counter plus depletion, never node positions (those are always reproducible). This is exactly
  the "majority procedural, only state in DB" split.
- **Never epoch-scope player-owned content.** Claimed beacons, built structures, territory are
  pure DB divergence, regenerated *never*. Epoch only touches unowned, naturally-cycling content
  (resource fields now; M7 anomalies/invasions later, where the epoch is **time-driven** off the
  Universe Clock §26 — same `hash(seed ⊕ id ⊕ epoch)` pattern, different trigger).
- **Live-ops knob, not a wipe switch:** a controlled single-region refresh (a season, a
  rebalance of a mined-flat region) is a surgical bump of *that region's* epoch — auditable,
  not a server restart.

**Decision — public seed at launch (with a private-salt escape hatch).** Use a public seed
everywhere for launch: clients can reconstruct the baseline (incl. respawns) with zero wire
cost, which is the §8.4 cold-start story, and predictable belts are a sandbox feature. If
mining-bot prediction later becomes a real problem, fold a **server-private salt into the
resource-field hash only** (`hash(masterSeed ⊕ privateSalt ⊕ fieldId ⊕ epoch)`) — never into
static layout, which must stay client-reproducible. The cost of a private salt is that those
nodes can no longer be pre-generated client-side and must come over the wire as divergence.

---

## Feature areas

### A. Server universe bootstrap + config (the unblocker)

- **Goal:** `ERServer` seeds a playable universe on boot instead of running empty.
- **Refs:** §20 (config), §15 (persistence boot), worldgen §7 ("Universe seeding"),
  `universe-worldgen.md` §5. **Touch:** `ERServer/ServerConfig.h`, `ERServer/ERServer.cpp`.
- **Current state:** `ServerUniverse universe;` is constructed and stepped but never seeded;
  `ServerConfig` has no universe field. Warm-restart restore (`RestoreFromWarmRestart`) already
  runs before the sim loop when a DB is configured.
- **Work:**
  - [ ] Add a `universe { dataset = "Config/universe/sol-frontier.universe"; masterSeed = …;
        epoch = … }` block to `ServerConfig` (§20; nothing from env).
  - [ ] In `ERServer.cpp` boot, **after** construct and **after** the warm-restart restore
        decision, branch: **restored from DB → do nothing** (state is authoritative);
        **fresh shard → seed.** Keep it on the single-threaded boot path (before the sim loop),
        matching how `RestoreFromWarmRestart` is sequenced.
  - [ ] Fresh-shard seed calls the area-B generator, not hand-placement; `SpawnDemoSeed()`
        becomes the **dev fallback** when no dataset is configured (so a no-DB smoke run is
        still mineable). Combat is already enabled by default (`m_combatEnabled{true}`).
  - [ ] Surface a one-line boot log: shard seed, epoch, node/beacon/site counts seeded (or
        "restored from DB").
- **Tests (`ERServerTest`, mirrored in `testrunner`):**
  - [ ] Boot-from-empty seeds **N>0** resource nodes, the beacon graph, and the NPC site(s)
        (count assertions off `ServerUniverse` accessors).
  - [ ] Boot-with-restore does **not** re-seed (no duplicate nodes/beacons).
  - [ ] No-dataset config falls back to `SpawnDemoSeed` and is mineable.
- **Depends on:** B (generator) for the real path; ships today against `SpawnDemoSeed`.
  **Blocks:** everything player-facing on a real server.

### B. Procedural generator (parameters → baseline)

- **Goal:** a deterministic, seed-reproducible generator that expands `.universe` **parameters**
  into the concrete baseline (regions → beacon coords → resource fields/nodes → NPC sites).
- **Refs:** `universe-worldgen.md` §4–§5, §8.4. **Touch:** new NeuronCore rule
  (`UniverseGen.h`, flat in `NeuronCore`) + `UniverseData.h` schema additions; generation lives
  in `ServerUniverse` (the seeding system).
- **Current state:** `sol-frontier.universe` currently **hand-authors** explicit fields with
  fixed coordinates/yields; `SpawnFieldNodes` rings nodes from those explicit fields. This area
  flips that: the file holds **distribution parameters**, the generator produces placements.
- **Work:**
  - [ ] Extend the `.universe` schema (`UniverseData.h` + `datacook`/`datacheck`) with a
        **parameter** form: per-region field-density and composition weights, yield/amount
        ranges, beacon-graph shape rules, NPC-site density — alongside (or replacing) explicit
        fields. Keep the cooked binary format stable for `LoadUniverseFromCooked`.
  - [ ] Add a deterministic PRNG keyed by `hash(masterSeed ⊕ regionId ⊕ epoch)` (no
        `Math.random`/wall-clock — must reproduce; mirrors the determinism harness discipline).
  - [ ] Generator emits the same entity set as today's `SpawnFieldNodes`/`SpawnNpcSite` so the
        downstream sim is unchanged; only *where the placements come from* changes.
  - [ ] `datacheck` gates the parameter ranges (densities sum sane, weights normalize, graph
        connectivity) — same spirit as the current beacon-graph checks.
- **Tests (`UniverseDataTests`/new `UniverseGenTests`, testrunner):**
  - [ ] Same seed → identical baseline (placement hash equal); different seed → different.
  - [ ] Generated node counts/composition fall within the authored parameter ranges.
  - [ ] Parameter round-trip cook/load; `datacheck` rejects out-of-range params.
- **Depends on:** nothing (pure rule). **Blocks:** A's real path, C's persistence shape.

### C. Persist the universe in the DB (state = authoritative)

- **Goal:** the shard's seed lives in the DB, and resource-node **divergence** (depletion) is
  captured in warm-restart so a restart reproduces the exact universe.
- **Refs:** §15, `M5-accounts-persistence.md`. **Touch:** `WarmRestart.h` (`PersistState`),
  `ServerUniverse::Capture/RestoreState`, `Config/db/` (new migration), `ERServer/persist/`.
- **Current state:** `PersistState` captures bases/ships/builds/npcs but **not** resource
  nodes; a `ResourceNodes` table already exists in `schema.sql` but is unused by capture. No
  `Shard`/seed row exists.
- **Work:**
  - [ ] Add a `Shard` (or `Universe`) row: `masterSeed`, `createdAt` — written **once** on fresh
        seed, read on boot to pick restore-vs-generate (area A's decision tree). `masterSeed`
        never changes (epoch policy).
  - [ ] Persist **per-field state**, not node positions: `{ fieldId, epoch, respawnTimer }` plus
        per-node `remaining` (depletion divergence keyed by the generated netId). Node placements
        are reproduced from `hash(masterSeed ⊕ fieldId ⊕ epoch)`, so the DB stores only the
        counter + depletion — the "majority procedural" split. Add to `PersistState`; capture in
        `CaptureState`, restore in `RestoreState`, include in `StateHash`.
  - [ ] **Respawn system** (NeuronCore rule + `ServerUniverse` system): when a field is depleted,
        run `respawnTimer`; on elapse, `++epoch` and regenerate that field's nodes (area B
        generator). Deterministic, so client and server agree without shipping placements.
  - [ ] Forward-only migration under `Config/db/migrations/` (project rule: ordered, additive).
- **Tests (`WarmRestartCaptureTests`/`WarmRestartTests`, testrunner):**
  - [ ] Capture→encode→decode→restore round-trips per-field `{epoch, respawnTimer, remaining}`;
        `StateHash` stable.
  - [ ] Depleted node stays depleted across a capture/restore cycle.
  - [ ] Respawn: a depleted field at epoch N, after its timer, regenerates at epoch N+1 to the
        same placements on a re-run (seed-reproducible); a different epoch → different placements.
  - [ ] `masterSeed` is read once and never rewritten on a warm restart (no re-seed).
- **Depends on:** A, B. **Blocks:** zero-loss restart for the economy.

### D. Close the loot-claim loop

- **Goal:** players can recover loot dropped on kills, end-to-end.
- **Refs:** §13.2 (loot-on-kill). **Touch:** `Command.h`, `ServerUniverse.h`
  (`ApplyIntentToUnit`), `ServerHost.h` (route), `NeuronClient/FleetControl.h` (smart-action).
- **Current state:** `ClaimLoot()` exists + tested; no `IntentType`, no route, no client verb.
- **Work:**
  - [ ] Add `IntentType::ClaimLoot` (next stable wire value) to `Command.h`; route it in
        `ApplyIntentToUnit` → `ClaimLoot(unit, cmd.targetNetId)` with the same ownership check.
  - [ ] `FleetControl.h` `ClassifyTarget` → loot container ⇒ smart-action "claim"
        (`MakeSmartCommand` fills the target).
  - [ ] No `ServerHost` change beyond the existing `FleetCommand` decode (it already routes all
        intents through `ApplyFleetCommand`).
- **Tests (`Fleet*`/`ClientFleet*`, testrunner):**
  - [ ] `ClaimLoot` intent round-trips and transfers items to the claimer's cargo; rejected for
        an unowned claimer / dead container.
  - [ ] Smart-action over a loot container resolves to the claim intent.
- **Depends on:** nothing (sim method exists). **Blocks:** the kill→loot→cargo loop being whole.

### E. Windows integration verification (out-of-environment, tracked)

- **Goal:** confirm the wired paths on the real client/server stack.
- **Touch:** `EarthRise/App.cpp` (input→`FleetCommand`), `ERHeadless/ERHeadless.cpp` (bot
  loop), the MSTest projects.
- **Work (cannot run on Linux here):**
  - [ ] Windows build of the solution; `ERServer` + `ERHeadless` smoke: connect, seed visible,
        harvest→build→attack→claim drive through real Winsock.
  - [ ] `EarthRise` client smoke: radar-command issues each intent; overview shows seeded nodes.
- **Depends on:** A–D. **Blocks:** the playable-slice sign-off.

---

## Suggested order / dependency notes

1. **D (loot loop)** is the cheapest, fully Linux-testable, and self-contained — a good warm-up
   that closes a visible gap.
2. **A (bootstrap)** against `SpawnDemoSeed` first — instantly makes a real server mineable —
   then swap in **B (generator)** for the parameter-driven path.
3. **B (generator)** and **C (persistence)** together realize the decided architecture (params
   in `.universe`, baseline from seed, state in DB). C depends on B's stable generated netIds.
4. **E (Windows)** last and elsewhere — it verifies, it doesn't build new logic.

## Done gate

- [ ] A fresh `ERServer` boot seeds a mineable, jumpable, fightable universe (resource nodes +
      beacons + NPC site present), logged at boot.
- [ ] The `.universe` file holds **parameters**; the baseline is **procedurally generated** from
      a permanent master seed; the **seed + divergences are persisted in the DB** and a restart
      reproduces the universe (per-field epoch + depletion preserved).
- [ ] Mined-out fields **respawn** via a per-field epoch bump (deterministic, no node positions
      stored or shipped); player-owned content is never epoch-regenerated.
- [ ] Loot dropped on a kill can be claimed end-to-end via a validated intent.
- [ ] All matching `<project>Test` suites green on the Linux `testrunner` (§16.1).
- [ ] (Out-of-env, tracked) Windows `ERServer`+`ERHeadless`+`EarthRise` smoke confirms the path.
