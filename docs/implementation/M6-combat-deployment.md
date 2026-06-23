# M6 — Combat Model & Deployment (Implementation Plan)

> Derived from [`../masterplan.md`](../masterplan.md) §17 (milestone **M6**).
> **Status:** 🔨 **Combat track implemented** — the platform-independent **combat model
> (areas A–G)** and the **balance-gate sim half (area M)** are landed in `NeuronCore` and
> **green on the Linux `testrunner`** (§16.2). The **Windows / real-infra halves — client
> VFX/SFX (H), optional prediction (I), Azure SQL (J), Kubernetes (K), the Store pass (L)**
> and the **deployed-stack** half of the balance gate (M) — are **not started** (they
> can't be compiled/exercised off a Windows agent). Per [`README.md`](README.md) rule 2
> this plan stays **subordinate to the masterplan**.
>
> **Implementation status (this pass):**
> - ✅ **A** Combat catalog as game data — `NeuronCore/CombatData.h` (model + versioned
>   codec + `ValidateCombatCatalog`/`ValidateFit` rules + the authored first-pass
>   `DefaultCombatCatalog`, the §15 catalog/balance boundary); shared primitives in
>   `CombatTypes.h`. Tests: `CombatDataTests`. *(A bespoke text-DSL cook tool under
>   `NeuronTools/datacook/` is deferred — the authored catalog + binary codec + validator
>   already make balance data, not code; the cooked-blob loader `LoadCombatFromCooked` exists.)*
> - ✅ **B** Layered defense + fitting ECS — `DefenseLayers`/`ResistProfile`/`Fitting`/
>   `HullInfo`/`EwarStatus`/`Projectile`/`LootContainer`/`BaseCombat` components;
>   `Health` kept as a synced derived **mirror** (= layer totals) so the wire/SimHash/HUD/
>   M5-persistence surfaces are unchanged. `ServerUniverse::InstallFit` spawns from the
>   catalog. Tests: `FittingTests`. *(Projectile/loot persistence across a warm restart is
>   deferred — they are snapshot-shaped replicated entities but sub-second/transient, and
>   extending the frozen M5 `PersistState` blob was out of scope; base `BaseState` IS
>   persisted, area G.)*
> - ✅ **C** Damage & defense sim rules — `NeuronCore/Combat.h` (`ApplyDamage` through three
>   layers + resists, tracking/falloff, shield regen, remote rep), pure + deterministic.
>   Tests: `CombatTests`.
> - ✅ **D** Weapons & projectiles with local sub-stepping (ballistic, anti-tunneling).
>   Tests: `ProjectileTests`.
> - ✅ **E** EWAR & logistics — jam/web/warp-disrupt(tackle→interdiction)/sensor-damp +
>   remote rep. Tests: `EwarLogiTests`.
> - ✅ **F** PvE AI on the real model — fitted NPCs, target priority (primary the logi/EWAR),
>   hull-threshold flee. Tests: `CombatScenarioTests` + the extended `FleetTests`.
> - ✅ **G** Loot-on-kill + base disable-not-destroy — loot containers + killmail +
>   cargo-loss as drainable economy events (→ M5 outbox), base retreats→disabled and is
>   never destroyed, state persisted. Tests: `CombatScenarioTests`.
> - ✅ **M (sim half)** Balance gates — composition > numbers, damage-type-vs-tank counter,
>   logi sustain, swarm-vs-heavy size RPS, and a mistune-bites-the-gate check, run as
>   deterministic focus-fire fleet sims on the `testrunner`. Tests: `BalanceTests`. *(The
>   statistical N-sim win-rate BANDS run on the Windows ERHeadless agent against the
>   deployed stack — that half is not started.)*
> - ⏳ **H, I, J, K, L** and the deployed-stack half of **M** — **not started** (Windows /
>   Azure / K8s / Store / live client, unverifiable in this Linux environment).
>
> The Windows MSTest projects get the new component **bindings** (so they still build); the
> combat **test cases** live on the `testrunner` (the §16.2 mirror, the verifiable gate here).
> **Plan style:** feature-area sections (see [`README.md`](README.md)).
> **Verification:** M6 has two halves with different test homes. The **combat model** (areas
> A–G) is platform-independent shared-sim logic → it lands with `NeuronCoreTest` + Linux
> `testrunner` mirrors (§16.1/§16.2), the same home as the M3 placeholder it replaces
> (`FleetTests.cpp`). The **balance gates** (M) run on the **Windows ERHeadless agent** but their
> sim-rule half mirrors on Linux. **Client VFX/SFX (H), prediction *feel* (I), the Azure SQL
> migration (J), the Kubernetes prod topology (K) and the Store-cert pass (L)** are
> Windows-agent / real-infra and verified there (a dev Azure SQL + a Windows-node cluster, or a
> documented parity check). Assumes **M5** (real auth + persistence + warm-restart, incl. its
> Windows surfaces) is **closed** when M6 starts.

## Milestone goal (verbatim from §17)

> **M6 — Combat model & deployment** *(L)* — **role + fitting + damage-type combat**
> (shield/armor/hull + resists, module fitting grid, EWAR/logistics archetypes),
> weapons/projectiles (local sub-stepping for fast shots), PvE AI, **loot-on-kill +
> base disable-not-destroy**; **Kubernetes production deploy (Windows nodes + UDP LB)**
> and **Azure SQL migration**; optional own-unit/fleet **prediction** where feel needs
> it; Store-compliance pass.
> **Done:** balanced fleet-vs-fleet fights where fit & composition beat raw numbers, playable
> on the prod topology.

## Scope at a glance

- **In scope:** turning M3's **placeholder combat** (flat damage to a single HP bar) into the
  masterplan's real model — **three defense layers (shield/armor/hull) with per-damage-type
  resists** (§13.2), a **module fitting grid** with a **power/CPU budget** (§13.2), **weapons +
  projectiles** with intra-tick **sub-stepping** for fast shots, **EWAR/logistics/tackle**
  archetypes, **PvE AI** that actually fights with the new model, and the two destruction
  outcomes — **loot-on-kill** (an economy event, §15) and **base disable-not-destroy** (§13.1).
  Plus the **client combat feel**: VFX/SFX (§11.2/§11.3) and an **optional, feel-gated**
  own-unit prediction path (§10.1, §19). And the **production deployment** half: the **Azure SQL
  migration** (connection-string + managed-identity change, §15/§20), the **Kubernetes prod
  topology** (Windows nodes + UDP LoadBalancer + one-pod-per-shard affinity, §20), and the
  **Store-compliance pass** (§25). The milestone is **gated by ERHeadless balance fights**
  (`combat-balance.md` §7) run on the prod topology.
- **Out of scope (later milestones):**
  - **Tiered-security enforcement (high/low/null PvP rules), territorial conquest (capture/hold
    timers, upkeep), player crafting economy + markets + insurance *payout* UI, dynamic faction
    invasions + procedural anomalies (the *content + event director*)** are **M7**. M6 builds the
    **combat model and the AI substrate** those features consume — e.g. M6 delivers NPC combatants
    that fight with real fits, M7 schedules them as invasions/anomalies (§26 event director).
  - **The full UI suite — fitting / market / research / inventory / territory screens** (§22.1) —
    is **M7**. M6 combat is exercised through **data-driven fit templates + bots** (`FitTemplates`,
    §13.2), *not* a player-facing fitting screen. The **research/tech-tree unlock system** (§13.3)
    is also M7; M6 only needs the **catalog with tier tags** (area A), which bots fit directly.
  - **Entity aggregation / LOD (fleet-as-cluster) + projectile batching** are the **committed M7
    feature** (R16). M6's new **projectile** entities ride the existing M4 interest pipeline (hard
    culling + per-client visible-entity cap); M6 must hold App. B with that alone and **measure
    where projectiles pressure the cap** — evidence M7 batching is needed.
  - **Active combat abilities** (overheat / MWD burst / EWAR burst) are **post-launch**, gated by
    prediction (§13.2/§19) — not M6 even if area I lands.
  - **Full ship insurance** (premiums/claims/payout) is **M7** (schema `InsuranceContracts` is
    provisioned); M6 emits the **kill/loss events** insurance later consumes (area G).
  - **Federated Entra ID *user login*** stays post-launch (§14); M6's identity change is only the
    **app→DB** auth (SQL login → **managed identity**) that comes with Azure SQL (area J).
- **Open questions that touch M6** (from §19 + `combat-balance.md` §8):
  - **Does any subsystem need own-unit/fleet prediction?** — the §19 question explicitly *"decided
    by feel at M6."* This is **area I**: measure direct-control feel under live RTT, then add a
    narrow `predict/` path or **document "not needed yet."** *Drives area I; blocks nothing else.*
  - **Balance tunables** — exact resist % per layer/type, tracking-vs-sig, logi rep/sec, EWAR
    strength/duration, insurance payout, **fleet cap (6/8/12)** (`combat-balance.md` §7–8, §19,
    App. B/R16). All authored as **game data** (area A), swept by the balance gates (area M).
    *Blocks tuning, not structure.*
  - **Does the base mount offensive weapons or is it fire-support/utility only?**
    (`combat-balance.md` §8, §13.1) — pick a first-pass (💡 fire-support + light defensive
    weapons) authored in data. *Decide in area B (the base fit), tune in M.*
  - **Explosive as a 4th damage type** (`combat-balance.md` §8) — **deferred**; the damage-type set
    is data-driven (area A) so adding it later is **data, not code**. Doesn't block M6.
  - **Azure SQL tier sizing / read-replicas / geo-redundancy** (§19) — deferred; doesn't block the
    **migration mechanism** (area J). Pick a dev tier; production sizing is a live-ops decision.

## Current state (what M1a–M5 left us)

> File-level baseline. Where M6 replaces an M3 placeholder, the placeholder is named; the
> Explore pass confirmed every combat path below is explicitly tagged "M6" in-code.

- **Combat is an M3 placeholder — flat damage, one HP bar.** `NeuronCore/Fleet.h` carries
  `ApplyDamage` (flat damage to `Health`, clamp to 0), `WeaponDamage` (accumulates `dps×dt` into
  `Weapon.pending`), `InWeaponRange`, and `NextAiState`; its header comment says *"combat is
  placeholder at M3: flat damage … no resists / damage-types / fitting — those are M6."*
  `NeuronCore/Components.h` has `struct Weapon { float range; float dps; float pending; }` flagged
  *"full fitting/resist model is M6,"* and a **single-layer** `struct Health { hp; maxHp; }`.
- **The combat *system* exists but is hardcoded.** `NeuronCore/ServerUniverse.h` has
  `CombatSystem()` (iterates Attack orders → `ApplyDamage` → destroy), `SpawnNpcGuardian`/
  `SpawnNpcSite` + `m_siteAlive` tracking, and `AiSystem()` driving the NPC state machine. Balance
  is **hardcoded constants** (`kShipHp=500`, `kShipWeaponDps=60`, `kNpcHp=300`,
  `kNpcWeaponRange=1400`, `kNpcDps=20`) with a comment: *"the real model + data-driven tuning land
  at M6"*; and *"loot-on-kill is M6."*
- **Entity kinds are reserved but unmodelled.** `EntityKind::Projectile` and
  `EntityKind::LootContainer` are **enum values only** — there is **no** `struct Module`,
  `Fitting`, `Projectile`, or `LootContainer` component yet. M6 adds them.
- **The schema already provisions every M6 table** (M5 area B, `Config/db/schema.sql` +
  migration `002_gameplay_v0_9.sql`): `Bases`/`Ships` carry **three-layer HP**
  (`Shield/Armor/HullHp` + `Max*`), `Bases.BaseState` + `RetreatUntil` (**disable-not-destroy**),
  `Ships.ShipState` + `InsuranceId`; **`ShipModules`/`FitTemplates`/`FitTemplateSlots`** (fitting),
  **`LootContainers`/`LootContainerItems`**, **`InsuranceContracts`**, **`KillmailLog`**, and the
  **`ItemDefs`** catalog (with `Category` incl. module/hull/ammo). **Module *stats* are game data,
  not SQL** (§15 catalog/balance boundary). **Note the gap M6 closes:** the SQL HP is three-layer
  but the **ECS `Health` is single-layer** — area B reconciles the sim model to the schema.
- **Game-data tooling exists for universe layout, not combat.** `NeuronCore/UniverseData.h`
  (model/codec/rules) + `NeuronTools/datacook/` cook region/beacon-graph data (§12.6). M6 extends
  this catalog to hull/module/damage-type/resist stats (area A) and adds a `datacheck`-style
  validator.
- **Persistence + auth are real (M5).** The **write-through outbox + append-only ledger** (M5
  area D) is the mechanism loot/kill events ride (area G); **warm-restart snapshot + log** (M5
  area F) already serializes transient sim state (so projectiles/loot must be snapshot-shaped);
  **client reconnect with backoff/jitter** (M5 area G) is what K8s rolling restarts depend on
  (area K). M5 runs **self-hosted SQL over the network with a SQL login**, every statement kept
  **Azure-SQL-compatible** — area J cashes that in.
- **Client is interpolation-only.** `NeuronClient/Interpolator.h` `InterpBuffer` does **snap-on-ack
  only** (*"predict/reconcile deferred to post-M1"*); there is **no `predict/` module**
  (`IClientController.h` notes the same). M2 wired combat-adjacent VFX/SFX hooks; M6 fills the
  combat cues.
- **Deployment is dev-only.** `Config/deploy/Dockerfile` (Windows Server Core ltsc2022, ODBC
  Driver 18, UDP 7777, env `ER_DB_CONNSTR`/`ER_SERVER_PEPPER`/`ER_LISTEN_PORT`) +
  `docker-compose.dev.yml` exist. There are **no Kubernetes manifests, no UDP LoadBalancer, no
  Windows node-pool / Secret / affinity config** — net-new for area K.
- **UWP package exists.** `EarthRise/Package.appxmanifest` (identity, self-signed `CN=Zwaliebaba`,
  `internetClient` + `privateNetworkClientServer`, landscape). The Store-cert pass (area L)
  reviews capabilities, WACK, and the loopback-exempt path first validated at M1b.

---

## Feature areas

### A. Combat catalog as game data (hulls · modules · damage types · resists) (§12.6, §13.2, §15)

- **Goal:** move the hardcoded `ServerUniverse.h` combat constants into **authored, versioned game
  data** (§12.6) — **hull classes**, **ship roles**, **module defs** (weapon/tank/EWAR/logi/
  prop/sensor with **PG/CPU cost** + **damage-type/resist** stats), **damage-type set**, and
  **fit templates** — so balance is data, not code (§15 catalog/balance boundary).
- **Masterplan refs:** §12.6 (game data → versioned binary serde; `datacook`/`datacheck`), §13.2
  (fitting grid, damage types, resists), §13.3 (tier tags T1→T2→T3), §15 (SQL holds canonical
  `ItemDefs` ids only; stats are game data). **Design docs:** [`combat-balance.md`](../design/combat-balance.md)
  (§3 hull ladder, §4 role spreadsheet, §5 modules), [`tech-tree.md`](../design/tech-tree.md) (tier gates).
- **Current state:** `NeuronCore/UniverseData.h` + `NeuronTools/datacook/` cook universe-layout
  data; combat balance is hardcoded constants in `ServerUniverse.h`. `ItemDefs` ids exist in SQL.
- **Work:**
  - [ ] **Combat-catalog schema** in the game-data model (`NeuronCore/UniverseData.h` or a sibling
        `CombatData.h`): hull classes (slots H/M/L, PG/CPU, mass, sig, base layer HP), module defs
        (slot type, PG/CPU cost, effect params, **per-damage-type** values / **per-layer resists**),
        the **damage-type set** (Kinetic/Thermal/EM — extensible so Explosive is later *data*), and
        named **fit templates** (the bot loadouts). Each catalog row keys a canonical `ItemDefs`
        `Code` (e.g. `module.railgun.t1`, `hull.cruiser`).
  - [ ] **Cook + validate:** extend `NeuronTools/datacook/` to emit the combat catalog to the
        versioned binary (§7.2); add a **`datacheck`/combat validator** (the `wavcheck` pattern,
        §12.5) — every module references a valid slot/damage-type, PG/CPU ≥ 0, resists in range,
        fit templates fit their hull's slots+budget. Build-time failure on a bad row.
  - [ ] **Authoring source** for the `combat-balance.md` first-pass numbers (hull ladder §3, role
        multipliers §4, resist spreads ±40/±25/0 §2.2, module PG/CPU §5) as the **tunables** the
        balance gates (M) sweep — *no balance literal survives in code.*
  - [ ] **Hot-reload hook** (§26 live-ops): the catalog is reloadable so balance/pacing can change
        without a redeploy (ties to area M tuning).
- **Tests (`NeuronCoreTest`, §16.1; mirror in `NeuronTools/testrunner/CombatDataTests.cpp`, §16.2):**
  - [ ] Catalog cook→load round-trips; a malformed module/hull/fit is **rejected** by `datacheck`
        (bad slot, negative PG/CPU, out-of-range resist, fit over budget).
  - [ ] A loaded fit template resolves to real `ItemDefs` codes and obeys its hull's slot/PG/CPU
        budget (the fitting-validation rule shared with area B).
- **Depends on:** nothing (data + tooling). **Blocks:** B, C, D, E, F, M (every combat path reads it).

### B. Layered defense & fitting ECS model (§13.2, §13.11)

- **Goal:** replace single-layer `Health` with **shield/armor/hull + per-type resists**, and add
  the **fitting** model — `Module` instances on a hull's slot grid within its PG/CPU budget — plus
  the `Projectile` and `LootContainer` components the rest of the milestone needs.
- **Masterplan refs:** §13.2 (defense layers + resists; fitting grid), §13.11 (entity list: `Base`
  layered HP + disable-not-destroy state, `Ship` fitting grid, `Module`, `Projectile`,
  `LootContainer`). **Design doc:** [`combat-balance.md`](../design/combat-balance.md) §2/§5.
- **Current state:** `Components.h` has single-layer `Health`, placeholder `Weapon`, and
  `EntityKind::{Projectile,LootContainer}` enums **without structs**. Schema HP is already
  three-layer (the model B aligns to).
- **Work:**
  - [ ] **Layered defense component** (`Components.h`): `DefenseLayers { shield, armor, hull (cur+max) }`
        + a `ResistProfile` (per layer × damage type), replacing single-layer `Health` for combat
        entities. Field names/units mirror the schema (`Shield/Armor/HullHp`) so M5 persistence
        round-trips without a translation layer.
  - [ ] **Fitting components:** `Fitting { slots H/M/L, pgUsed/pgMax, cpuUsed/cpuMax }` + per-slot
        `Module` instances (ref a catalog def id from area A, + per-instance state e.g. cooldown).
        A **pure `ValidateFit(hull, modules)`** rule (shared with area A's check) enforces slot
        type/count + PG/CPU budget — server-authoritative, no over-budget fit is accepted.
  - [ ] **Projectile + loot components:** `Projectile { sourceNetId, damageType, baseDmg, vel,
        ttl, optimal, falloff, tracking }` and `LootContainer { items, expiresAt }` (area D/G use
        these). Both are **transient** → they live only in the warm-restart `SimSnapshots` blob
        (§15), never normalized.
  - [ ] **Base capital state:** model `BaseState { active/retreating/disabled }` + `retreatUntil`
        in the ECS (mirrors `Bases.BaseState`/`RetreatUntil`) for area G's disable-not-destroy.
  - [ ] **Migrate spawns:** `SpawnNpcGuardian`/base/ship spawns read **layered HP + a fit** from
        the catalog (area A) instead of the hardcoded `kShipHp`/`kNpcHp` constants.
- **Tests (`NeuronCoreTest`; `NeuronTools/testrunner/FittingTests.cpp` mirror):**
  - [ ] `ValidateFit` accepts a legal fit, rejects over-PG, over-CPU, and wrong-slot-type fits.
  - [ ] Layered-defense init from a catalog hull sets shield/armor/hull + resists; a ship spawned
        from a fit template has exactly its template's modules.
  - [ ] Projectile/loot components serialize into and out of the warm-restart blob (snapshot-shape).
- **Depends on:** A. **Blocks:** C, D, E, F, G.

### C. Damage & defense sim rules (resist · tracking · falloff · layers · regen/rep) (§7.2, §13.2)

- **Goal:** the **pure, deterministic** combat math — the `combat-balance.md` §2.3 formula applied
  through three layers, depleted outside-in, with shield regen, armor/hull no-regen, and the
  remote-rep hook — replacing `Fleet.h::ApplyDamage`'s flat model.
- **Masterplan refs:** §7.2 (shared sim rules = pure functions, deterministic, client+server;
  allocator discipline in the tick), §13.2 (defense layers, damage-type counters, range/tracking).
  **Design doc:** [`combat-balance.md`](../design/combat-balance.md) §2.3 (`effective_dmg = base ×
  (1 − resist[layer][type]) × tracking × falloff`).
- **Current state:** `Fleet.h` `ApplyDamage` is flat to single-layer `Health`; no resists,
  tracking, falloff, or regen.
- **Work:**
  - [ ] **Damage application** (new `NeuronCore/Combat.h`, the `Fleet.h` pattern): `ApplyDamage(
        DefenseLayers&, ResistProfile, damageType, baseDmg, tracking, falloff)` — compute effective
        damage per the formula, **deplete shield→armor→hull**, return outcome (absorbed / layer
        broken / killed). Replaces `Fleet.h::ApplyDamage`; `CombatSystem` calls the new path.
  - [ ] **Defense regen/rep step:** shield **passive regen** per tick; armor/hull **no** passive
        regen; a `RemoteRep(target, layer, amount)` entry point (area E logistics drives it).
  - [ ] **Tracking vs signature + optimal/falloff range** helpers (small fast target vs big slow
        gun; 1.0 in optimal, decaying through falloff to 0) — pure, fed by area-A stats.
  - [ ] **Determinism + allocator discipline:** fixed iteration order, no global-heap alloc in the
        tick (§7.2); identical result client/server so the record/replay harness (§16.2) stays green.
- **Tests (`NeuronCoreTest`; `NeuronTools/testrunner/CombatTests.cpp` mirror):**
  - [ ] Resist counter triangle holds: EM > shield-tank, Thermal > armor-tank, Kinetic weak vs
        shield (`combat-balance.md` §2.2) — right damage type out-damages wrong for equal base.
  - [ ] Damage depletes outside-in (shield then armor then hull); shield regens, armor/hull don't;
        remote rep restores the right layer.
  - [ ] Tracking reduces hits on small fast targets; falloff decays damage past optimal; the whole
        path is deterministic across two runs (`SimHash`-stable).
- **Depends on:** A, B. **Blocks:** D, E, F, M.

### D. Weapons & projectiles — local sub-stepping (§13.2, App. B)

- **Goal:** real **weapon firing** + **projectile** travel resolved with **intra-tick
  sub-stepping** so fast shots don't tunnel through targets between 33.3 ms ticks, applying damage
  via area C.
- **Masterplan refs:** §13.2 ("weapons/projectiles (local sub-stepping for fast shots)"), §13.11
  (`Projectile`), App. B (30 Hz tick — sub-stepping is *local* to the step, not a faster tick),
  §8.4/App. A (projectiles are short-lived snapshot entities), R16 (projectile count pressure).
- **Current state:** `WeaponDamage` accumulates `dps×dt` with **no projectile entity** and no hit
  geometry — instantaneous flat damage in range. `EntityKind::Projectile` reserved, unmodelled.
- **Work:**
  - [ ] **Weapon firing** (`Combat.h` + `CombatSystem`): per-weapon cadence/cooldown from area-A
        stats; in optimal/falloff range + tracking (area C) → spawn a `Projectile` (or resolve a
        hitscan beam for instant weapons) toward the target.
  - [ ] **Projectile integration with sub-stepping:** advance projectiles each tick in **N local
        sub-steps**, testing intercept against the target's swept position so a fast shot can't skip
        past it within a tick; on hit → `ApplyDamage` (C); expire on `ttl`/miss. Sub-step count is a
        **tunable** bounded to keep the step within App. B.
  - [ ] **Interest/snapshot:** projectiles replicate via the **existing M4 pipeline** (delta
        snapshot, App. A) under hard interest culling + the per-client visible cap; **measure their
        contribution to the cap** so M7 batching (R16) is evidenced, not assumed.
  - [ ] **Server-authoritative validation:** firing rate, range, and damage are server-checked
        (intents only from clients) — the §8.4 "validate everything" rule extends to weapons.
- **Tests (`NeuronCoreTest`; `NeuronTools/testrunner/ProjectileTests.cpp` mirror):**
  - [ ] A fast projectile across a target between ticks **hits** with sub-stepping (and would miss
        without it) — the anti-tunneling assertion.
  - [ ] Weapon cadence/cooldown gates fire rate; out-of-range / out-of-arc shots don't fire; hits
        route damage through area C with the right damage type.
  - [ ] Projectile spawn/expire is deterministic; projectile entities round-trip the snapshot codec.
- **Depends on:** B, C. **Blocks:** F, M.

### E. EWAR & logistics archetypes (§13.2)

- **Goal:** the **tactical archetypes** that make composition beat numbers — **EWAR** (jam / web /
  warp-disrupt / sensor-damp), **logistics** (remote shield/armor rep), and **tackle**
  (warp-disrupt that ties into §13.12 interdiction) — as module effects on the combat sim.
- **Masterplan refs:** §13.2 (EWAR/logi/tackle archetypes; "balanced fleet beats unbalanced"),
  §13.12 (warp-disrupt = interdiction, the interception/blockade PvP), R21 (interdiction is
  server-validated). **Design doc:** [`combat-balance.md`](../design/combat-balance.md) §4 (role
  counters: "tackle the logi → jam the logi → kill the logi → fleet melts").
- **Current state:** none — no EWAR/logi/tackle effects exist; `combat-balance.md` defines them.
- **Work:**
  - [ ] **Module effects** (`Combat.h`): jam (suppress target weapons for a duration), web (reduce
        target max speed), **warp-disrupt** (set a "tackled" flag preventing warp — consumed by the
        §13.12 nav state machine in `Navigation.h`), sensor-damp (cut target lock/optimal range).
        Each is a timed effect with strength/duration from area-A stats.
  - [ ] **Logistics remote rep:** a logi module restores a target ally's shield/armor via area C's
        `RemoteRep`, rate from area-A stats — the "kill the logi" loop's other half.
  - [ ] **Tackle ↔ interdiction:** warp-disrupt integrates with `Navigation.h::StepNav` so a
        tackled ship can't enter warp (the interception mechanic, R21); validated server-side.
  - [ ] **Counter web is real, not cosmetic:** effects are wired so a logi-supported fleet
        out-sustains, a jammed fighter misses, a tackled runner is held — the balance gate (M)
        depends on these being load-bearing.
- **Tests (`NeuronCoreTest`; `NeuronTools/testrunner/EwarLogiTests.cpp` mirror):**
  - [ ] Jam suppresses fire for its duration then clears; web reduces speed; sensor-damp cuts range.
  - [ ] Warp-disrupt prevents a tackled ship from warping (nav state stays Align/blocked); clears on
        expiry — interdiction works (R21).
  - [ ] Remote rep restores the targeted layer at the stat rate; a logi-backed ship survives a DPS
        stream a solo ship doesn't (the sustain loop).
- **Depends on:** B, C. **Blocks:** F, M.

### F. PvE AI combatants on the real model (§13.7, §9)

- **Goal:** make NPC AI **fight with the full combat model** — fitted hulls (area A), real
  damage/EWAR/logi, and target priority — extending M3's Patrol/Aggro/Flee/Defend so a fleet
  "clears a site" against a *real* opponent, not a flat-HP dummy. (Invasion/anomaly *content* is M7.)
- **Masterplan refs:** §13.7 (NPC AI = server ECS; patrol/aggro/flee/defend/escalate; site
  guardians), §9 (PvE NPCs are server ECS; the masterplan names `ERServer/ai/`), §13.11 (`NpcUnit`).
- **Current state:** `AiSystem`/`NextAiState` + `SpawnNpcSite`/`m_siteAlive` work against the
  **placeholder** combat; NPC stats are hardcoded; AI lives **inline in `ServerUniverse.h`**, not a
  separate `ERServer/ai/` (a §9 naming note, not a blocker).
- **Work:**
  - [ ] **NPC fits from the catalog:** spawn guardians/combatants with **layered HP + a fit** (area
        A/B) instead of `kNpcHp`/`kNpcDps`; difficulty scales by which fit/tier is spawned.
  - [ ] **Combat-aware behavior:** extend `NextAiState` so NPCs use weapons/EWAR/logi (D/E),
        **focus-fire** and **primary the logi/EWAR** (the `combat-balance.md` §4 target-priority the
        balance gate rewards), and flee/retreat on the layered-HP danger threshold (hull, not a flat
        bar).
  - [ ] **Site substrate for M7:** keep site/guardian spawning **data-driven and snapshot-shaped**
        so M7's invasion/anomaly **event director** (§26) schedules them without reworking the AI.
        (Optionally relocate the AI to `ERServer/ai/` per §9 — note if kept in `ServerUniverse.h`.)
  - [ ] **Server-authoritative + deterministic:** AI runs in the tick under the §7.2 rules; bots
        (client sessions) remain distinct from NPCs (server AI) per §10.3.
- **Tests (`NeuronCoreTest`/`ERServerTest`; `NeuronTools/testrunner/FleetTests.cpp` extends the 6
  existing NPC cases):**
  - [ ] A fitted NPC takes damage-type-correct damage and dies through three layers (not a flat bar);
        an NPC logi keeps its wing alive until primaried.
  - [ ] NPC target priority picks the logi/EWAR first; flee triggers on the hull threshold; a player
        fleet clears a fitted site (the M3 gate, now against real combat).
- **Depends on:** B, C, D, E. **Blocks:** M.

### G. Loot-on-kill & base disable-not-destroy (§13.1, §13.2, §15)

- **Goal:** the two destruction outcomes — a destroyed **ship** drops a recoverable
  **`LootContainer`** (an **economy event**, zero-loss via the M5 outbox) and logs a **killmail**;
  a **base** is **disabled, not destroyed** — forced emergency jump/retreat at low hull (cooldown +
  cargo loss), never deleted.
- **Masterplan refs:** §13.1 (base = capital, disable-not-destroy; emergency jump + cargo loss),
  §13.2 (loot-on-kill = economy event), §15 (loot/kills are **write-through/outbox, zero-loss**;
  `LootContainers`/`KillmailLog` rows), §24 (killmail notification — stored, surfaced), R12.
  **Design doc:** [`combat-balance.md`](../design/combat-balance.md) §1 goal 5 (disable-not-destroy).
- **Current state:** `CombatSystem` destroys a unit on 0 HP with **no loot, no killmail**
  (*"loot-on-kill is M6"*); base destruction has no disable-not-destroy path. Schema
  `LootContainers`/`LootContainerItems`/`KillmailLog`, `Bases.BaseState`/`RetreatUntil` exist.
- **Work:**
  - [ ] **Loot-on-kill:** on ship death, spawn a `LootContainer` (area B) with a **fraction of
        fit/cargo**; recovery transfers items to the looter's cargo. Each loot drop + claim is an
        **economy event → the M5 write-through outbox** (zero-loss) and writes
        `LootContainers`/`LootContainerItems`.
  - [ ] **Killmail:** write a `KillmailLog` row (victim/killer/hull/region/value) on every kill —
        an economy event (outbox); surface as an **offline notification** (§24, `Notifications`).
  - [ ] **Base disable-not-destroy** (§13.1): at low **hull**, force the base into
        **emergency jump/retreat** — set `BaseState=retreating`, apply **cargo loss**, start the
        `RetreatUntil` cooldown via the §13.12 jump path, then `disabled`; the base is **never
        removed**. Persist the state (M5 write-behind for HP, outbox for the cargo-loss economy
        event).
  - [ ] **Audio/UI hook:** raise the **base low-hull → retreat alert** event (area H consumes it,
        §11.3).
- **Tests (`ERServerTest` + `NeuronCoreTest`; testrunner mirror for the pure loot/kill rules):**
  - [ ] Ship death spawns a loot container with the expected fit/cargo fraction; claiming moves the
        items and **emits one outbox economy event** (zero-loss, idempotent on replay — M5 area D).
  - [ ] A kill writes exactly one `KillmailLog` row + a notification.
  - [ ] A base driven to low hull **retreats and survives** (state → retreating → disabled, cargo
        lost, cooldown set) and is **never destroyed**; state restores across a warm restart (M5 F).
- **Depends on:** B, C, D. **Blocks:** M. **Leans on:** M5 D (outbox), F (warm-restart), §13.12 nav.

### H. Combat VFX & SFX — client feedback (§11.2, §11.3)

- **Goal:** make combat **read and feel** on the client — weapon tracers/muzzle, impacts,
  **explosions**, and the combat SFX set — all driven by **replicated sim events** (no audio/VFX
  on the wire, no determinism requirement, §11.3).
- **Masterplan refs:** §11.2 (GPU-compute additive particles: thrusters, **weapon tracers/muzzle,
  impacts, explosions**, mining beams), §11.3 (event SFX: weapons fire/impacts,
  **shield/armor/hull hits**, explosions, **base low-hull → retreat alert**; UI alerts), §10.1
  (client reacts to replica/interp events). **Design doc:** [`combat-balance.md`](../design/combat-balance.md)
  §1 goal 4 (roles visually distinct / readable at RTS scale).
- **Current state:** M2 stood up the GPU-particle system + the four audio buses + event-SFX hooks;
  the **combat-specific** cues are unfilled (combat was a placeholder at M3).
- **Work:**
  - [ ] **Weapon/impact/explosion VFX** (`NeuronRender`): additive particle effects for tracers/
        muzzle, layer-hit impacts (shield vs armor vs hull read differently), and ship/explosion
        FX, emitted from replicated combat events; within the App. B particle budget + distance LOD
        (R16).
  - [ ] **Combat SFX** (`NeuronAudio`): 3D-positioned weapons/impacts/explosions on the **SFX bus**,
        the **base low-hull retreat alert** (area G event), and **UI alerts** on the UI bus —
        listener = camera (§11.3), **no `int64` reaches audio** (R2).
  - [ ] **Event → cue mapping:** drive both from the NeuronClient replica/interp layer (a
        fire/hit/death/retreat event → VFX + SFX), **client-side only** (§11.3: no determinism, no
        wire audio).
- **Tests (`NeuronRenderTest` + `NeuronAudioTest`, §16.1; logic mirrors where platform-independent):**
  - [ ] A replicated fire/hit/death/retreat event maps to the right VFX + SFX cue (event→cue table).
  - [ ] Particle/voice counts stay within the App. B budget under a scripted big-fight event stream
        (R16); ERHeadless still builds/runs with **no audio** (§11.3).
- **Depends on:** C, D, G (the events to react to). **Blocks:** M (the playable feel).

### I. Optional own-unit/fleet prediction — decided by feel (§10.1, §19)

- **Goal:** resolve the standing §19 question *"whether any subsystem needs own-unit/fleet
  prediction (decided by feel at M6)"* — **measure** direct-control feel under live RTT, then add a
  **narrow** `NeuronClient/predict/` reconcile path **only where feel demands it**, or **document
  "interpolation suffices."**
- **Masterplan refs:** §10.1 (a `predict/` module added **post-M1 only where input feel requires
  it** — fast direct control; slow command-driven units stay interpolation-only), §8.4 (own units
  server-confirmed + snap-on-ack until then), §19 (the feel decision), R8 (prediction deliberately
  deferred to bound scope — **keep it narrow**).
- **Current state:** `Interpolator.h` is **snap-on-ack only**; no `predict/` module
  (`IClientController.h` says "post-M1").
- **Work:**
  - [ ] **Feel measurement first:** under representative RTT/loss, assess own-base/own-fleet move
        responsiveness with interpolation + snap-on-ack. **Gate the rest of I on this** — if it
        feels fine, write the decision down and **stop** (the cheapest correct outcome).
  - [ ] **If needed — narrow predict/reconcile** (`NeuronClient/predict/`): client-side predict own
        **movement** from issued intents, reconcile against the server snapshot on ack (replay
        unacked intents). **Own units only**; remote entities stay interpolation; slow
        command-driven units stay interpolation (§10.1). No prediction of combat outcomes (server
        authority is absolute).
  - [ ] **Shared prediction rule = the sim rule:** reuse `Movement.h`/`Navigation.h` (§7.2) so
        client prediction and server authority compute identically (no divergence by construction).
- **Tests (`NeuronClientTest`; mirror the reconcile math on `testrunner` where platform-independent):**
  - [ ] *(If built)* Predicted own-unit position reconciles to the server state on ack with no
        visible snap under nominal RTT; a mis-prediction converges within bounded ticks.
  - [ ] *(If not built)* A recorded decision note + a snap-on-ack regression test that own-unit feel
        meets the bar without prediction.
- **Depends on:** nothing (client-only; needs a live client to judge feel). **Blocks:** nothing —
  explicitly optional; **must not expand scope** (R8).

### J. Azure SQL migration (managed identity / Entra ID) (§15, §20, §14)

- **Goal:** migrate the persistence layer from M5's **self-hosted SQL Server + SQL login** to
  **Azure SQL** — a **connection-string + auth change** (to **managed identity / Entra ID**), not a
  rewrite — leveraging the Azure-SQL-compatibility M5 already enforced.
- **Masterplan refs:** §15 (self-hosted → Azure SQL = connection-string + auth change; **app→DB
  auth: SQL login now → managed identity / Entra ID**; avoid cross-DB/SQL Agent/FILESTREAM), §20
  (Azure SQL via private endpoint/firewall; connection string via a K8s Secret), §14 (Entra ID for
  app→DB), R4 (DB latency — co-locate), R7 (Azure parity). **Built on:** M5 areas A/B (ODBC layer +
  schema).
- **Current state:** M5's ODBC layer (`ERServer/persist/`) connects to **self-hosted SQL over TCP
  1433, `Encrypt=yes`, SQL login**; every statement was kept Azure-SQL-compatible. No Azure SQL
  target, no managed-identity auth path.
- **Work:**
  - [ ] **Managed-identity auth path** in `ERServer/persist/`: support an **Entra ID / managed
        identity** ODBC connection (Driver 18 `Authentication=ActiveDirectoryMsi`/equivalent)
        alongside the existing SQL-login path, selected by config — credentials from the
        environment/Secret (§20), never hard-coded.
  - [ ] **Connection string → Azure SQL** (private endpoint/firewall), supplied via the K8s Secret
        (area K); `Encrypt=yes` (mandatory on Azure SQL).
  - [ ] **Parity verification:** run the M5 schema + migrations against a **dev Azure SQL** instance
        and the full M5 persistence test suite (areas A–I) — confirm **no parity break** (the §15
        guardrails held). Fix any surfaced incompatibility **in a forward migration**, never by
        editing an applied one.
  - [ ] **Co-location note** (R4): document the ERServer↔Azure SQL region pairing so DB latency
        stays out of the tick budget (§9).
- **Tests (`ERServerTest` against dev Azure SQL on the Windows agent, §16.3):**
  - [ ] The managed-identity connection authenticates and round-trips a parameterized insert+select.
  - [ ] M5 migrations apply clean empty→current on Azure SQL; the M5 persistence suite passes
        unchanged (parity proven).
- **Depends on:** M5 A/B (ODBC layer + schema). **Blocks:** K (the prod pod talks to Azure SQL), M.

### K. Kubernetes production deployment — Windows nodes + UDP LB (§20)

- **Goal:** stand up the **prod topology** — ERServer as a **Windows container** on a **Windows
  node pool**, fronted by a **UDP-capable LoadBalancer**, **one pod per shard** with **client→pod
  session affinity**, config/secrets from K8s, stateless → **safe rolling restarts** (M5
  warm-restart + reconnect).
- **Masterplan refs:** §20 (Windows node pool; **UDP LoadBalancer** — HTTP ingress won't route UDP;
  one pod per shard + client→pod affinity; Azure SQL via Secret; stateless → rolling restarts), §26
  (rolling restarts rely on warm-restart; clients reconnect with backoff/jitter), R5 (K8s + Windows
  + UDP). **Built on:** the existing `Config/deploy/Dockerfile`, M5 F (warm-restart) + G (reconnect).
- **Current state:** `Config/deploy/Dockerfile` (Windows Server Core, ODBC 18, UDP 7777) +
  `docker-compose.dev.yml` exist. **No K8s manifests, UDP LB, node-pool affinity, Secret, or
  ConfigMap.**
- **Work:**
  - [ ] **K8s manifests** (`Config/deploy/k8s/`): a Deployment for the ERServer pod with a
        **Windows `nodeSelector`/affinity** (tag-matched to the host build), resource requests, and
        liveness/readiness probes.
  - [ ] **UDP LoadBalancer:** a `Service type: LoadBalancer` with **`protocol: UDP`** (or
        NodePort / cloud UDP LB) exposing the reliable-UDP port; **client→pod session affinity** so
        a connection stays on its shard pod.
  - [ ] **Config + secrets:** a **Secret** for the Azure SQL connection string (area J) +
        `ER_SERVER_PEPPER`, a ConfigMap for non-secret tunables (`ER_LISTEN_PORT`, game-data
        version) — the Dockerfile already reads these envs.
  - [ ] **Rolling-restart on the cluster:** prove the §26 SLA on K8s — restart the pod
        (warm-restart, M5 F), clients reconnect with backoff/jitter (M5 G), economy zero-loss; a
        brief blip is acceptable. **One shard = one pod** (the matchmaking/directory service for
        multiple shards is post-launch, §19).
- **Tests / verification (Windows-node cluster on the agent, or a documented dry-run, §16.3):**
  - [ ] A pod schedules on a Windows node and serves reliable-UDP through the UDP LB end-to-end (a
        bot connects through the Service, not pod-direct).
  - [ ] A rolling restart of the pod → bots reconnect (M5 G) and resume with **zero economy loss**
        (the M5 drill, now on the cluster) — the prod-topology half of the Done gate.
- **Depends on:** J (Azure SQL target), M5 F/G (warm-restart + reconnect). **Blocks:** M (playable
  on the prod topology).

### L. Store-compliance pass (§25, R1)

- **Goal:** get the UWP client **Store-shippable** — capability review, **Windows App
  Certification Kit (WACK)** pass, the protocol-version-gate "update required" UX, crash/telemetry
  per §25, and a final **non-loopback-exempt** run (first validated at M1b).
- **Masterplan refs:** §25 (updates via MSIX/Store; **protocol-version gate** → "update required";
  crash reporting/minidump/DRED; settings in UWP local storage), §8.1/§8.5 (loopback-exempt only
  for dev; non-exempt before Store; version gate), R1 (test the Store path early; UWP kept for
  Store reach). **Built on:** M1b's loopback/non-exempt validation, M2's settings screen.
- **Current state:** `EarthRise/Package.appxmanifest` exists (self-signed `CN=Zwaliebaba`,
  `internetClient` + `privateNetworkClientServer`, landscape). No WACK pass, no
  "update required" UX, no crash-reporting wiring confirmed.
- **Work:**
  - [ ] **Capability minimization:** keep only what ships needs (`internetClient`); justify or drop
        `privateNetworkClientServer` (a dev/LAN capability that can fail Store cert) — the released
        client must run **without** a loopback exemption.
  - [ ] **WACK pass:** run the Windows App Certification Kit on the Release MSIX; fix flagged items;
        confirm no Debug-only deps ship (e.g. **PIX runtime is Debug-only**, §11.1 — verify it's
        absent from Release/Store).
  - [ ] **"Update required" UX:** wire the §8.5 **protocol-version gate** rejection to a clear
        client message (R1) so a mismatched build can't silently fail.
  - [ ] **Crash + settings per §25:** confirm minidump + rolling logs + DRED capture (opt-in
        upload) and that settings (graphics, **audio bus volumes**, keybinds, HUD scale,
        accessibility, language) persist to **UWP local storage**, not SQL.
  - [ ] **Non-exempt run:** validate the full login→play path with **no loopback exemption** (the
        Store reality), end-to-end against the prod-like server.
- **Tests (`EarthRiseTest` / `NeuronClientTest`; manual WACK on the Windows agent, §16.3):**
  - [ ] The version-gate rejection surfaces the "update required" message (a stale-protocol client
        is cleanly refused, §8.5).
  - [ ] Settings round-trip through UWP local storage; defaults + reset work (§25).
  - [ ] WACK pass recorded; the non-exempt login→play smoke run succeeds.
- **Depends on:** nothing structural (client packaging) — best run **after** the combat client
  (H) so the certified build is the playable one. **Blocks:** the "Store-compliance pass" clause.

### M. Balance gates + prod-topology integration (the Done gate) (§17, R15)

- **Goal:** the end-to-end milestone proof — **balanced fleet-vs-fleet fights where fit &
  composition beat raw numbers** (the `combat-balance.md` §7 automated gates), **playable on the
  prod topology** (Azure SQL + K8s, J/K), with combat events persisting **zero-loss** (G via M5).
- **Masterplan refs:** §17 M6 Done, §10.3 (ERHeadless many sessions; bots ≠ NPCs), §16.1/§16.2/§16.3
  (load/balance harness + perf gates), App. B (tick budget; **fleet cap 6–12**; projectile/particle
  pressure under R16), R15 (validate the *fun*/depth with bots before M7 polish), R16 (measure where
  the visible-entity cap binds). **Design doc:** [`combat-balance.md`](../design/combat-balance.md)
  §6 (the "should-win" comp) + §7 (the gate matrix).
- **Current state:** M3 area H drives a handful of bots through the loop with `SimHash` record/replay;
  `FleetTests.cpp` has 6 NPC cases against placeholder combat. M6 adds **balance matchups** on the
  real model and runs them on the **deployed** stack.
- **Work:**
  - [ ] **Automated balance gates** (`ERHeadlessTest` + `testrunner` sim half), per
        `combat-balance.md` §7 — for each matchup, neither side wins outside the stated band over N
        sims, else **flag for tuning** (the area-A data, not code):
        - mixed comp vs **mono-DPS** equal-cost → **mixed wins, ≤ 80%** (Goal #1, composition).
        - **right-damage-type T1 vs wrong-type T2** equal-cost → **T1 competitive, ≥ 45%** (Goal #2,
          fit > tier; R17 catch-up).
        - **swarm-of-Light vs single Heavy** equal-cost → **~50/50** (size rock-paper-scissors).
        - **with-logi vs without-logi** equal ships → **with-logi wins, ≤ 70%** (the sustain loop).
  - [ ] **Fleet-cap sweep** (6/8/12, §19/App. B): confirm RTS readability + the App. B entity/
        bandwidth budget hold; record **where projectiles + ships pressure the per-client visible
        cap** (R16 evidence for M7 aggregation).
  - [ ] **Tune the data, not the code:** iterate area-A tunables until the gates pass (hot-reload,
        §26) — *the balance contract is met by data.*
  - [ ] **Playable on the prod topology:** run a fleet-vs-fleet session with bots **through the K8s
        UDP LB against Azure SQL** (J/K), with loot/kills going **write-through, zero-loss** (G/M5),
        surviving a rolling restart (K) — the literal Done clause "playable on the prod topology."
- **Tests (`ERHeadlessTest` on the Windows agent; `testrunner` mirror for the balance sim half):**
  - [ ] All four balance matchups land inside their bands over N sims (deterministic seeds);
        a deliberately mis-tuned datum trips the gate (the gate actually bites).
  - [ ] A bot fleet-vs-fleet fight runs end-to-end on the deployed prod stack (UDP LB → pod → Azure
        SQL), economy zero-loss across a rolling restart.
- **Depends on:** A–H, J, K (and L for the certified client). **Blocks:** Done gate (it *is* the
  end-to-end verification).

---

## Suggested order / dependency notes

> Two largely independent tracks plus a gate. The **combat track** (A→B→C→{D,E}→F→G, with H/I on
> the client) is the milestone's core and is mostly sequential. The **deployment track** (J→K, plus
> L) is independent of the combat code and can run in parallel. **M** is the end-to-end gate and
> depends on both.

1. **A (combat catalog as game data)** first — every combat path reads it; no balance literal in code.
2. **B (layered defense + fitting ECS)** → **C (damage/defense sim rules)** in order — the model and
   its math. Then **D (weapons/projectiles)** and **E (EWAR/logi)** in **parallel** (both build on C).
3. **F (PvE AI on the real model)** once B–E exist — NPCs fight with the full model.
4. **G (loot-on-kill + disable-not-destroy)** once B–D exist — leans on M5's outbox + warm-restart.
5. **H (combat VFX/SFX)** as C/D/G land — client feel; **I (optional prediction)** is feel-gated and
   **must stay narrow** (R8) — measure first, build only if needed.
6. **J (Azure SQL) → K (K8s prod)** run **in parallel** with the combat track; **L (Store pass)**
   after the combat client (H) so the certified build is the playable one.
7. **M (balance gates + prod integration)** last — the fit-beats-numbers gate, run on the deployed
   prod topology, that closes M6.

**Cross-milestone:** M6 **consumes** M5 (outbox for loot/kills, warm-restart for transient combat
state, reconnect for K8s rolling restarts) and the M4 interest pipeline (projectiles replicate
through it). M6 **builds the substrate** M7 consumes — the combat model under conquest/PvP, the AI
under invasions/anomalies, the catalog under the fitting/market/research UI. **Aggregation/LOD +
projectile batching (R16)** stay **M7**; M6 holds App. B with M4 culling + the visible cap and
**measures where it binds**.

## Done gate (mirrors §17 "Done")

- [ ] **Role + fitting + damage-type combat** — three layers (shield/armor/hull) + per-type resists,
      a module fitting grid with PG/CPU budget, the counter triangle live (A, B, C).
- [ ] **Weapons/projectiles with local sub-stepping** — fast shots hit, no tunneling, server-
      authoritative (D).
- [ ] **EWAR/logistics archetypes** load-bearing — jam/web/tackle/sensor-damp + remote rep make the
      "kill the logi" loop real (E).
- [ ] **PvE AI** fights with the real model and target priority; a fleet clears a fitted site (F).
- [ ] **Loot-on-kill + base disable-not-destroy** — ships drop recoverable loot (zero-loss economy
      event), kills logged; the base retreats and is **never destroyed** (G, via M5 D/F).
- [ ] **Combat VFX/SFX** read at RTS scale within the App. B particle/voice budget; ERHeadless still
      runs with no audio (H).
- [ ] **Prediction decision recorded** — narrow own-unit predict/reconcile **only if feel needs it**,
      else a documented "interpolation suffices" (I).
- [ ] **Azure SQL migration** — managed-identity connection, M5 schema/suite pass on Azure SQL with
      no parity break (J).
- [ ] **Kubernetes prod topology** — Windows-node pod behind a UDP LB with client→pod affinity;
      rolling restart → reconnect → zero economy loss (K, via M5 F/G).
- [ ] **Store-compliance pass** — WACK pass, minimal capabilities, version-gate "update required"
      UX, non-loopback-exempt login→play (L).
- [ ] **Balanced fleet-vs-fleet fights where fit & composition beat raw numbers** — all four
      `combat-balance.md` §7 gates pass via data tuning, **playable on the prod topology** (M).
- [ ] All matching `<project>Test` suites green (§16.1) + Linux `testrunner` mirrors for the
      platform-independent combat/balance logic (§16.2); perf gates met where applicable (§16.3,
      App. B).
