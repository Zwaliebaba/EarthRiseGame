# EarthRise — PvE Enemy-Ship AI (design)

> **Status:** design draft for review — **no code yet**. Companion to `masterplan.md`
> §13.7 (PvE) and §13.2 (combat). This is the first *designed* AI behaviour; it
> **replaces** the M6 placeholder static-guardian behaviour (`AiSystem`/`NextAiState`)
> with a configurable **roaming-kiter** that patrols a boundary, engages players that
> wander in, and holds them at range while circling. Per the project decision, the
> behaviour's tuning is **stored in SQL** and loaded at server start — an intentional,
> documented exception to the §15 "balance lives in cooked game-data, not SQL" boundary
> (see §9 *Configuration storage*).

| Masterplan refs | §13.7 (PvE), §13.2 (combat), §15 (catalog/balance boundary), §26 (live-ops/hot-reload), §7.2 (deterministic sim), §9 (server architecture), §12.6 (game-data pipeline) |
| --- | --- |

---

## 1. Goal & scope

**The behaviour we are building** (the player-facing description):

> An enemy ship slowly drifts through space **within a boundary**, doing nothing. When
> it gets **close to one of the player's ships**, it automatically **engages and shoots**.
> While engaged it **keeps the player ship at range and slowly circles** it.

Decomposed into a state machine: **Patrol** (wander a boundary) → **Engage/Kite** (hold
at standoff, circle, fire) → optionally **Flee** (break off at low hull) → **Return**
(leash back to the patrol anchor when the player escapes).

### In scope
- One NPC behaviour (the roaming kiter), **replacing** the M6 guardian behaviour.
- **Boundary-bounded patrol** with slow deterministic wander.
- **Aggro** on proximity to any player-owned ship (re-uses the existing target scan).
- **Keep-range + true tangential circling** — new steering; today's orbit is radial-only.
- **Fire while kiting** — decouple "shoot the target" from the `Attack` close-in order.
- **All tuning in a SQL table**, loaded into memory at startup, hot-reloadable (§26),
  with the determinism/replay safeguards that moving data out of the build implies.

### Non-goals (explicitly deferred)
- **Factions, invasions, anomalies, escalation, bounties** — that is the M7 PvE content
  layer (`docs/implementation/M7-sandbox-conquest.md`); this design is the *substrate* it
  will later schedule, not the content director (§26).
- **Multiple branching behaviour profiles** — the project chose *one* behaviour. The SQL
  table parameterises that one behaviour; it does **not** select between Sentry/Roamer/…
  logic. (Adding profiles later is a `BehaviourKind` column + a switch — noted in §13.)
- **Pathfinding / obstacle avoidance / formations / group assist** — straight-line
  steering only, matching the rest of the sim (`Movement.h`, `Fleet.h`).
- **Client-side AI** — AI is server-authoritative; clients only see the resulting motion,
  which already replicates (§8). No snapshot wire-format change for clients (see §10).

---

## 2. Current state — what exists, what's missing

The M6 milestone shipped a working PvE skeleton. We **extend the data path and steering**,
not start from scratch.

| Piece | Where | Today | Needed |
| --- | --- | --- | --- |
| `NpcAi` component | `Components.h` | `state`, `home`, `aggroRange`, `fleeHpFrac`, `targetNetId`, `siteId` | + patrol boundary, config ref, orbit direction, de-aggro/leash |
| `AiSystem()` | `ServerUniverse.h` (~L1868) | runs first each tick; scans player ships; writes a `FleetOrder` | drive the new state machine; read tuning from config, not constants |
| `NextAiState()` | `Fleet.h` (L76) | returns **Flee / Aggro / Defend** — **`Patrol` is dead code, never returned** | reachable Patrol; de-aggro/return transitions |
| Patrol behaviour | `AiSystem` | `Move` to `home`, range 0 → **sits still**; no wander, no boundary | slow wander inside an `anchor + radius` volume |
| Orbit / KeepRange | `Fleet.h::StepStandoff` (L28) | **radial only** — `StepToward` straight at the ring; no circling | tangential **circle** + radial correction (new `StepOrbit`) |
| Aggro action | `AiSystem` (~L1897) | issues `OrderType::Attack` → **closes to weapon range to brawl** | issue **keep-range/orbit** so it holds the player at range |
| Firing gate | `CombatSystem`/`FireWeapons` (~L1626) | fires **only** when `order.type == Attack` | fire at a hostile target while **orbiting/keep-ranging** too |
| Tuning | `ServerUniverse.h` (~L1128) | `NPC_AGGRO_RANGE=6000`, `NPC_FLEE_HP_FRAC=0.15`, `NPC_FIT` — **hard-coded `constexpr`** | **SQL config table** → in-memory catalog |
| Warm-restart | `WarmRestart.h::PersistNpc` (L61) | persists `netId,x,y,z,hp,siteId,aiState` — **no patrol anchor, no config ref** | + anchor + config code so restored NPCs resume identically |

---

## 3. Behaviour model — the state machine

States and the order each one issues into the **shared** `FleetOrder` machine (the same
one player ships use — `OrderType` in `Components.h`):

```
                 player ship enters aggroRange of NPC
   ┌──────────┐  ───────────────────────────────────▶  ┌──────────────────┐
   │  PATROL  │                                         │  ENGAGE (kite)   │
   │ wander a │  ◀───────────────────────────────────  │ hold @ standoff, │
   │ boundary │   target gone (dist > leashRange) OR     │ circle, fire     │
   └──────────┘   no target in deaggroRange (hysteresis) └────────┬─────────┘
        ▲                                                          │ hull ≤ fleeHpFrac
        │ reached anchor / hull recovered                          ▼
        │                                                 ┌──────────────────┐
        └──────────────────  leash home  ───────────────  │  FLEE (optional) │
                                                          │ run from target  │
                                                          └──────────────────┘
```

| State | Condition (priority order) | Order issued | Movement | Weapons |
| --- | --- | --- | --- | --- |
| **Flee** | `hullFrac ≤ cfg.fleeHpFrac` (and `fleeHpFrac > 0`) | `Move` away from target toward `anchor` | flee speed (= patrol or max) | hold fire |
| **Engage** | a player ship is within `aggroRange` **and** within `leashRange` of `anchor` | `Orbit(target, range = standoffRange)` | **`StepOrbit`** (circle) | fire at `targetNetId` in range |
| **Return** | was engaged, target now beyond `deaggroRange` or `leashRange` | `Move` to `anchor` | patrol speed | hold fire |
| **Patrol** | default | `Move` to current wander waypoint | patrol speed | hold fire |

`NextAiState()` is rewritten to return all four reachable states (Patrol becomes live).
Two hysteresis ranges prevent flapping at the edge of aggro:

- `aggroRange` — distance at which a *patrolling* NPC starts engaging.
- `deaggroRange ( > aggroRange )` — distance at which an *engaged* NPC gives up.
- `leashRange` — max distance from `anchor` the NPC will pursue; past it, disengage and
  return. This is what makes the "boundary" hold during combat, not just during patrol.

All transitions are evaluated once per tick in `AiSystem`, before steering/combat, so the
existing deterministic system ordering (AI → steering → combat → movement, §7.2) is kept.

---

## 4. Steering — the new math

Both new motions are **pure, deterministic functions** (no globals, no wall-clock, no
`rand`) added next to `StepStandoff` in `Fleet.h` (or a new `AiSteering.h` if `Fleet.h`
grows too large). This keeps them on the Linux `testrunner` and inside the record/replay
determinism guarantee (§7.2, §16.2). Motion is **kinematic** — they step the `Transform`
directly, exactly like the existing `StepStandoff`, so they slot into `FleetOrderSystem`
with no change to the Velocity/`IntegrateMovement` path.

The sim is played on the horizontal **x–z plane** (the `SpawnNpcSite` ring already varies
x/z at constant y); circling and wander operate in that plane and keep `y` level. This is
a deliberate simplification — full 3-D orbit is a future option (§13).

### 4.1 Patrol wander inside a boundary (sphere = `anchor` + `patrolRadius`)

The NPC walks slowly between **deterministically chosen** waypoints inside the sphere:

```
// pure, deterministic — no rand(); the "randomness" is a hash of identity + waypoint #
UniversePos PatrolWaypoint(uint32_t netId, uint32_t waypointSeq,
                           UniversePos anchor, double patrolRadius)
{
    const uint64_t h = SplitMix64(netId * 0x9E3779B97F4A7C15ull ^ waypointSeq);
    const double ang = (h & 0xFFFF) / 65535.0 * TWO_PI;          // bearing in x–z
    const double rad = ((h >> 16) & 0xFFFF) / 65535.0;           // 0..1
    const double r   = patrolRadius * std::sqrt(rad);            // area-uniform
    return { anchor.x + llround(cos(ang) * r), anchor.y,
             anchor.z + llround(sin(ang) * r) };
}
```

Per tick while **Patrol**:
1. If within `arriveEpsilon` of the current waypoint (or it has been `waypointSeq`-stale
   for `cfg.repathSeconds`), advance `npc.waypointSeq++` and recompute the waypoint.
2. `StepStandoff(tr, waypoint, patrolSpeed·dt, arriveEpsilon)` — re-uses the existing
   straight-line stepper at **`cfg.patrolSpeed`** (slow).
3. **Leash guard:** if `dist(tr, anchor) > patrolRadius` (e.g. after a flee chase), the
   waypoint is forced to `anchor` until back inside — the NPC can never wander out.

Determinism: `waypointSeq` is per-NPC runtime state in `NpcAi`, advanced only by the sim;
the waypoint is a pure function of `(netId, waypointSeq, anchor, radius)`, so a replay or a
warm-restart reproduces the identical patrol path (`netId` + `waypointSeq` are persisted —
§10).

### 4.2 Engage — keep range **and circle** (the real fix)

Today `OrderType::Orbit` and `KeepRange` both call `StepStandoff`, which only closes the
radial gap to the ring — the NPC reaches the standoff distance and **stops**. To *circle*,
we add a tangential component. New pure function:

```
// Hold radius R around 'target' while sweeping around it. Direction 'dir' ∈ {+1,-1}.
// vMax = cfg.orbitSpeed * dt (the "slowly circles" speed). kRadial ∈ (0,1] is a soft
// pull back onto the ring (1 = snap, ~0.25 = gentle spiral-in). All in the x–z plane.
bool StepOrbit(Transform& tr, UniversePos target, double R,
               double vMax, int dir, float kRadial)
{
    double rx = (tr.pos.x - target.x), rz = (tr.pos.z - target.z);   // target → unit
    double r  = std::hypot(rx, rz);
    if (r < 1e-3) { rx = 1.0; rz = 0.0; r = 1.0; }                   // degenerate: pick +x
    const double ux = rx / r,           uz = rz / r;                 // radial unit
    const double tx = -dir * uz,        tz =  dir * ux;              // tangential unit
    const double radialErr = R - r;                                  // + => push out
    // desired displacement = sweep along tangent + correct toward the ring
    double dx = tx * vMax + ux * kRadial * radialErr;
    double dz = tz * vMax + uz * kRadial * radialErr;
    const double mag = std::hypot(dx, dz);                           // clamp to vMax
    if (mag > vMax) { dx = dx / mag * vMax; dz = dz / mag * vMax; }
    StepBy(tr, dx, 0.0, dz);                                         // y stays level
    return std::abs(radialErr) <= R * 0.05;                          // "on the ring" band
}
```

Behaviour: from far away the radial term dominates → the NPC spirals **in** to `R`; once on
the ring the tangential term dominates → it **circles** at `orbitSpeed`. `dir` is chosen
once on entering Engage (e.g. `dir = (netId & 1) ? +1 : -1`) and stored in `NpcAi` so the
circle direction is stable and deterministic. `standoffRange` (R) should be set to roughly
the NPC's **weapon optimal** so it sits at the edge of its effective range and keeps firing
while orbiting (see §4.3). A ±5 % hysteresis band stops radial jitter.

This new `StepOrbit` is wired in for **NPC** Engage. We can also upgrade player
`OrderType::Orbit` to use it (a strict improvement — players today get the same radial-only
hold), but that is optional and called out separately so it doesn't surprise existing
player-command behaviour.

### 4.3 Fire while kiting (combat-gate change)

`CombatSystem` currently fires **only** when `fo.current.type == OrderType::Attack`
(`ServerUniverse.h` ~L1626). If Engage issues `Orbit` (so the NPC holds range instead of
closing), the NPC would circle **but never shoot**. Fix: a unit fires at the **entity
target of its current order** whenever that order is a *combat* order and the target is
hostile and in range — i.e. broaden the gate from `Attack` to
`{ Attack, Orbit, KeepRange, Guard }`-with-an-entity-target. Concretely:

```
const bool wantsToFire =
    fo.current.targetNetId != 0 &&
    (fo.current.type == OrderType::Attack    ||
     fo.current.type == OrderType::Orbit     ||
     fo.current.type == OrderType::KeepRange  );
if (!wantsToFire) { TickWeaponCooldowns(e, dt); return; }
FireWeapons(e, …, fo.current.targetNetId, …);   // unchanged downstream
```

`FireWeapons` already range-gates per weapon (`InEngagementRange`, optimal+falloff), so a
target outside reach simply yields no shots — no extra check needed. This change is
**target-driven, not NPC-specific**, so player ships told to `Orbit`/`KeepRange` an enemy
will also fire while orbiting, which is the intuitive RTS behaviour and matches the
right-click context menu's *orbit / keep-range* combat actions (README controls table).

---

## 5. Components & per-NPC runtime state

`NpcAi` holds **runtime state** (mutable, per-entity, snapshot-able). **Tuning** lives in
the config catalog (§6) and is referenced by code, never copied field-by-field into every
NPC. Proposed `NpcAi` (extends today's struct):

```
struct NpcAi
{
    AiState        state{ AiState::Patrol };
    UniversePos    anchor{};            // renamed from 'home' — patrol/leash centre
    uint32_t       targetNetId{ 0 };    // current hostile (0 = none)
    // --- patrol runtime ---
    uint32_t       waypointSeq{ 0 };    // advances per waypoint; seeds PatrolWaypoint()
    // --- engage runtime ---
    int8_t         orbitDir{ +1 };      // +1/-1, chosen on first Engage; stable circle
    // --- binding ---
    uint16_t       configId{ 0 };       // → NpcBehaviorConfig row (replaces siteId role)
    uint16_t       siteId{ 0 };         // kept for "site cleared" bookkeeping (M6/M7)
};
```

Removed from the **hot-coded constants** block in `ServerUniverse.h`: `NPC_AGGRO_RANGE`,
`NPC_FLEE_HP_FRAC`, and the `NPC_FIT` default — all now come from the config row. (Keep a
single compiled-in `DefaultNpcConfig()` as the fallback if the table is empty, mirroring
`DefaultCombatCatalog()`.)

The immutable per-type tuning, as an in-memory POD in **NeuronCore** (pure, ODBC-free):

```
struct NpcBehaviorConfig                       // one row of the SQL table, resolved
{
    uint16_t    configId{ 0 };
    // sensing / aggro
    float       aggroRange{ 6000.f };          // start engaging within this of the NPC
    float       deaggroRange{ 9000.f };        // give up beyond this (hysteresis > aggro)
    float       leashRange{ 12000.f };         // max pursuit distance from anchor
    // patrol
    float       patrolRadius{ 8000.f };        // boundary sphere radius around anchor
    float       patrolSpeed{ 400.f };          // m/s — "slowly flying"
    float       repathSeconds{ 6.f };          // pick a new waypoint at least this often
    // engage / kite
    float       standoffRange{ 4000.f };       // ring radius held while circling (~optimal)
    float       orbitSpeed{ 600.f };           // m/s tangential — "slowly circles"
    int8_t      orbitKRadialPct{ 25 };         // radial pull toward the ring, 0..100
    // survival
    float       fleeHpFrac{ 0.15f };           // flee below this hull fraction (0 = never)
    // identity
    char        fitCode[24]{ "fighter-kin" };  // catalog fit installed on spawn
};
```

`DefaultNpcConfig()` returns these defaults so the sim is fully functional with an empty
table and the testrunner needs no SQL.

---

## 6. Configuration storage (SQL) — the core requirement

Per the project decision, tuning lives in **SQL Server**, loaded into the in-memory
catalog at startup and hot-reloadable. NeuronCore stays ODBC-free; the ODBC read lives in
`ERServer/persist/` like every other store.

### 6.1 Table

```sql
CREATE TABLE NpcBehaviorConfigs (
    ConfigId        SMALLINT     NOT NULL IDENTITY(1,1) PRIMARY KEY,
    Code            NVARCHAR(48) NOT NULL,            -- e.g. 'raider.light'
    -- sensing / aggro (metres)
    AggroRange      INT          NOT NULL DEFAULT 6000,
    DeaggroRange    INT          NOT NULL DEFAULT 9000,
    LeashRange      INT          NOT NULL DEFAULT 12000,
    -- patrol
    PatrolRadius    INT          NOT NULL DEFAULT 8000,
    PatrolSpeed     FLOAT        NOT NULL DEFAULT 400,   -- m/s
    RepathSeconds   FLOAT        NOT NULL DEFAULT 6,
    -- engage / kite
    StandoffRange   INT          NOT NULL DEFAULT 4000,
    OrbitSpeed      FLOAT        NOT NULL DEFAULT 600,   -- m/s
    OrbitKRadialPct TINYINT      NOT NULL DEFAULT 25,    -- 0..100
    -- survival
    FleeHullPct     FLOAT        NOT NULL DEFAULT 0.15,  -- 0..1, 0 = never flee
    -- identity
    FitCode         NVARCHAR(24) NOT NULL DEFAULT 'fighter-kin',
    UpdatedAt       DATETIME2    NOT NULL DEFAULT SYSUTCDATETIME(),
    CONSTRAINT UQ_NpcBehaviorConfigs_Code UNIQUE (Code)
);
```

The table can hold **multiple rows** (e.g. a weaker frontier raider and a tougher deep-space
one) — that is just *parameter* variety, **not** a behaviour branch: every row runs the
identical §3 state machine. A spawn picks a row by `Code`; the NPC stores its `ConfigId`.
Money/quantities elsewhere are BIGINT integer credits (§ schema header) — here all values
are tuning floats/ints in metres and m/s, consistent with `NavTuning`/`EconomyTuning`.

### 6.2 Migration & seed (forward-only, idempotent, Azure-SQL-compatible)

A new `Config/db/migrations/006_pve_ai_config.sql`, matching the house style of `004`/`005`
(guarded by `sys.tables`/`sys.columns`, dynamic SQL where a column may not yet exist). It
`CREATE`s `NpcBehaviorConfigs` and seeds one default row (`'raider.light'`) so a fresh DB
spawns working enemies. `schema.sql` (the canonical document) gains the same table plus a
boundary-amendment note (§9.5).

### 6.3 Loading path (mirrors the combat catalog)

```
ERServer startup (before accepting connections, alongside the universe/combat load):
  ┌ persist thread up ────────────────────────────────────────────────┐
  │ rows = NpcConfigStore.LoadAll()        // ERServer/persist (ODBC)   │
  │ catalog = BuildNpcCatalog(rows)        // NeuronCore POD, validated │
  │ universe.LoadAiConfig(std::move(catalog))   // setter, like         │
  │                                              // LoadCombat() (L214) │
  └───────────────────────────────────────────────────────────────────┘
Per tick (hot loop): AiSystem reads universe.AiConfig(npc.configId) from memory.
                     NEVER touches the DB (§9 hot-path isolation).
Hot-reload (§26 live-ops): an operator edits rows; an admin command re-runs LoadAll →
                     LoadAiConfig. New spawns use new values; in-flight NPCs keep the
                     config snapshot they were bound to (see §8 determinism).
```

- **`NpcConfigStore`** (new, `ERServer/persist/`): one `LoadAll()` that runs a single
  `SELECT` through the existing `OdbcConnectionPool` and maps rows → `std::vector<NpcRow>`.
  No per-tick or write path (config is read-only to the server; editing is an ops action).
- **`ServerUniverse::LoadAiConfig(NpcBehaviorCatalog)`** + `AiConfig(configId)` accessor +
  `m_npcConfig{ DefaultNpcCatalog() }` member — a direct copy of the `LoadCombat` /
  `m_combat{ DefaultCombatCatalog() }` shape (`ServerUniverse.h` L211–224).
- **Linux testability:** because the catalog is a plain NeuronCore struct with a baked-in
  default, every AI/steering test runs on the `testrunner` with a hand-built catalog and
  **no SQL** — only the thin `NpcConfigStore` ODBC mapping is Windows-only (validated on
  the build agent, like the other stores).

### 6.4 Validation on load (replacing the `datacheck` we give up)

Cooked game-data is validated by `datacheck` before it ships; a SQL table can be edited
out-of-band, so the server **must validate on load** and refuse/clamp bad rows (logging
which). Rules (the SQL-side analog of `ValidateUniverseDataset`):

`aggroRange > 0`; `deaggroRange ≥ aggroRange`; `leashRange ≥ deaggroRange`;
`patrolRadius > 0`; `0 < patrolSpeed`, `0 < orbitSpeed`; `standoffRange > 0`;
`0 ≤ orbitKRadialPct ≤ 100`; `0 ≤ fleeHullPct ≤ 1`; `FitCode` resolves in the combat
catalog. A row failing a hard rule falls back to `DefaultNpcConfig()` for that field and is
logged; a missing/empty table → all defaults (server still boots).

### 6.5 Why this is an intentional exception to §15

`schema.sql` (CATALOG / BALANCE BOUNDARY) and masterplan §15 state that *stats / recipes /
anomaly-invasion **definitions*** are **game data, not SQL**. Putting AI tuning in SQL is a
deliberate, scoped exception, justified by the requirement for **live-ops editing without a
build/cook/redeploy**, and bounded so it doesn't erode the rest of the boundary:

| Concern the boundary protects | How this design preserves it |
| --- | --- |
| **Determinism / replay** (data versioned with the build, §7.2/§16) | Config is read **once** into an immutable session snapshot; the record/replay log records the catalog's **version + hash** so a replay re-binds identical values. Hot-reload bumps the version (a logged sim event), so a replay knows when values changed. |
| **Validation** (`datacheck` gates) | Server-side `Validate-on-load` (§6.4) replaces the CI gate. |
| **Hot path never hits SQL** (§9) | Config is resident in memory; the 30 Hz loop only reads the struct. |
| **Scope creep** (everything migrating to SQL) | **Only behaviour *tuning* moves.** Hull/module/fit *stats* stay cooked game-data (the NPC's *fit* is still `FitCode` → the cooked combat catalog); spawn *placement* stays in the cooked universe/anomaly data (M7). |

The amendment to record (a one-line carve-out in `schema.sql`'s boundary note and masterplan
§15) is listed in §12. **This design proposes the wording; it does not edit those files —
that is the review gate.**

---

## 7. Server integration (`AiSystem` rewrite)

`AiSystem` keeps its place **first** in `Step()` (§7.2 ordering) and its target scan (all
`OwnerId.player != 0`, `hull.cur > 0`, priority by logi/EWAR — unchanged). Per NPC it now:

1. `const auto& cfg = AiConfig(npc.configId);`
2. compute `hullFrac`, nearest/priority target, `distToTarget`, `distToAnchor`.
3. `npc.state = NextAiState(npc, cfg, hasTarget, distToTarget, distToAnchor, hullFrac);`
   (rewritten to return the four reachable states of §3, with the hysteresis/leash rules).
4. translate state → `FleetOrder`:
   - **Patrol** → `Move(PatrolWaypoint(...))` at `patrolSpeed` (advance `waypointSeq` on arrival).
   - **Engage** → `Orbit(targetNetId, range = standoffRange)`; set `orbitDir` on first entry.
   - **Return** → `Move(anchor)` at `patrolSpeed`.
   - **Flee** → `Move(anchor)` (away from target) at flee speed; hold fire.
5. `FleetOrderSystem` executes the order, routing **Engage's** `Orbit` through the new
   `StepOrbit` (§4.2); `CombatSystem` fires via the broadened gate (§4.3).

Spawning: `SpawnNpcGuardian`/`SpawnNpcSite` gain a `configCode` parameter (default
`'raider.light'`); they resolve it to a `ConfigId`, set `npc.anchor = spawnPos`,
`npc.configId`, install `cfg.fitCode`. The ring-spawn helper stays (it is just placement);
each spawned NPC now **patrols its own boundary** instead of standing on its ring node.

---

## 8. Snapshot, replication & warm-restart

- **Client replication:** unchanged. NPC `Transform`/HP/`EntityKind::NpcUnit` already
  replicate (§8); the kite/patrol motion is ordinary movement on the wire. AI internals
  (`NpcAi`, config) are **server-only** — no snapshot wire-format change, no client work.
- **Warm-restart (`WarmRestart.h::PersistNpc`):** today it stores
  `netId,x,y,z,hp,siteId,aiState` — **insufficient** for a roamer. A restored NPC must
  resume the *same* patrol path and binding, so `PersistNpc` gains:
  `anchorX/Y/Z` (the patrol centre), `configId`, `waypointSeq`, `orbitDir`, `targetNetId`.
  These extend the versioned `Encode`/`Decode` and the `StateHash` (the existing pattern —
  add fields + bump the blob version). The **config values themselves are *not* in the
  blob** (they are reloaded from SQL on boot); only the `configId` reference is, so a
  restored NPC re-binds to its row. If the row vanished between runs, fall back to
  `DefaultNpcConfig()` and log.
- **Determinism across restart:** because the patrol path is a pure function of
  `(netId, waypointSeq, anchor, radius)` and all four are persisted, a warm-restart
  reproduces the identical behaviour — the same property the §16.2 `StateHash` verifies.

---

## 9. Determinism & test plan

Every feature ships with its `*Test` cases in the same commit (README/Contributing). The
pure rules go in NeuronCore so they run on the Linux `testrunner` (no SQL/Windows needed).

**`testrunner` (NeuronCore, pure):**
1. **Patrol stays in bounds** — step a patrolling NPC for N ticks; assert
   `dist(pos, anchor) ≤ patrolRadius + ε` always; assert it actually moves (not parked).
2. **Patrol is deterministic** — two NPCs with the same `(netId, seq, anchor, radius)`
   yield identical waypoints; a re-run reproduces the path (replay guarantee).
3. **Aggro on proximity** — Patrol → Engage exactly when a player ship crosses
   `aggroRange`; not before.
4. **Hysteresis / leash** — Engage → Return only past `deaggroRange`/`leashRange`; no
   flapping when the player loiters near `aggroRange`.
5. **Kite holds range** — over many ticks in Engage, `dist(npc, target)` converges to and
   stays within ±5 % of `standoffRange` (never closes to brawl).
6. **Orbit circles** — the bearing `atan2(rz, rx)` advances monotonically (sign = `orbitDir`)
   while radius stays ~constant → proves *tangential* motion, the gap today.
7. **Fire while kiting** — with the broadened gate, an Engaging NPC at `standoffRange ≤
   optimal+falloff` produces shots/projectiles; outside reach, none.
8. **Flee threshold** — drops below `fleeHpFrac` → Flee → moves away; `fleeHpFrac=0` → never.
9. **Config load + validate** — `BuildNpcCatalog` clamps/rejects bad rows per §6.4; empty
   table → all defaults; the sim runs identically to the old hard-coded constants when the
   seed row equals the old values (regression anchor).
10. **Warm-restart round-trip** — `Encode`→`Decode` a `PersistNpc` with the new fields;
    `StateHash` matches; the restored NPC continues the same patrol/engage.

**ERServer/SQL (Windows build agent):** `NpcConfigStore.LoadAll()` maps seeded rows;
`LoadAiConfig` swaps the live catalog; an end-to-end `ERHeadless` run spawns a configured
raider, flies a bot ship in, and observes patrol→engage→circle→fire.

---

## 10. Files touched (for the follow-up implementation task)

| File | Change |
| --- | --- |
| `NeuronCore/Components.h` | extend `NpcAi` (§5); rename `home`→`anchor` |
| `NeuronCore/Fleet.h` *(or new `AiSteering.h`)* | `PatrolWaypoint`, `StepOrbit`, rewritten `NextAiState`; keep `StepStandoff` |
| `NeuronCore/ServerUniverse.h` | `AiSystem` rewrite; broaden `CombatSystem` fire gate; `m_npcConfig` + `LoadAiConfig`/`AiConfig`; `SpawnNpc*` take `configCode`; drop the AI `constexpr`s |
| `NeuronCore/WarmRestart.h` | extend `PersistNpc` (+anchor, configId, waypointSeq, orbitDir, target) + `StateHash`/version bump |
| `NeuronCore/` (new) | `NpcBehaviorConfig` POD + `DefaultNpcCatalog()` + `BuildNpcCatalog`/validate (could live in a small `NpcConfig.h`) |
| `ERServer/persist/` (new) | `NpcConfigStore` (ODBC `SELECT` → rows) |
| `ERServer/ERServer.cpp` | call `NpcConfigStore.LoadAll` → `LoadAiConfig` in startup |
| `Config/db/migrations/006_pve_ai_config.sql` (new) | table + seed row |
| `Config/db/schema.sql` | add table to canonical schema + boundary-amendment note |
| `Testing/…`, `NeuronTools/testrunner/…` | the §9 cases |

---

## 11. Implementation phasing (when approved)

1. **Pure core** — `NpcBehaviorConfig` + defaults + validate; `PatrolWaypoint`, `StepOrbit`,
   `NextAiState`; testrunner cases 1–2,5,6,8. *(No server/SQL — fully Linux-testable.)*
2. **Wire into the sim** — `AiSystem` rewrite + fire-gate + `SpawnNpc*` config code; cases
   3,4,7,9. Replaces the guardian behaviour.
3. **Warm-restart** — extend `PersistNpc`; case 10.
4. **SQL** — migration 006 + seed + `NpcConfigStore` + startup `LoadAiConfig`; Windows
   integration test. Hot-reload admin hook (§26) optional in this phase.
5. **Docs** — fold the §6.5 amendment into `schema.sql`/masterplan §15; update this doc's
   status to "implemented".

Phases 1–3 deliver the full *behaviour* with compiled-in defaults; phase 4 delivers the
*database-stored* requirement. Each phase is independently testable and shippable.

---

## 12. Required amendments to register (review gate)

1. **`schema.sql`** — add `NpcBehaviorConfigs` and a one-line carve-out in the CATALOG /
   BALANCE BOUNDARY note: *"AI behaviour **tuning** (NpcBehaviorConfigs) is SQL-stored for
   live-ops; hull/module/fit **stats** and spawn placement remain cooked game-data."*
2. **`masterplan.md` §15** — same carve-out; **§13.7** — note the roaming-kiter as the M6/M7
   baseline NPC behaviour and that its tuning is the first SQL-backed live-ops lever (§26).
3. **`docs/design/README.md`** — index row for this doc (added with this draft).

---

## 13. Open questions / future work

- **3-D orbit** — circling is x–z-plane for now; full 3-D (orbit-plane from the approach
  vector) is a `StepOrbit` extension if vertical separation becomes meaningful.
- **Multiple behaviours** — if Sentry/Picket/Swarm logic is later wanted, add a
  `BehaviourKind` column + a switch in `NextAiState`; the SQL/loading plumbing already
  supports it (this is why config is keyed, not singleton).
- **Group aggro / assist** — NPCs currently aggro independently; site-wide "assist the
  primaried ally" is an M7 escalation behaviour.
- **Spawn ownership** — placement still comes from `SpawnNpcSite`/the demo seed; M7's event
  director (§26) will schedule spawns from the cooked anomaly/invasion templates and pass
  the `configCode` — this design is the substrate it drives.
- **Per-region scaling** — should `aggroRange`/difficulty scale by security tier
  (high→low→null)? Cleanest as distinct config rows referenced by the region's spawn table
  (M7), keeping the AI itself tier-agnostic.
