# EarthRise — Universe Content & Generation

> Companion to `masterplan.md` **§6** (Universe Model), **§8.4** (seed-procedural
> replication), **§12.6** (game-data tooling), **§13.5–13.6** (Universe structure /
> territory), **§13.7** (PvE), **§13.11** (entities), **§13.12** (navigation), **§15**
> (persistence). **Status:** DRAFT v0.1.
>
> **Why this doc exists.** "Where do the stars, asteroids and jumpgates live?" has, until
> now, been answered in pieces across the masterplan. This consolidates the
> **universe-management** design into one subsystem view: what the universe is made of,
> how its layout is **authored as game data**, how its dynamic content is **generated from
> a seed**, and which milestone builds which slice. It re-decides nothing — it sequences
> and schematizes what the masterplan already locks.

---

## 1. The two halves (and what this doc is *not*)

Universe content splits cleanly along the §8.4 replication boundary:

| Half | What | How it's defined | Replicated as | Milestone |
| --- | --- | --- | --- | --- |
| **Static layout** | regions + security tiers, the **jump-beacon graph**, fixed stations/landmarks, named resource fields | **authored game data** → `datacook` → NeuronCore (§12.6) | build data (ships with client+server; never on the wire) | **M3** (slice), M7 (full) |
| **Dynamic content** | **resource nodes**, **anomalies/expeditions**, **invasions**, loot | **deterministic generation from a seed** (§8.4) + server spawn systems | the **divergences** only (depletion, kills, claims) as interest-deltas | **M3** (hand-placed slice), M7 (procedural) |

This is **not** a balance doc (yields, jump ranges, spawn rates are tunables — they live in
the cooked datasets and are validated with bots, per §12.6/§19) and **not** a netcode doc
(§8.4 owns replication). It defines **content structure, data schemas, and generation
rules**.

> **The substrate is already built.** §6 (Universe Model) — `UniversePos` (`int64` metres,
> ±975 ly), `SectorId`, the interest grid, floating origin — is the coordinate space the
> universe lives in. It exists in `NeuronCore/UniversePos.h` + `ServerUniverse`. Everything
> below places content *into* that space.

---

## 2. Content layers

The universe is a stack of layers over the coordinate substrate. Lower = more permanent /
authored; higher = more dynamic / generated.

```
  ┌──────────────────────────────────────────────────────────────┐
  │ 6  SCENERY / BACKDROP   stars (skybox), decoration props      │  visual only
  ├──────────────────────────────────────────────────────────────┤
  │ 5  DYNAMIC PvE          anomalies/expeditions, invasions      │  seed + events (M7)
  ├──────────────────────────────────────────────────────────────┤
  │ 4  RESOURCE FIELD       resource nodes (+ asteroid dressing)  │  seed/data spawn (M3 slice)
  ├──────────────────────────────────────────────────────────────┤
  │ 3  STRUCTURES           jump beacons, stations, territory     │  authored data (M3 public/M7 claim)
  ├──────────────────────────────────────────────────────────────┤
  │ 2  REGIONS / SECURITY   high → low → null, yield gradient     │  authored data (§13.5)
  ├──────────────────────────────────────────────────────────────┤
  │ 1  COORDINATE SUBSTRATE UniversePos · sectors · interest grid │  ENGINE — built (§6)
  └──────────────────────────────────────────────────────────────┘
```

A **"system"** in EarthRise is not a simulated star with orbits — it's a **named cluster of
sectors** inside a region, anchored by landmark scenery and (usually) a jump beacon. There
is no celestial-mechanics sim; the universe is sectors + placed content.

---

## 3. Entity inventory (which §13.11 entities are universe content)

| Entity | `EntityKind` | Layer | Origin | Persisted (§15) | Milestone |
| --- | --- | --- | --- | --- |
| **JumpBeacon** | `Structure` | 3 | authored (graph) | M5 (player beacons only) | M3 public · M7 claimable |
| **TerritoryStructure** | `Structure` | 3 | authored / player-built | **M5** (SQL) | M7 |
| Station / landmark | `Station` | 3 | authored | — | M3 (scenery) |
| **ResourceNode** | `ResourceNode` | 4 | seed/data spawn | M5 (depletion state) | **M3** |
| Asteroid (dressing) | `Asteroid` | 4/6 | scenery prop | — | M2 (exists) |
| **AnomalySite** | `NpcUnit`+`Structure` | 5 | seed/template | M5 (cleared state) | M7 (M3 = 1 hand-placed) |
| **InvasionEvent** | `NpcUnit` swarm | 5 | event director | — (event log) | M7 |
| **NpcUnit** (guardians) | `NpcUnit` | 4/5 | spawned by site/event | — | M3 (basic) |
| Stars / debris / decoration | skybox · `Debris` · `Decoration` | 6 | scenery / skybox | — | M2 (exists) |

> **Today:** `ServerUniverse::SpawnScenery()` hand-places ~12 catalog props (a jumpgate, 3
> asteroids, stations, debris, hulls) near spawn to exercise the M2 `ShapeCatalog` render
> path. That is **placeholder dressing**, not the layer-3/4 systems below — those are net-new
> from M3.

---

## 4. Authoring pipeline — the cooked datasets (§12.6)

Static layout is authored as **text source** and cooked by **`datacook`** to the versioned
binary serde loaded by NeuronCore (one dataset → server sim + client display + bots, no
drift). **`datacheck`** runs referential integrity in CI. *(Both tool executables are an M2
carry-over — they get stood up in **M3 area D**, which needs the beacon-graph cook.)*

Four dataset families (schemas are sketches, not final syntax):

**4.1 Region / security layout** — partitions the universe into tiered regions (§13.5).
```
region MERIDIAN_REACH {
  security   = high                       # high | low | null
  bounds     = sectors(-8..8, -8..8, -2..2)   # inclusive SectorId ranges
  yield_mult = 0.6                        # risk→reward: high-sec yields less
}
```

**4.2 Jump-beacon graph** — the navigation network (§13.12). Nodes + bidirectional jump edges.
```
beacon MER_GATE_1 {
  region = MERIDIAN_REACH
  pos    = universe(131072, 0, 0)         # UniversePos (int64 metres)
  links  = [ MER_GATE_2, BORDER_GATE_A ]  # jump edges (must be reciprocal)
  kind   = public                         # public (NPC, M3) | claimable (M7)
}
```
*`datacheck`:* every `region`/`link` target resolves; links are reciprocal; the graph's
public-beacon subgraph is connected (report islands); `claimable` only in low/null (§13.6).

**4.3 Resource fields** — where harvestable nodes spawn (composition + density, not positions).
```
field MER_BELT_1 {
  region = MERIDIAN_REACH ; center = universe(...) ; radius = 1500m
  nodes  = { Ore: 0.6, Ice: 0.3, Gas: 0.1 }   # type weights
  count  = 8..14 ; yield = 5000..9000 ; respawn = 600s
}
```

**4.4 Anomaly / invasion definitions** *(M7)* — site & event templates (difficulty tier,
guardian composition, loot table, spawn weight by security, escalation script). The
event-director (§26 Universe Clock) schedules these against region exploitation (§13.7).

---

## 5. The seed-procedural model (§8.4)

Dynamic content (layer 4–5) is **not** shipped as data or streamed wholesale — it is
**reconstructed locally from a shared seed by the same deterministic generator** the server
runs; the server transmits only **divergences** from that baseline (depleted nodes, killed
guardians, claimed structures) through the ordinary interest-delta loop.

```
  authored data (regions, beacons, fields)  ─┐
                                             ├─►  deterministic generator  ─►  baseline content
  universeSeed ⊕ regionId ⊕ epoch ──────────┘        (server == client)        (nodes/anomalies)
                                                              │
                          server applies player edits ────────┘──►  divergences ──► interest-deltas
```

- **Authored** (regions, beacon graph, field *definitions*) = build data, identical both ends.
- **Generated** (concrete node placements within a field, anomaly instances) = `hash(seed,
  regionId, epoch)` → same result both ends. The only hard requirement is **seed-reproducible
  determinism**, already guaranteed by the deterministic ECS sim (§7.2).
- **Divergence** (a depleted node, a cleared site, a captured beacon) = the *only* universe
  state that crosses the wire, relevance-prioritised from the empty baseline (§8.4).

This is what lets "hundreds of players, one contiguous universe" cold-start without ever
transferring the universe as a bulk artifact.

---

## 6. Stars, asteroids, jumpgates — the concrete answer

- **Stars** — **not entities.** No star-system simulation exists or is planned. Stars are the
  **skybox backdrop** (`Assets/Textures/starbox_*.dds`) plus optional `Decoration`/`Structure`
  scenery props from the `ShapeCatalog` (layer 6). A bright "local star" is set dressing /
  the lighting key (§11.2 world-fixed sun), not a body you fly to.
- **Asteroids** — **two distinct things:** (a) `EntityKind::Asteroid` **scenery** props
  (decorative, ShapeCatalog, exists since M2); (b) harvestable **`ResourceNode`s** (gameplay,
  layer 4, M3). A "belt" is a **resource field** (§4.3) of ResourceNodes, optionally dressed
  with Asteroid scenery for looks. Mining targets the node, not the mesh.
- **Jumpgates** — **`JumpBeacon` entities** = nodes in the **beacon graph** (§4.2, §13.12),
  the long-haul travel network. Rendered via the `Structure`/"Jumpgate" catalog mesh. **Public**
  NPC beacons in high-sec (M3); **claimable `TerritoryStructure`** beacons in low/null own the
  network as a conquest objective (M7, §13.6).

---

## 7. Server systems & where they live

| System | Module | Role | Milestone |
| --- | --- | --- | --- |
| Cooked-data load | NeuronCore (from `datacook`) | regions, beacon graph, field defs | M3 |
| Universe seeding | `ServerUniverse` (replaces `SpawnScenery`) | place authored structures + seed fields at startup | M3 |
| Resource spawn/respawn | sim system (NeuronCore rules) | maintain field node population, depletion | M3 |
| Sensor / fog | sim system (per-player detected set) | what each player can see (§13.7, M3 area E) | M3 |
| NPC AI | `ERServer/ai/` | site guardians, patrol/aggro/flee (M3); invasions/escalation (M7) | M3 / M7 |
| Event director | server + Universe Clock (§26) | schedule anomalies/invasions by region heat | M7 |

---

## 8. Milestone mapping

- **M3 (now):** the **static-data slice** + the first dynamic loop. Stand up `datacook`/
  `datacheck`; author a **small region set + a handful of public beacons** (graph integrity in
  CI); spawn **resource fields** (the harvest loop, area C); one **hand-placed NPC site** (area
  F). Non-procedural, non-claimable. *This doc's §4.1–4.3 schemas are area D's cook target.*
- **M4:** sector-subscription interest + warp/jump prefetch — universe content becomes
  interest-scoped instead of full-snapshot.
- **M5:** persistence — ResourceNode depletion, cleared sites, and player/territory beacons
  snapshot to SQL (`TerritoryStructures`, §15). Until then universe state is in-memory.
- **M7:** the **procedural & territorial** half — anomaly/invasion generation (§4.4, §5),
  claimable beacons/structures (§13.6), tiered-security enforcement across the full region map.

---

## 9. Open questions (track)

- **Region layout: fully authored vs seed-generated?** §4.1 assumes authored regions with
  seed-generated *contents*. A fully procedural region map is possible (§8.4 supports it) but
  authored regions give hand-tuned onboarding (dense high-sec beacons, §13.12). *Recommend
  authored regions + seeded contents for launch.*
- **Do asteroid scenery and `ResourceNode`s share placement?** I.e. is every visual asteroid
  mineable, or is dressing separate from nodes? (Separate is cheaper and lets art and balance
  move independently — recommend separate, co-located by the field def.)
- **Beacon graph: hand-authored vs generated topology?** M3 hand-authors a small graph; at
  scale, a generated-then-tuned topology may be needed. Keep the cooked format identical so the
  source can switch.
- **Epoch granularity for seed regeneration** (§5) — how often does generated content "reset"
  (respawn anomalies)? Ties to the Universe Clock (§26) and live-ops.
- **`SpawnScenery` retirement** — fold today's placeholder props into authored
  station/decoration data once §4 lands, or keep a dev-only scenery path?

> See also: `economy-crafting.md` (what resource nodes feed), `tech-tree.md` (anomalies as the
> datacore faucet), and `masterplan.md` §13.12 (the navigation layer this network drives).
