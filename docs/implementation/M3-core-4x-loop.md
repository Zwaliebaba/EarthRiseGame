# M3 — Core 4X Loop, Fleet Command & Navigation (Implementation Plan)

> Derived from [`../masterplan.md`](../masterplan.md) §17 (milestone **M3**).
> **Status:** 🔨 Active (M0/M1a/M1b/M2 complete). **Server track: areas A + C + D done;**
> B/E/F/G/H not started.
> **Plan style:** feature-area sections (see [`README.md`](README.md)).

## Milestone goal (verbatim from §17)

> **M3 — Core 4X loop, fleet command & navigation** *(L)* — nodes, harvesting, storage,
> build queue, sensor/fog; **RTS fleet control** (select/move/attack/guard/control-groups)
> over a 6–12-ship fleet + mobile base; **navigation: warp + jump-beacon network** (fuel,
> interdiction) + **starmap UI**; tactical **overview list**.
> **Done:** harvest → return → build a ship, **warp/jump across beacons**, and **command a
> multi-ship fleet** to clear a basic NPC site, server-authoritative.

## Status summary (this branch)

| Area | Status | Notes |
| --- | --- | --- |
| **A** Sim entities & shared rules | ✅ done | components (`OwnerId`/`ResourceNodeTag`/`Cargo`/`Storage`/`BuildQueue`/`FleetMember`/`Sensor`), pure rules (`Economy.h`), fleet spawning + data-driven cap, the build-queue system; balance = cooked `EconomyTuning`. |
| **B** Fleet command — intents | ⏳ not started | `Command.h` still carries only `MoveCommand`. |
| **C** eXploit loop (harvest→return→build) | ✅ done | `HarvestSystem` auto-pilot (travel → mine → return → deposit) + the build queue; nodes spawn from the dataset's fields; `OrderHarvest` is the entry point. |
| **D** Navigation — warp + jump | ✅ done | `Navigation.h` (warp/jump/interdiction), beacon graph loaded into `ServerUniverse`, fuel + spool/cooldown; balance = cooked `NavTuning`. |
| **E** eXplore — sensor/fog | ⏳ not started | `Sensor` component + `SensorDetect` rule exist (A); the per-player detected-set (fog) filter is pending. |
| **F** Basic PvE NPC site | ⏳ not started | no `ERServer/ai/` yet. |
| **G** Client UI — overview/command/starmap | ⏳ not started | depends on B/D/E/F + M2's HUD. |
| **H** ERHeadless bots & determinism | ⏳ not started | extend the bots to drive the full loop. |

> **Tooling/data:** `datacook`/`datacheck` (`NeuronTools/datacook/`) + `Config/universe/sol-frontier.universe`
> cook the universe layout (regions, beacon graph, resource fields) + nav/economy tuning.
> **Visibility:** `ServerUniverse::SpawnDemoSeed` places working beacons + a resource node + a
> starter fleet near spawn so the M3 entities render in the live client (a dev seed; the real
> path is ERServer loading the cooked universe + areas B/C/G).
> **Verification:** all of the above is checked headless on the Linux `testrunner`; the
> Windows/MSVC client+server build is **not** exercised in this environment.

## Scope at a glance

- **In scope:** the server-authoritative **4X gameplay loop** — eXplore (sensor/fog,
  scanning), eXpand (relocate base, navigation), eXploit (resource nodes → harvest →
  storage → build queue → new ship), eXterminate (command a fleet to clear a *basic* NPC
  site). Plus **fleet command** (select/move/attack/guard/orbit/control-groups as intents,
  §8.4/§23.4), **navigation** (warp + jump-beacon network with fuel + interdiction, §13.12),
  and the client **overview list + fleet command bar + starmap UI** that drive it.
- **Out of scope (later milestones):**
  - **Full combat model** — role + fitting + damage-type counters, EWAR/logistics, fitting
    grid, loot-on-kill, base disable-not-destroy retreat — is **M6**. M3 clears a *basic*
    NPC site with **placeholder damage** (the existing `Health` component is enough); do
    **not** build the fitting/resist system here.
  - **Persistence** — accounts, SQL, outbox/write-behind, warm-restart — is **M5**. M3's
    economy state (cargo, storage, build queue, fuel, ownership) lives **in-memory** in the
    sim; an ERServer restart loses it for now. Keep state in a shape M5 can later snapshot.
  - **Scale & interest** (sector subscriptions, delta compression, snapshot job pool) is
    **M4**; M3 may keep the M1a "full snapshot" path, but **warp/jump interest prefetch**
    (R21) is prototyped here because navigation needs it.
  - **Tiered security / territorial conquest / markets / invasions / anomalies / insurance**
    are **M7**. M3 uses plain (non-claimable) beacons and a single hand-placed NPC site.
  - **Touch controls** are **M7**; M3 input is **mouse+keyboard** (§23.2). **Own-unit/fleet
    prediction** is decided at **M6** (§10.1) — M3 fleet uses interpolation + snap-on-ack;
    slow command-driven units stay interp-only.
- **Open questions that touch M3** (from §19):
  - **Warp/jump balance** (warp-speed vs distance; jump range/fuel/spool/cooldown;
    interdiction strength) — author as **game data** (§12.6) so it's tunable; pick first-pass
    numbers, validate feel with bots, don't hard-code. *Blocks balance, not structure.*
  - **Fleet-cap (6 vs 8 vs 12)** — keep **data-driven** (§13.1); default 💡 8. *Doesn't block.*
  - **Onboarding objective chain** — recommended for M3/M7 (§13.9). *Optional in M3; include
    a minimal "harvest → build → warp" guided sequence only if cheap; otherwise defer to M7.*

## Current state (what M1a/M1b/M2 left us)

- **Sim is M1a-level.** `NeuronCore/sim/ServerUniverse.h` spawns **one mobile `Base` per
  player**, applies a single validated `MoveCommand` (`SetBaseVelocity`→`ClampSpeed`), and
  steps `MovementSystem` at the fixed 30 Hz. `Components.h` defines `Transform`/`Velocity`/
  `BaseTag`/`ShipTag`/`NetId`/`Health` and **already enumerates** `EntityKind::{NpcUnit,
  ResourceNode,Projectile,LootContainer}` as placeholders — **no systems for them yet**.
- **Commands** (`sim/Command.h`) carry only `MoveCommand` (base velocity). M3 expands this
  to the full intent set.
- **Snapshots** build full-universe state (interest = everything until M4). Client has
  `replica`/`interp`/`session`/`control` (M1b) and renders via NeuronRender.
- **M2 (complete)** provides the render/HUD foundation M3's UI builds on: monospace
  Canvas HUD + **radar/overview basics** (M2 area F) and instanced CMO ships (M2 area B) —
  area G is now **unblocked**. The **`datacook`/`datacheck`** tool *executables* (an M2 area-A
  carry-over) are **now built in area D** (`NeuronTools/datacook/`), with the universe-layout
  schema, binary codec, and validation rules in `NeuronCore/UniverseData.h` — see area D.
- **No navigation, harvesting, build queue, sensor/fog, NPC AI, or fleet-command intents
  exist yet** — all net-new in M3. `ERServer/` has no `ai/`, `interest/`, or `simloop/`
  modules yet (only `netio/`).

---

## Feature areas

### A. Sim entities & shared rules (server-authoritative content)

- **Goal:** add the entities and **shared, pure sim rules** (§7.2) the 4X loop needs:
  resource nodes, a player-owned fleet of ships, cargo/storage, a build queue, and fuel.
- **Masterplan refs:** §13.0 (4X loop), §13.1 (fleet of 6–12 ships + base), §13.4 (economy
  chain, in-memory only at M3), §13.11 (entities), §7.2 (shared sim rules, identical
  client/server), §6 (movement/sectors).
- **Current state:** ✅ **done** — components + pure economy rules (`NeuronCore/Economy.h`) +
  the build-queue system & fleet spawning on `ServerUniverse`; balance is the cooked `EconomyTuning`.
- **Work:**
  - [x] New components (`Components.h`, stable IDs — wire contract): `OwnerId`(10),
        `ResourceNodeTag`(11), `Cargo`(12)/`Storage`(13) (itemised by `ResourceType` + capacity),
        `BuildQueue`(14), `FleetMember`(15), `Sensor`(16); `Fuel`(7) from area D. Bound in
        `SimComponents.cpp` + the test TU; snapshot-friendly for M5.
  - [x] **Fleet spawning & ownership:** `ServerUniverse::SpawnFleetShip` enforces the
        **data-driven fleet cap** (`EconomyTuning.fleetCap`, default 8); `OwnerId` ties base +
        ships to a player (≈ their base net id); `OwnedShipCount` counts per owner.
  - [x] **Shared sim rules** (pure, §7.2, `Economy.h`): `HarvestStep`, `DepositAll`,
        `CanAfford`/`BuildStep`, `ConsumeFuel`, `SensorDetect` — all read `EconomyTuning`
        (cooked `economy {}` block), **no balance constants in code**.
  - [x] **Build queue system:** `ServerUniverse::BuildSystem` consumes `Storage` → advances →
        spawns a ship at the base (`EnqueueBuild` enqueues; `DrainBuildCompleted` is the
        "build complete" feedback hook; in-memory until M5).
- **Tests (mirrored in `NeuronTools/testrunner/`, §16.1/§16.2):**
  - [x] Harvest: node yield decrements, cargo fills, capacity clamps (`EconomyTests`).
  - [x] Build: insufficient vs sufficient resources; progress over ticks; ship spawns +
        fleet-cap enforced (`EconomyTests`).
  - [x] Fuel consumption + sensor-range queries (`EconomyTests`).
- **Depends on:** nothing (server-side). **Blocks:** B, C, D, F.

### B. Fleet command — intents & validation (§8.4 / §23.4)

- **Goal:** expand the command layer from `MoveCommand` to the full RTS intent set, routed
  through the existing intent path; the server validates everything (never client-authoritative).
- **Masterplan refs:** §8.4 (intents/commands, server validates), §23.4 (selection & command
  model: single/additive/by-type/control-group, smart context action, queued commands),
  §13.1 (RTS fleet control), §23.2 (desktop affordances).
- **Current state:** `sim/Command.h` has only `MoveCommand`.
- **Work:**
  - [ ] Extend `Command.h` with intents: **Move, Attack, Guard, Orbit, KeepRange, Harvest,
        Warp, Jump, Retreat, Stop**, each targeting a set of owned entities (by `NetId`) +
        a target (entity/point/beacon). Versioned serde encoding (§7.2).
  - [ ] **Server-side validation** for every intent: ownership check (you only command your
        own entities), reachability, speed/rate clamps, target validity. Invalid intents are
        rejected, not applied (§8.4).
  - [ ] **Command queue + control groups:** intents **queue** (shift-chain), respect simple
        stances; control-group membership is client-side selection state mapped to entity
        sets in the intent (server stores no UI grouping).
  - [ ] **Smart context action** resolution lives client-side (area G) but the resulting
        concrete intent is what's validated server-side.
- **Tests:**
  - [ ] `NeuronCoreTest`: intent encode/decode round-trip; queueing order preserved.
  - [ ] `ERServerTest`: ownership rejection (can't command another player's ship); speed
        clamp; invalid-target rejection; valid intent mutates sim as expected.
- **Depends on:** A (entities to command). **Blocks:** C, D, F, G.

### C. eXploit loop — harvest → return → build

- **Goal:** the closed economic micro-loop that proves "eXploit": send a harvester to a
  node, fill cargo, return to base, deposit to storage, enqueue + complete a ship build.
- **Masterplan refs:** §13.0 (eXploit), §13.4 (raw→…→ship, in-memory at M3), §13.11.
- **Current state:** ✅ **done** — `ServerUniverse::HarvestSystem` drives the auto-pilot loop;
  `OrderHarvest` is the server entry point; nodes spawn from the dataset's fields.
- **Work:**
  - [x] Harvest system: `HarvestSystem` auto-pilots a harvester (a `HarvestOrder`) ToNode →
        Harvesting → ToBase → Depositing; mining uses the pure `HarvestStep` rule (area A);
        `harvesterSpeed`/`harvestRange` are cooked tuning. `OrderHarvest` is the entry point
        (area B routes a Harvest command here).
  - [x] Deposit: at base range → `DepositAll` moves `Cargo` into base `Storage`.
  - [x] Build: `BuildQueue` draws from `Storage` → spawns a ship (area A `BuildSystem`) →
        `DrainBuildCompleted` event hook.
  - [x] Resource nodes spawn from the dataset's **fields** (`SpawnFieldNodes`, deterministic
        ring typed by the field's composition). All state **in-memory**, snapshot-shaped.
- **Tests (mirrored in `NeuronTools/testrunner/`, §16.1/§16.2):**
  - [x] Full loop as a deterministic sequence: node depletes → cargo → storage → build →
        ship count +1 (`HarvestTests`).
  - [x] Harvester drains a small node then idles; deposits banked; `OrderHarvest` validation;
        fields spawn the right node count (`HarvestTests`).
  - [ ] `ERHeadlessTest`: a scripted bot drives harvest→return→build end-to-end (**area H**).
- **Depends on:** A, B (B's command wiring pending — driven via `OrderHarvest` for now).
  **Blocks:** Done gate "harvest → return → build a ship".

### D. Navigation — warp + jump-beacon network (§13.12)

- **Goal:** the three travel scales — sublight (exists) + **warp** (sim-stepped,
  interdictable) + **jump** (beacon-to-beacon, fuel + spool + cooldown) — server-authoritative.
- **Masterplan refs:** §13.12 (full navigation spec), §6.3 (sectors/interest), §8.4
  (validated), §13.6 (beacons are a `TerritoryStructure` type — but **non-claimable** in M3),
  R21 (interest prefetch). **Game data:** §12.6 (beacon graph + region layout via `datacook`).
- **Current state:** ✅ **done** — `NeuronCore/Navigation.h` (warp/jump rules + `NavigationSystem`);
  `ServerUniverse` loads the cooked beacon graph and exposes `BeginWarpTo`/`BeginJumpTo`/`Interdict`,
  server-authoritative; balance is the cooked `NavTuning`.
- **Work:**
  - [x] **Warp:** *align → warp* to a destination at high sublight; **travel time ∝ distance**;
        **not instant** → **interdictable** (`NavState.interdicted` drops it out; full EWAR is
        M6). Sim-stepped each tick (`Navigation.h` `StepNav`/`NavigationSystem`).
  - [x] **Jump drive + fuel:** beacons load as `Structure` entities (`BeaconTag`); jump between
        **linked** beacons consuming `Fuel`, with **spool-up** (vulnerability window) + post-jump
        **cooldown**; running dry / unlinked / busy are rejected (`BeginJumpTo` → `JumpReject`).
  - [x] **Mobile-base travel:** the base carries `Fuel` + `NavState` and warps/jumps with
        base-specific tuning (slower warp, larger fuel + longer spool).
  - [x] **Beacon graph + balance as game data** (§12.6): `datacook` cooks authored text → packed
        binary (codec/rules in `NeuronCore/UniverseData.h`); `datacheck` gates region refs,
        reciprocal links, public-graph connectivity, claimable-tier & weights; nav balance is a
        cooked `tuning {}` block (`NavTuning`). `ServerUniverse::LoadUniverse` spawns the beacons.
        Schema = `docs/design/universe-worldgen.md` §4; dataset =
        `Config/universe/sol-frontier.universe`. *(Resource fields stay in the dataset for area C.)*
  - [x] **Interest prefetch (R21):** `OnTravelStart` records the destination sector on warp/jump
        start (lightweight hook; M3 keeps the full-snapshot path, full interest mgmt is M4).
- **Tests:**
  - [x] Warp travel-time ∝ distance; arrival exact; fuel decrement on jump; spool→cooldown
        timing; jump rejected when unlinked / out of fuel — `NavigationTests` (testrunner;
        mirrors `NeuronCoreTest`).
  - [x] Interdiction drops a unit out of an in-progress warp; jump validation (fuel, link,
        not-at-beacon, busy) — `NavigationTests` (mirrors `ERServerTest`; **ownership** rides on
        the area-B intent layer).
  - [x] `NeuronTools` `datacheck`: beacon-graph referential integrity — parse/validate/
        round-trip covered by `UniverseDataTests` (testrunner); `make check` gates
        `Config/universe/*.universe`.
- **Depends on:** A, B; M2 area A (`datacook`/`datacheck`). **Blocks:** Done "warp/jump
  across beacons"; area G starmap.

### E. eXplore — sensor range & fog of war

- **Goal:** sensor-bounded visibility and scanning so exploration has teeth (and so the
  overview only shows what you can detect).
- **Masterplan refs:** §13.0 (eXplore), §13.7 (scannable sites — basic in M3), §22.2
  (sensor/scan UI), §6.3 (interest grid).
- **Current state:** client sees everything (full snapshot).
- **Work:**
  - [ ] `Sensor` range per base/ship; server computes a per-player **detected set** (fog):
        entities outside sensor range are hidden/`Unknown` in that player's snapshot.
  - [ ] **Scan action:** reveal a node/beacon/NPC-site at range over time (feeds D's
        "scanned destination" for warp and F's site).
  - [ ] Keep this **interest-light** at M3 (full sector-subscription interest is M4); fog is
        a visibility filter on the existing snapshot path.
- **Tests:**
  - [ ] `NeuronCoreTest`: detected-set membership by range; scan reveals after dwell time.
  - [ ] `ERServerTest`: a player's snapshot excludes undetected entities.
- **Depends on:** A. **Blocks:** D (scan→warp target), F (scan the site), G (overview/scan UI).

### F. Basic PvE NPC site (server AI)

- **Goal:** a hand-placed NPC site with guardian units a fleet can be commanded to clear —
  enough to prove "command a multi-ship fleet to clear a basic NPC site, server-authoritative."
- **Masterplan refs:** §13.7 (PvE; NPC AI is server ECS in `ERServer/ai/`), §13.11
  (`NpcUnit`, `AnomalySite`), §9 (sim owns state). **Combat is placeholder — full model M6.**
- **Current state:** no `ERServer/ai/`, no NPC entities.
- **Work:**
  - [ ] `ERServer/ai/`: NPC AI states — **patrol / aggro / flee / defend** (escalation is
        M7). NPCs are server ECS entities (distinct from bots).
  - [ ] A single **scripted site**: a cluster of guardian `NpcUnit`s; "cleared" when all
        guardians are destroyed.
  - [ ] **Placeholder damage:** weapons apply flat damage to the existing `Health`
        component; no resists/fitting/damage-types (those are M6). Destroyed entity is
        removed (loot-on-kill is M6).
- **Tests:**
  - [ ] `ERServerTest`: NPC state transitions (idle→aggro on detect→flee at low HP); a fleet
        applying damage clears the site; site "cleared" event fires once.
- **Depends on:** A, B (commandable fleet), E (detect the site). **Blocks:** Done "clear a
  basic NPC site".

### G. Client UI — overview, fleet command bar, starmap, panels (§22/§23)

- **Goal:** make the loop playable on desktop: a functional **overview list** (primary
  selection/command surface), **fleet command bar**, **selected/target panels**, **sensor/
  scan UI**, and a **starmap** for the beacon network — built on M2's HUD/overview basics.
- **Masterplan refs:** §22.1 (screens: starmap, inventory/build queue), §22.2 (HUD:
  command bar, selected/target panels, sensor/scan), §22.3 (overview as primary surface),
  §23.1/§23.2 (overview-driven command; desktop affordances), §13.12 (starmap = beacon
  graph, set destination/route). **Design doc:** `docs/design/ui-hud-layout.md`.
- **Current state:** M2 gives radar/overview *basics* (display); no command wiring, no
  starmap, no panels.
- **Work:**
  - [ ] **Overview → command:** click/select a contact; **smart context action** resolves
        intent by target (empty=move, enemy=attack, node=harvest, beacon=warp/jump); small
        **contextual command bar** (move/attack/guard/orbit/keep-range/warp/retreat). Emits
        validated intents (area B).
  - [ ] **Control groups:** `Ctrl+#` set / `#` recall; box-select (desktop convenience,
        §23.2); double-click select-by-type.
  - [ ] **Selected-unit / target panels:** HP bars (shield/armor/hull shown as plain HP at
        M3; layered resists are M6), range, base fuel/jump-cooldown.
  - [ ] **Sensor/scan UI** (area E) + **fleet command bar** (control groups, orders, stance).
  - [ ] **Starmap / navigation UI:** render the beacon graph (area D data), set
        destination/route (autopilot across beacons, §23.1), fleet/base location.
  - [ ] Inventory/cargo + **build queue** screen (enqueue builds, area C).
- **Tests (`NeuronClientTest`, §16.1):**
  - [ ] Smart-action → correct intent per target type.
  - [ ] Control-group set/recall maps to the right entity set.
  - [ ] Starmap route solver across the beacon graph (shortest path / reachable set).
  - [ ] Overview sort/filter on detected set (ties to E fog).
- **Depends on:** B, D, E, F; M2 areas F (HUD/overview), B (instanced ships). **Blocks:**
  Done "command a multi-ship fleet".

### H. ERHeadless bots & determinism harness

- **Goal:** scripted bots that drive the whole M3 loop for integration + the record/replay
  determinism harness (the primary netcode/sim debugging tool).
- **Masterplan refs:** §10.3 (ERHeadless bots ≠ NPCs), §16.1/§16.2 (ERHeadlessTest,
  record/replay), §16.3 (perf gates).
- **Current state:** ERHeadless drives ≥3 base-moving bots (M1a).
- **Work:**
  - [ ] Extend the bot controller (`NeuronClient`, Control filter, scripted) to issue the new
        intents: harvest, return, enqueue build, warp/jump across beacons, attack a site.
  - [ ] Integration scenario: bots run **harvest→build→warp/jump→clear-site** end-to-end
        against a real ERServer (server-authoritative).
  - [ ] **Record/replay determinism:** same input log → identical sim (extend the M1a harness
        to the new systems).
- **Tests (`ERHeadlessTest`):**
  - [ ] Multi-bot full-loop integration passes server-authoritatively.
  - [ ] Record/replay: identical sim hash across two runs of the same input log.
- **Depends on:** A–F. **Blocks:** Done gate verification (it *is* the end-to-end check).

---

## Suggested order / dependency notes

1. **A (entities & rules)** first — foundation for everything server-side.
2. Then **B (intents)** — needed by C, D, F, G.
3. **C (harvest loop)**, **D (navigation)**, **E (fog)**, **F (NPC site)** can largely run in
   **parallel** once A+B exist (D's `datacook`/`datacheck` tooling is now built; E feeds D's
   scan targets and F's detection, so sequence E slightly ahead of the D-target/F-detect wiring).
4. **G (client UI)** follows the server features it surfaces (B, D, E, F) and M2's HUD.
5. **H (bots/determinism)** continuously, and is the final end-to-end gate.

Server track (A→B→{C,D,E,F}) is independent of the M2 render track. **G** hard-depended on
M2, which is **now complete**, so it's unblocked. **Cross-milestone:** the `datacook`/
`datacheck` tooling D needs (nominally an M2 area-A carry-over) is **now built in D**
(`NeuronTools/datacook/` + `NeuronCore/UniverseData.h`). G builds on M2 areas B/F (instanced
ships + HUD/overview basics), both done.

## Done gate (mirrors §17 "Done")

- [x] **Harvest → return → build a ship**, server-authoritative (A, C) — *bot-driven E2E is area H.*
- [x] **Warp and jump across beacons** (fuel, spool/cooldown, interdiction), server-
      authoritative (D) — *the bot-driven end-to-end run is area H.*
- [ ] **Command a multi-ship fleet** (6–12 ships + base) via overview/command bar +
      control groups (B, G).
- [ ] **Clear a basic NPC site** with that fleet (F).
- [ ] Driven end-to-end by ERHeadless bots with **record/replay determinism** holding (H).
- [ ] All matching `<project>Test` suites green — NeuronCore/Client/Server/Headless (§16.1).
- [ ] Sim tick p50/p99 within budget with the fleet + nodes + NPC site live (§16.3, App. B;
      note the contested-sector load test itself is M4/R16).
