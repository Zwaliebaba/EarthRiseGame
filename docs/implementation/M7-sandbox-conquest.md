# M7 — Sandbox: Conquest, Economy, PvE Content & Onboarding (Implementation Plan)

> Derived from [`../masterplan.md`](../masterplan.md) §17 (milestone **M7**).
> **Status:** ⏳ Not started — **drafted ahead** (M6 is the next active plan; per
> [`README.md`](README.md) rule 2, M7 is normally not drafted until it is next — this is an
> explicit forward-look). It is **subordinate to the masterplan** and must be
> **re-confirmed against it when M7 goes active**: M6's actual landing (the combat model, the
> AI substrate, the catalog shape, the Azure SQL + K8s topology) will shift details here.
> **Plan style:** feature-area sections (see [`README.md`](README.md)).
> **Verification (two homes, same as M6).** The **sandbox-rules half** (security enforcement,
> crafting/refining, research, markets/insurance, conquest timers, anomaly/invasion generation,
> aggregation) is platform-independent shared-sim logic → it lands with `NeuronCoreTest` +
> Linux `testrunner` mirrors (§16.1/§16.2), extending the M3 economy/universe placeholders and
> the M6 combat model. The **persistence half** (markets/insurance/conquest/mail as economy
> events through the M5 outbox, plus territory/mail/notification SQL state) runs on the
> **Windows ERServer agent against dev/Azure SQL**. The **client half** (the full UI suite,
> mail/notifications UI, touch scheme) is Windows-agent / real-client and verified there. The
> **end-to-end gate** (N) runs on **ERHeadless** (bots) + a manual client pass. Assumes **M6**
> (combat model, PvE AI on the real model, loot/kill economy events, Azure SQL + K8s prod
> topology) is **closed** when M7 starts.

## Milestone goal (verbatim from §17)

> **M7 — Sandbox: conquest, economy, PvE content & onboarding** *(L–XL)* —
> **tiered security (high→low→null)**; **territorial conquest** (claimable structures,
> capture/hold timers, upkeep/yield, ownership persistence); **player crafting economy**
> (refine→components→build) + **regional markets** + currency sinks + **ship insurance**;
> **dynamic faction invasions** + **procedural anomalies/expeditions**; **protected starter
> onboarding** + objective chain; retention loop; **full UI suite** (fitting / market /
> research / inventory / territory), **in-game mail + notifications** (§24), **touch control
> scheme** (§23). **Interest at scale:** **entity aggregation/LOD is a committed feature here,
> not an optional lever** — at hundreds of players a contested sector *must* send distant
> fleets as a **cluster, not N ships** (plus projectile batching) to hold App. B; validated for
> contested sectors (App. B, R16, R24).
> **Done:** a full sandbox session — onboard in high-sec, build & fit a fleet, run anomalies,
> trade, push into null, and **claim & hold territory** through a contested fight — playable
> end-to-end by players + bots (mouse+keyboard and touch), with zero economy loss across a
> server restart.

## Scope at a glance

- **In scope:** turning the **mechanisms** built in M1–M6 (networked sim, scale/interest,
  persistence/auth, combat) into a **playable sandbox** — the §13 game design, end to end:
  the **risk→reward security gradient** (§13.5) enforced server-side; the **player crafting
  economy** (refine→components→assemble, `economy-crafting.md`) so destruction creates demand;
  **regional markets + currency sinks + ship insurance** (§13.4/§13.9) on the M5 outbox;
  **research/tech-tree progression** (§13.3, `tech-tree.md`) with the datacore faucet;
  **territorial conquest** (claimable structures, capture/hold timers, upkeep/yield, persisted
  ownership, §13.6); first-class **PvE content** — **procedural anomalies/expeditions** and
  **dynamic faction invasions** driven by the §26 **event director** on the universe clock;
  **new-player onboarding** (protected high-sec start + objective chain) and the §13.10
  **retention loop**; the **full UI suite** (fitting / market / research / inventory / starmap /
  territory, §22.1) + **in-game mail & notifications** (§24) + the **touch control scheme**
  (§23); and the **committed scale feature** — **entity aggregation/LOD (fleet-as-cluster) +
  projectile batching** (R16, App. B). Gated by an **end-to-end ERHeadless + client sandbox
  session** (N) with **zero economy loss across a restart**.
- **Out of scope (post-launch, §19):**
  - **Persistent corporations / alliances + shared wallet/diplomacy** — launch is **light
    parties/fleets with individual ownership** (§13.8). R18 flags this as the **likeliest first
    social expansion** (promote forward only if conquest/retention demands it); M7 builds
    **individual** territory ownership (§13.6), not corp ownership.
  - **Multi-shard topology + directory/matchmaking** — the post-launch **capacity** lever
    (§19). M7 is the single-shard sandbox; aggregation/LOD + time dilation (M4) are the
    in-shard levers, multi-shard is not built here.
  - **Time-gated skill training**, **universe bosses / capital PvE**, **escalating-territory-
    heat**, **active combat abilities** (overheat/EWAR burst), **seasons/leaderboards**,
    **gamepad**, **multi-language localization**, **federated Entra ID *user* login** — all
    tracked post-launch (§19). M7 ships English (localization-ready, §22.4) + custom auth (M5).
  - **Explosive 4th damage type** — data-only when wanted (M6 area A made the set data-driven).
- **Open questions that touch M7** (from §19 + the design docs — resolve *before* the work item
  each blocks; most are **balance dials tuned with bots**, not structural blockers):
  - **Promote persistent corporations forward?** (R18, §19) — the one question that could
    **change F's ownership model**. Default: **individual ownership at launch**; decide *before*
    F (conquest) if a closed playtest shows solo territory defense is unworkable. *Blocks F's
    ownership shape; nothing else.*
  - **Market model — regional hubs vs fewer trade centers; order-matching at scale** (§19,
    `economy-crafting.md` §9). Pick **per-region order books** (the §13.4 default); tune hub
    count as data. *Blocks E's structure choice, tunable after.*
  - **Economy balance** — resource scarcity by tier, faucet/sink rates, **insurance payout %**
    (the master risk dial, `economy-crafting.md` §9 / `combat-balance.md` §7). All **game data**
    (area A), swept by the N sandbox gate + telemetry (§21). *Blocks tuning, not structure.*
  - **Onboarding objective-chain scope** (§19) — partly **already prototyped** in
    [`playable-slice.md`](playable-slice.md) (`Onboarding.h`). M7 extends it to the full loop.
    *Drives J; blocks nothing.*
  - **Anomaly epoch granularity / region layout authored-vs-generated / beacon topology**
    (`universe-worldgen.md` §9) — **authored regions + seed-generated contents** is the launch
    recommendation; the cooked format is identical either way so the source can switch later.
    *Drives A/G/H tunables.*
  - **Mail/notification volume & anti-spam** (rate-limits/blocklists) at scale (§19, §24).
    *Drives L's abuse controls; pick first-pass limits.*
  - **Blueprints account-permanent vs per-base** (`tech-tree.md` §6) — recommend
    **account-permanent** for retention. *Decide in D (research persistence).*

## Current state (what M1a–M6 left us)

> File-level baseline. Where M7 replaces an M3/M6 placeholder or extends a stub, it's named.
> Re-run the Explore pass when M7 goes active — M6's landing moves several of these.

- **The 4X eXploit loop exists at M3 scale, hand-seeded.** `NeuronCore/Economy.h` +
  `HarvestSystem` drive harvest → return → deposit → build-a-ship against cooked `EconomyTuning`;
  `Navigation.h` does warp/jump on the cooked beacon graph. The **crafting chain is a single
  step** (raw → ship) — the **refine → components → assemble** stages of `economy-crafting.md`
  are **not** modelled yet. **No markets, no currency, no insurance, no research** exist.
- **The universe is the M3 static slice.** `datacook`/`datacheck` + `NeuronCore/UniverseData.h`
  cook **regions + a public beacon graph + resource fields** (`Config/universe/sol-frontier.universe`);
  `ServerUniverse` loads them and spawns nodes/beacons. **Security tiers are authored but not
  *enforced*** (no high-sec PvP block, no claim rules). Anomalies/invasions are **one hand-placed
  NPC site** — the **procedural + event-director** half (`universe-worldgen.md` §4.4/§5) is M7.
  `TerritoryStructure`/`AnomalySite`/`InvasionEvent` are §13.11 entities **not yet modelled**.
- **Combat + AI are real (M6).** The layered defense/fitting model, weapons/projectiles, EWAR/
  logi, **PvE AI that fights with real fits**, and **loot-on-kill + base disable-not-destroy**
  land in M6. M7 **schedules** that AI as invasions/anomalies and **consumes** the loot/kill
  economy events — it does **not** rebuild combat. The **combat catalog as game data** (M6 area
  A) is the catalog M7 extends with recipes/research/site templates (area A).
- **Persistence + the outbox are real (M5).** The **write-through outbox + append-only ledger**
  (M5 area D) is the zero-loss mechanism every M7 economy event rides (crafting completion,
  trades, insurance payouts, conquest captures, killmails). The **schema already provisions M7
  tables** (M5 area B + migrations): wallet + currency ledger, `MarketOrders`/`MarketTrades`,
  `InsuranceContracts`, research/unlocked-blueprints, `TerritoryStructures` + capture log,
  `Mail`/`Notifications` (migration 003). **Stats/recipes/research costs/site defs are game
  data, not SQL** (§15 catalog/balance boundary). **Warm-restart snapshot + log** (M5 area F)
  is what the N gate's zero-economy-loss-across-restart clause exercises.
- **Scale/interest is real (M4).** Cell pub/sub interest, per-entity version stamping, quantized
  delta snapshots, tombstone eviction, the snapshot job pool, and **time dilation** land in M4.
  M4/M6 **measured where the per-client visible cap binds** under contested fights — M7 area I
  cashes that evidence in with **fleet-as-cluster aggregation + projectile batching** (R16).
- **The client is interpolation + a forward-pulled slice.** M1b/M2 stood up the DX12 Scene/
  Canvas split, the **Darwinia windowed-menu/options toolkit** (§22.6, `NeuronRender`), the radar/
  overview HUD basics, and settings. [`playable-slice.md`](playable-slice.md) **already prototyped**
  (Linux-tested, Windows-smoke pending) the **free RTS camera** (`RtsCamera.h`), an **onboarding
  objective chain** (`Onboarding.h`), **viewport pick/box-select** (`Picking.h`), and **in-world
  selection/health feedback** (`HudOverlay.h`). M7's **full UI suite** (area K) builds on that
  toolkit; M7's onboarding (area J) extends `Onboarding.h`. **No fitting/market/research/
  inventory/territory screens, no mail UI, no touch scheme exist yet.**

---

## Feature areas

### A. Game-data catalog expansion — recipes · research · region map · claimable beacons · site templates (§12.6, §13.3–13.7, §15)

- **Goal:** extend the cooked game-data catalog (M3 universe layout + M6 combat catalog) with
  every **M7 sandbox dataset** — **crafting recipes** (the 4-stage chain), the **research tree**,
  the **full region/security map**, **claimable** beacon/structure defs, and **anomaly/invasion
  templates** — so all M7 balance/content is **data, not code** (§15 catalog/balance boundary).
- **Masterplan refs:** §12.6 (text source → versioned binary serde; `datacook`/`datacheck`),
  §13.3 (research), §13.4 (recipes/markets), §13.5–13.6 (regions/territory), §13.7 (PvE
  templates). **Design docs:** [`economy-crafting.md`](../design/economy-crafting.md) §2–6,
  [`tech-tree.md`](../design/tech-tree.md) §2–5, [`universe-worldgen.md`](../design/universe-worldgen.md)
  §4 (region/beacon/field/anomaly schemas).
- **Current state:** `NeuronCore/UniverseData.h` + `NeuronTools/datacook/` cook regions/beacons/
  fields + (M6) the combat catalog; `Config/universe/sol-frontier.universe` is the first dataset.
  No recipe/research/anomaly-template datasets; the region map is the small M3 slice.
- **Work:**
  - [ ] **Crafting-recipe dataset** (refine yields, component recipes, product recipes keyed to
        `ItemDefs` codes) per `economy-crafting.md` §2–5 — inputs/outputs/quantities/tech gate.
  - [ ] **Research-tree dataset** (5 branches × T1→T2→T3, datacore + resource cost, cross-branch
        prereqs) per `tech-tree.md` §2–5; each node unlocks a blueprint id.
  - [ ] **Full region/security map** + the **claimable** beacon/structure layer (`kind = claimable`
        only in low/null per `universe-worldgen.md` §4.1/§4.2) — extends the M3 region slice.
  - [ ] **Anomaly + invasion templates** (`universe-worldgen.md` §4.4): difficulty tier, guardian
        composition (M6 fits), loot table, spawn weight by security, escalation script.
  - [ ] **Extend `datacheck`:** recipe inputs/outputs resolve to real `ItemDefs`; **research
        prereqs are acyclic** (§12.6); claimable beacons only in low/null; site loot/guardian ids
        resolve; reciprocal/connected beacon graph. Build-time failure on a bad row.
  - [ ] **Hot-reload** the new datasets in the running ERServer (§26 live-ops) so economy/PvE
        pacing tunes without a redeploy (area N tuning leans on this).
- **Tests (`NeuronCoreTest`; `NeuronTools/testrunner/UniverseDataTests.cpp` + new
  `RecipeDataTests.cpp`/`ResearchDataTests.cpp` mirrors, §16.2):**
  - [ ] Each dataset cook→load round-trips; a malformed recipe (dangling id, negative qty), a
        **cyclic research prereq**, and a high-sec `claimable` beacon are each **rejected**.
  - [ ] A product recipe resolves through refine→component→product to real raw `ItemDefs`; a
        research node's prereqs form a DAG reachable from Core (`tech-tree.md` §2).
- **Depends on:** M6 area A (combat catalog). **Blocks:** C, D, E, F, G, H (every system reads it).

### B. Tiered-security enforcement — high → low → null (§13.5)

- **Goal:** make security tiers **load-bearing rules**, not just authored labels — **high-sec PvP
  off** (NPC-enforced), **low-sec PvP on / no claims**, **null PvP + claimable** — plus the
  retained **base safe-zone bubble** (§13.5), all server-authoritative.
- **Masterplan refs:** §13.5 (the tier table; security is a region/sector property enforced
  server-side; safe-zone emitter retained as a per-base defensive bubble), §13.8 (standings drive
  friendly-fire), §8.4 (server validates all damage intents). **Design doc:**
  [`universe-worldgen.md`](../design/universe-worldgen.md) §4.1 (region security data).
- **Current state:** regions carry a `security` tag in cooked data but **nothing enforces it** —
  M6 combat applies damage regardless of tier; no high-sec aggressor response, no claim gate.
- **Work:**
  - [ ] **Security lookup** (`NeuronCore`): `SecurityTierAt(UniversePos)` from the cooked region
        map — a pure, fast sector→tier query the combat/claim systems consult.
  - [ ] **High-sec PvP block + response** (the M6 `CombatSystem` damage path): reject player-vs-
        player damage intents in high-sec; an aggressor is flagged and **disabled by an NPC
        response fleet** (a scripted spawn on the M6 AI substrate). Low/null: PvP allowed.
  - [ ] **Base safe-zone bubble** (§13.5): no-PvP inside the bubble in high-sec; a **defensive
        bonus** (resist/regen) in low/null — a per-base radius effect on the combat sim.
  - [ ] **Claim gate** (feeds F): claiming is **server-rejected outside null-sec**.
  - [ ] **Standings/IFF** (§13.8): per-player/party ally/neutral/hostile drive friendly-fire so
        the §22.3 IFF and the engagement rules agree.
- **Tests (`NeuronCoreTest`/`ERServerTest`; `testrunner/SecurityTests.cpp` mirror):**
  - [ ] A PvP damage intent is **rejected in high-sec**, **allowed in low/null**; the high-sec
        aggressor is flagged + response-spawned; damage inside a base bubble is blocked in high-sec.
  - [ ] A claim attempt succeeds only in null; standings gate friendly-fire correctly.
- **Depends on:** A, M6 (combat). **Blocks:** E (low/null piracy targets), F (null claims).

### C. Player crafting economy — refine → components → assemble (§13.4)

- **Goal:** replace M3's single-step raw→ship build with the **full 4-stage production chain**
  (`economy-crafting.md`) — **raw → refine → components → products** — so almost everything flown
  is **player-built** and destruction creates demand.
- **Masterplan refs:** §13.4 (production chain; player-built; minimal NPC seed), §13.0 (eXploit),
  §15 (build-completion = economy event, outbox). **Design doc:**
  [`economy-crafting.md`](../design/economy-crafting.md) §1–6 (stages, raw/refined/component/
  product tables, dependency graph).
- **Current state:** `Economy.h`/`HarvestSystem` do harvest → deposit → **one-step** ship build
  against `EconomyTuning`; `BaseInventory`/`ShipCargo` exist (M5 schema). No refine/component
  stages, no itemized recipe consumption.
- **Work:**
  - [ ] **Refine + fabricate + assemble systems** (`Economy.h`, the recipe dataset from A):
        refine raw→refined (yield from Industry tech, area D); fabricate refined→components;
        assemble components→products. Each stage consumes itemized inputs from inventory,
        produces outputs, server-authoritative, deterministic (§7.2).
  - [ ] **Build-queue II** keyed to recipes + tech gates (D): a build is rejected if the
        blueprint isn't unlocked or inputs are missing; completion is an **economy event** →
        the **M5 write-through outbox** (zero-loss) + inventory write.
  - [ ] **Minimal NPC seed** as a price floor/ceiling (§13.4) — a few NPC buy/sell orders so the
        market (E) isn't empty at launch; not a vendor economy.
  - [ ] **Refining-as-base-function** (the `economy-crafting.md` §9 open question) — pick the
        base-function default; a dedicated refine structure stays a later data/role addition.
- **Tests (`NeuronCoreTest`; `testrunner/CraftingTests.cpp` mirror):**
  - [ ] The worked example (`economy-crafting.md` §8) round-trips: raw shopping list → refine →
        components → a Medium Fighter, consuming exactly the recipe inputs; missing-input and
        locked-blueprint builds are rejected.
  - [ ] A build completion emits **one idempotent outbox event** (zero-loss, replay-safe, M5 D).
- **Depends on:** A, D (tech gates), M5 D (outbox). **Blocks:** E (goods to trade), N.

### D. Research / tech-tree progression (§13.3)

- **Goal:** the **vertical progression axis** — spend **datacores** (the anomaly/NPC faucet) +
  resources + time to **unlock blueprints** (hull classes / module tiers), gating what the
  crafting economy (C) can build, with the **catch-up flattening** that keeps newcomers relevant.
- **Masterplan refs:** §13.3 (research unlocks hulls/module tiers; fed by salvage/anomaly data;
  catch-up curve; time-gated training **excluded**), §15 (research + unlocked blueprints in SQL).
  **Design doc:** [`tech-tree.md`](../design/tech-tree.md) §1–5.
- **Current state:** none — no research system, no datacore item, no blueprint-unlock state.
  Schema provisions research/unlocked-blueprints (M5 area B).
- **Work:**
  - [ ] **Research system** (`NeuronCore`): start/queue a research node (cost from A's dataset),
        consume datacores + resources over time → **unlock a blueprint id**. Prereqs enforced
        (acyclic, area A); unlocks are an **economy event** (outbox) + persisted (account-permanent
        per `tech-tree.md` §6 recommendation, M5 schema).
  - [ ] **Datacore faucet:** datacores drop from anomalies/expeditions + NPC salvage (G) —
        exploration → progression; tune faucet vs cost as data (A).
  - [ ] **Catch-up curve** (`tech-tree.md` §5): cost escalates / stat gain diminishes per tier so
        a T1/T2 fit stays within ~12–35% of T3 (R17) — authored in A, asserted here.
  - [ ] **Gate crafting (C) + fitting**: a build/fit using a locked blueprint is server-rejected.
- **Tests (`NeuronCoreTest`; `testrunner/ResearchTests.cpp` mirror):**
  - [ ] Completing a research node unlocks exactly its blueprint and persists; a node with unmet
        prereqs can't start; the unlock is one idempotent outbox event.
  - [ ] The catch-up math holds (cost↑ / gain↓ per tier, `tech-tree.md` §5); a locked blueprint
        is unbuildable until researched.
- **Depends on:** A, G (datacore source), M5 D. **Blocks:** C (tech gates), E, N.

### E. Regional markets, currency sinks & ship insurance (§13.4, §13.9)

- **Goal:** the **currency economy** — **per-region order books** (buy/sell, fees, hauling
  gameplay), the **currency sinks** that keep the loot economy from inflating, and **opt-in ship
  insurance** (a sink + risk buffer that keeps players fighting) — all **zero-loss economy
  events** on the M5 outbox.
- **Masterplan refs:** §13.4 (regional markets, single currency + sinks, all economy events),
  §13.9 (insurance partially reimburses destroyed ships; base disable-not-destroy is the core-
  asset backstop), §15 (`MarketOrders`/`MarketTrades`/`InsuranceContracts`, wallet + ledger;
  write-through outbox), §24 (order-filled + insurance-paid notifications). **Design docs:**
  [`economy-crafting.md`](../design/economy-crafting.md) §7 (sinks table) + §8,
  [`combat-balance.md`](../design/combat-balance.md) §7 (insurance payout is the master risk dial).
- **Current state:** none — no wallet logic, no order book, no insurance flow. Schema provisions
  wallet + ledger, `MarketOrders`/`MarketTrades`, `InsuranceContracts` (M5 area B); M6 emits the
  kill/loss events insurance consumes (M6 area G).
- **Work:**
  - [ ] **Per-region order books** (`ERServer` economy module): place/cancel buy/sell orders,
        **match** at a regional hub, settle via the **wallet + append-only currency ledger**; each
        trade is a **write-through outbox event** (zero-loss) + a `MarketTrades` row +
        **order-filled notification** (L). Price differs by region → hauling/piracy targets in
        low/null (B).
  - [ ] **Currency sinks** (`economy-crafting.md` §7): **transaction fees**, refit/repackage
        costs, **structure upkeep/territory tax** (F), **base repair / fuel / jump cost** (M3 nav)
        — each a ledger debit, tunable as data (A).
  - [ ] **Ship insurance** (§13.9): opt-in premium (sink) → on ship loss (M6 kill event) pay a
        **partial currency reimbursement** (faucet) via the ledger + **insurance-paid
        notification** (L). Payout % is the **master risk dial** (`combat-balance.md` §7), data-
        tuned. The base is **never insured** — disable-not-destroy is its backstop.
  - [ ] **Faucets** (NPC bounties from invasions/anomalies G, minimal NPC seed orders C) balanced
        against sinks so net currency doesn't inflate — swept by the N gate + telemetry (§21).
- **Tests (`ERServerTest` + `NeuronCoreTest`; `testrunner` mirror for the pure match/ledger rules):**
  - [ ] A buy/sell match settles the wallet, writes one `MarketTrades` row + **one idempotent
        outbox event** + a notification; a cancelled order refunds; fees debit the ledger.
  - [ ] An insured ship's loss pays the configured % to the owner (faucet) and the premium debited
        the ledger (sink); the payout balances per `combat-balance.md` §7 over N sims.
  - [ ] The currency ledger is append-only and reconstructs the wallet exactly across a warm
        restart (M5 F) — **zero economy loss**.
- **Depends on:** A, B, C, M6 G (kill events), M5 D/F. **Blocks:** F (upkeep/tax sink), N.

### F. Territorial conquest — claimable structures, capture/hold, upkeep/yield (§13.6)

- **Goal:** the **PvP endgame** — null-sec **claimable `TerritoryStructure`s** (extractors,
  sensor arrays, **jump beacons**) captured by **clearing defenders + a hold timer**, paying
  **yield** and costing **upkeep** while held, with **persisted individual ownership** and the
  base **disable-not-destroy** rule as the push-off mechanic.
- **Masterplan refs:** §13.6 (objectives, individual ownership, capture = clear + hold timer +
  reinforcement/retreat windows, upkeep/tax + yield), §13.1 (base disable-not-destroy pushes a
  defender off), §13.12 (jump beacons are a claimable `TerritoryStructure` type — own the network
  = control movement), §15 (`TerritoryStructures` + capture log; persisted), §24 (territory
  attacked/lost notification), R18 (individual ownership caveat). **Design doc:**
  [`universe-worldgen.md`](../design/universe-worldgen.md) §3 (TerritoryStructure entity).
- **Current state:** none — `TerritoryStructure` is a §13.11 entity not yet modelled; M5 schema
  provisions `TerritoryStructures` + a capture log; B gates claims to null-sec.
- **Work:**
  - [ ] **`TerritoryStructure` entity + claim system** (`NeuronCore`/`ERServer`): a null-sec
        structure with `owner`, `state`, `captureProgress`, upkeep/yield. Capture requires
        **clearing defenders + a hold timer** (no instant flip); **reinforcement/retreat windows**
        make it a campaign. Ownership is **banked to the account** (individual, §13.6/R18) and
        **persisted** (`TerritoryStructures` + capture-log row, an outbox economy event).
  - [ ] **Upkeep + yield ticks** on the §26 universe clock: held structures **pay yield** (income/
        buff/build bonus) and **cost upkeep** (currency sink, E) — use-it-or-lose-it pressure;
        unpaid upkeep lapses ownership.
  - [ ] **Claimable jump beacons** (§13.12): a captured beacon joins the owner's network — the
        conquest-controls-movement loop; a contested beacon is a chokepoint.
  - [ ] **Push-off via disable-not-destroy** (§13.1, M6 G): a defender is driven off when their
        garrisoned base is forced to emergency-retreat — capture means *driving off*, never
        deleting the account asset.
  - [ ] **Territory notifications** (L): attacked / lost / captured → `Notifications` (§24).
- **Tests (`ERServerTest` + `NeuronCoreTest`; `testrunner/ConquestTests.cpp` mirror):**
  - [ ] A null structure flips only after defenders cleared **and** the hold timer elapses (a
        reinforcement window resets it); a high/low-sec claim is rejected (B); ownership persists +
        restores across a warm restart (M5 F).
  - [ ] Upkeep debits + yield credits tick on the clock; unpaid upkeep lapses ownership; capturing
        a beacon adds it to the owner's jump network; each capture is one outbox event + notification.
- **Depends on:** A, B, E (upkeep sink), M6 G (disable-not-destroy), M5 D/F. **Blocks:** N.

### G. Procedural anomalies / expeditions (§13.7, §8.4)

- **Goal:** the **repeatable PvE + exploration** anchor — **scannable procedural sites**
  (combat / exploration / salvage) that spawn across the universe with **scaling difficulty +
  loot**, gated by sensor/scan, **reconstructed from a seed** (so they cost ~0 on the wire),
  clustering richer in low/null (risk→reward) and feeding the **datacore** research faucet (D).
- **Masterplan refs:** §13.7 (scannable sites, scaling difficulty/loot, research-data source,
  cluster in low/null), §8.4 / [`universe-worldgen.md`](../design/universe-worldgen.md) §5
  (**seed-procedural** — server transmits only divergences; same deterministic generator both
  ends), §13.3 (datacore faucet), §26 (event director schedules spawns). **Design doc:**
  `universe-worldgen.md` §4.4 (anomaly templates) + §5 (seed model).
- **Current state:** one **hand-placed** NPC site (M3 area F); `AnomalySite` is a §13.11 entity
  not yet modelled; M5 persists "cleared" state. The seed/deterministic generator substrate exists
  (deterministic ECS, §7.2; `universe-worldgen.md` §5).
- **Work:**
  - [ ] **Seed-procedural site generator** (`NeuronCore`, the `universe-worldgen.md` §5 model):
        `hash(universeSeed, regionId, epoch)` → concrete anomaly instances from the area-A
        templates — **identical server and client**, so only **divergences** (cleared/looted) cross
        the wire as interest-deltas (§8.4). Difficulty/loot scale by template + security tier.
  - [ ] **Scanning gameplay** (sensor/scan, building on M3 sensor/fog): a site is revealed by
        scan skill (Electronics tech, D); harder/richer sites need better scan — the exploration
        loop. Site combat runs on the **M6 AI substrate** (guardians = M6 fits).
  - [ ] **Loot + datacore payout** on clear: loot via the M6 loot path (outbox); **datacores**
        feed research (D) — exploration → progression.
  - [ ] **Snapshot-shaped + epoch respawn** (§26): sites live in the warm-restart `SimSnapshots`
        blob (transient, §15), respawning on the universe-clock epoch (the `universe-worldgen.md`
        §9 epoch-granularity dial).
- **Tests (`NeuronCoreTest`/`ERServerTest`; `testrunner/AnomalyTests.cpp` mirror):**
  - [ ] The generator is **deterministic** — same seed/region/epoch → identical sites on two runs
        (client==server, `SimHash`-stable); only divergences (a cleared site) persist/replicate.
  - [ ] A scanned site reveals at the right scan tier; clearing it drops loot + datacores (outbox);
        richer sites cluster in low/null per template weights; cleared state survives a warm restart.
- **Depends on:** A, B, M6 F (AI). **Blocks:** D (datacore faucet), H (shares the director), N.

### H. Dynamic faction invasions + event director (§13.7, §26)

- **Goal:** the **living-universe pulse** — NPC-faction **escalating incursions** that **degrade
  a region's yield/safety until repelled** (pulling even rivals into temporary cooperation),
  scheduled by a lightweight **event director** on the **universe clock**, coupled to how heavily
  a region is exploited.
- **Masterplan refs:** §13.7 (invasions degrade local yields/safety; escalation tied to universe
  clock + region exploitation), §26 (event director schedules invasions/anomalies on the clock +
  region heat, within sim/bandwidth budget), §9 (NPCs are server ECS, `ERServer/ai/`). **Design
  doc:** [`universe-worldgen.md`](../design/universe-worldgen.md) §4.4/§7 (invasion templates,
  event director).
- **Current state:** none — `InvasionEvent` is a §13.11 entity not yet modelled; no event director;
  the universe clock (§26) is specified but the director system is M7.
- **Work:**
  - [ ] **Event director** (`ERServer`, §26): a server system that schedules invasion/anomaly
        spawns from the area-A templates on the **universe clock**, weighted by **region
        exploitation heat** (a per-region exploitation counter) — within the sim/bandwidth budget
        (App. B), hot-reloadable as a live-ops lever (§26).
  - [ ] **Invasion events** (`InvasionEvent`): an escalating NPC-faction swarm (M6 AI fits) that
        **degrades local yields/safety** (a region debuff) until **repelled**; escalation ramps on
        the clock; repelling clears the debuff + pays bounties (E faucet).
  - [ ] **Coupling to the 4X loop:** heavier region exploitation raises invasion pressure — the
        built-in PvE/economy feedback (§13.7); tune the coupling as data (A).
  - [ ] **Snapshot-shaped + event-logged** (§15): invasions are transient (`SimSnapshots`) with an
        event log; a notification fires on incoming invasion (§24, L; HUD alert §22.2).
- **Tests (`ERServerTest`/`NeuronCoreTest`; `testrunner/InvasionTests.cpp` mirror):**
  - [ ] The director schedules a spawn at the right clock tick + region heat; an invasion applies
        the yield/safety debuff and **escalates** until repelled; repelling clears it + pays bounties.
  - [ ] Higher region exploitation raises invasion frequency (the coupling); the director stays
        within the App. B spawn/bandwidth budget; invasion state restores across a warm restart.
- **Depends on:** A, G (shared templates/clock), M6 F (AI). **Blocks:** N.

### I. Entity aggregation / LOD (fleet-as-cluster) + projectile batching — interest at scale (R16, R24, App. B)

- **Goal:** the **committed scale feature** (not an optional lever) — at hundreds of players a
  contested sector **must** replicate **distant fleets as a cluster, not N ships**, and **batch
  projectiles**, so per-client interest stays within App. B even when sim+encode are the binding
  limit.
- **Masterplan refs:** App. B (v0.15: "aggregation is mandatory, not optional"; ≤~250 visible
  entities × ~16 B × 20 Hz), R16 (fleet-as-cluster + projectile batching, committed M7), R24
  (per-client encode CPU + baseline RAM), §8.4 (the snapshot pipeline this extends — cell pub/
  sub, per-entity version, priority function, quantized delta), §9 (read-only encode job pool).
- **Current state:** M4 built cell pub/sub interest + per-entity version + quantized delta +
  tombstones + the job pool + time dilation, with **hard interest culling + a per-client visible
  cap**; M4/M6 **measured where the cap binds** (M6 projectiles, area D/M). M7 adds the
  **aggregation** layer the measurements justified.
- **Work:**
  - [ ] **Fleet-as-cluster aggregation/LOD** (`NeuronCore` snapshot pipeline, §8.4): beyond a
        relevance/distance threshold, replicate a fleet/sub-group as a **single cluster record**
        (count + centroid + aggregate IFF/velocity) instead of N per-ship deltas; the client
        renders it as a cluster, resolving to individual ships only when it enters close interest.
        Drops the per-client visible-entity count below the App. B cap in the contested case.
  - [ ] **Projectile batching:** coalesce many same-source/same-type projectiles into a **batched
        record** (the M6-measured pressure, area D) rather than one delta each.
  - [ ] **Priority-function integration:** aggregation rides the existing §8.4 **priority + quota
        scheduler** (a cluster is one low-relevance record; near entities stay full-fidelity) — no
        new scheduler, an extension of the version/relevance model. Determinism preserved (§7.2).
  - [ ] **Measure against M4/M6 numbers:** re-run the contested-sector load test and show the
        per-client visible set + **encode CPU + baseline RAM** now hold App. B at the **target
        hundreds** where M6 culling-alone was pressured (R24).
- **Tests (`NeuronCoreTest` + `ERHeadlessTest`; `testrunner/AggregationTests.cpp` mirror):**
  - [ ] A distant fleet replicates as **one cluster record** below the threshold and **resolves to
        N ships** when it crosses into close interest; the client reconstructs both correctly;
        aggregation is deterministic (`SimHash`-stable).
  - [ ] Under the contested-sector load test (hundreds), per-client downstream + **encode p99 +
        baseline RAM** hold the App. B gates **with** aggregation where they were pressured without
        it (R16/R24 evidence cashed in).
- **Depends on:** M4 (interest pipeline), M6 D (projectile-pressure measurements). **Blocks:** N
  (the scale half of the Done gate).

### J. New-player onboarding + retention loop (§13.9, §13.10)

- **Goal:** the **soft landing** that makes growth possible — **protected high-sec start** + a
  **guided objective chain** teaching the loop (move base, harvest, refine, fit, run an anomaly,
  fleet up) before low/null, plus the §13.10 **retention loop** (daily reasons to log in).
- **Masterplan refs:** §13.9 (protected high-sec start, ship insurance + disable-not-destroy as
  loss mitigation, onboarding objective chain), §13.10 (short/mid/long session loops), §13.5
  (high-sec dense beacons / short distances, §13.12). **Already prototyped:**
  [`playable-slice.md`](playable-slice.md) (`NeuronClient/Onboarding.h` Welcome→Select→Engage→
  Clear→Done, Linux-tested). **Design doc:** [`tech-tree.md`](../design/tech-tree.md) §4 (the
  player's progression arc by phase/security).
- **Current state:** the **slice prototype** of an objective chain exists (`Onboarding.h`,
  Windows-smoke pending); no spawn-into-high-sec flow, no full loop coverage, no retention hooks.
- **Work:**
  - [ ] **Protected starter spawn** (§13.9/§13.5): new accounts spawn into **high-sec** (B
        enforces the no-PvP), near **dense public beacons** (A's region map) — the safe learning
        space.
  - [ ] **Full objective chain** (extend `Onboarding.h`): teach the **whole loop** — move base →
        harvest → **refine/craft** (C) → **fit** (M6/K) → **research** a first blueprint (D) →
        **run an anomaly** (G) → **fleet up** (§13.8) → choose to venture to low/null; seeds first
        goals; observed from the replica each frame (the slice's pattern).
  - [ ] **Retention loop hooks** (§13.10): surface the short loop (anomalies / trade route /
        defend an invasion / low-sec skirmish) and mid/long goals via objectives + notifications
        (L) — daily reasons to log in. (Seasons/leaderboards stay post-launch, §19.)
  - [ ] **Loss-mitigation surfacing:** insurance (E) + base disable-not-destroy (M6) made legible
        in the UI (K) so newcomers stay willing to fight (R17).
- **Tests (`NeuronClientTest`; `testrunner/OnboardingTests.cpp` extends the slice cases):**
  - [ ] A new account spawns in high-sec; the objective chain advances through the **full loop**
        (harvest→refine→fit→research→anomaly→fleet) and gates correctly on each step's completion.
  - [ ] Retention hooks surface the short-loop options + first goals; loss-mitigation state
        (insurance/disable) reads correctly.
- **Depends on:** B, C, D, G, K (the screens it points at), M6. **Blocks:** N.

### K. Full UI suite — fitting · market · research · inventory · starmap · territory (§22.1, §22.6)

- **Goal:** the **launch screen inventory** (§22.1) the sandbox needs — **fitting**, **market /
  trade**, **research / tech-tree**, **inventory / cargo & build queue**, **starmap /
  navigation**, **territory** — built on the existing **Canvas immediate-mode toolkit** (§22.6),
  all strings through the string table (§22.4), all actions as **server-validated intents** (§8.4).
- **Masterplan refs:** §22.1 (screen inventory), §22.6 (Darwinia windowed toolkit on
  CanvasRenderer — Window/Button/DropDown/Label, **already built in M2**), §22.4 (string table,
  no hard-coded text), §22.5 (HUD scale / accessibility), §23.4 (commands → intents). **Design
  docs:** [`ui-hud-layout.md`](../design/ui-hud-layout.md), [`darwinia-menu-ui.md`](../design/darwinia-menu-ui.md).
- **Current state:** M2 built the **Canvas widget toolkit + settings + radar/overview HUD**;
  [`playable-slice.md`](playable-slice.md) added the in-space camera/selection/HUD overlay. The
  **six sandbox screens do not exist** — M7 builds them on the M2 toolkit.
- **Work:**
  - [ ] **Fitting screen** (§22.1): hull slot grid, modules, **PG/CPU budget**, save/load **fit
        templates** (M6 `FitTemplates`) — drives the M6 fitting model; fits become validated intents.
  - [ ] **Market / trade screen:** regional order book, place buy/sell, fees, my orders (E).
  - [ ] **Research / tech-tree screen:** branches × tiers, datacore costs, queue (D).
  - [ ] **Inventory / cargo & build queue:** base storage + ship cargo; enqueue recipe builds (C).
  - [ ] **Starmap / navigation:** the **jump-beacon graph** (§13.12), set destination/route,
        security-tier coloring, owned territory (F), fleet/base location; autopilot (§23.1).
  - [ ] **Territory screen:** owned/contested structures, capture progress, upkeep/yield (F).
  - [ ] **Shared plumbing:** all six use the M2 toolkit + string table (no hard-coded text, §22.4),
        HUD-scale aware (§22.5); every action is a **server-validated intent** (§8.4), never client-
        authoritative.
- **Tests (`NeuronRenderTest` for widget/layout logic + `NeuronClientTest` for intent wiring;
  manual visual-acceptance on the Windows agent per `darwinia-menu-ui.md`):**
  - [ ] Each screen's widget logic (slot validation reflects PG/CPU, order entry, research queue,
        build enqueue, route plot) unit-tests on the toolkit; captions resolve through the string
        table (no literals).
  - [ ] A fit/order/research/build/route action emits the right **validated intent** (rejected
        server-side if illegal); HUD-scale reflows the panels.
- **Depends on:** M2 (toolkit), C, D, E, F (the systems each screen drives), M6 (fitting). **Blocks:** J, N.

### L. In-game mail + offline notifications (§24)

- **Goal:** the **async-retention** comms layer — **persistent in-game mail** (survives offline)
  and **offline notifications** for the events players miss (territory attacked/lost, order
  filled, insurance paid, killmail, build complete) — stored in SQL, surfaced on login and live.
- **Masterplan refs:** §24 (chat already on ReliableOrdered §8.2; **mail** persistent in SQL
  `Mail`; **notifications** in `Notifications`, surfaced on login + live), §15 (`Mail`/
  `Notifications`, **migration 003**), §22.1 (Mail & notifications screen). **Sources of events:**
  E (order filled / insurance paid), F (territory), G/M6 (killmail), C (build complete), H (invasion).
- **Current state:** chat is carried on the reliable channel (M1a+); the `Mail`/`Notifications`
  tables exist (M5 migration 003); **no mail send/inbox flow, no notification fan-out, no UI**.
- **Work:**
  - [ ] **In-game mail** (`ERServer` + SQL): player-to-player send → `Mail` row; inbox UI (K) with
        unread counts; **survives offline**; rate-limit + mute/block (§24 abuse controls; the §19
        anti-spam dial).
  - [ ] **Notification fan-out:** the M7 event sources (E/F/G/H/C/M6) write `Notifications` rows;
        surfaced **on login and live** (HUD alert §22.2, paired with audio cues §22.5).
  - [ ] **Mail & notifications screen** (K): inbox, read/unread, notification feed.
  - [ ] **Anti-spam** (§19): per-sender rate-limits + blocklists at a first-pass tuned as data.
- **Tests (`ERServerTest`; `NeuronClientTest` for the inbox/feed logic):**
  - [ ] A sent mail persists, shows unread, survives an offline round-trip; mute/block + rate-limit
        drop spam.
  - [ ] Each M7 event (order filled / insurance paid / territory lost / killmail / build complete /
        invasion) writes exactly one notification, surfaced on login and live.
- **Depends on:** E, F, G, H, C, M6 (the events), M5 (SQL), K (the screen). **Blocks:** J, N.

### M. Touch control scheme (§23)

- **Goal:** make the **overview-driven command model** work on **tablets** — the same intent
  layer as desktop, with touch affordances that **design out the select-vs-pan ambiguity** (R20),
  so desktop and touch are **one game, not two**.
- **Masterplan refs:** §23.1 (overview-driven model shared by desktop & touch — selection/commands
  through the same intent layer), §23.3 (touch affordances: **camera never on one finger**; one-
  finger tap = select / smart action / tap-hold radial; two-finger = pan/pinch/twist; large
  scalable hit targets), §23.4 (selection & command model), R20 (RTS-on-touch, Med→Low with this
  design). **Design doc:** [`touch-controls.md`](../design/touch-controls.md) (full gesture/smart-
  action tables).
- **Current state:** desktop input (mouse+keyboard, overview, pick/box-select) exists (M1b/M2 +
  `playable-slice.md`); the **`IInputSource` abstraction** routes input → intents. **No touch
  gesture handling, no on-screen touch bars.**
- **Work:**
  - [ ] **Touch gesture handling** (UWP `CoreWindow` gestures → `IInputSource`, §23): one-finger
        tap = select (overview blip/bracket); tap universe = **smart action** (§23.4 target-keyed);
        **tap-and-hold = radial context menu** (recovers right-click); **two-finger = camera only**
        (pan/pinch-zoom/twist) — the R20 ambiguity designed out.
  - [ ] **On-screen touch UI:** smart-select bar, contextual command bar, control-group bar,
        module buttons — **large, scalable hit targets** (§22.5 HUD scale).
  - [ ] **Box-select as a secondary marquee toggle** (not the default drag, §23.3).
  - [ ] **Same intent layer:** every touch command becomes the **same validated intent** as its
        desktop equivalent (§23.1/§8.4) — no touch-specific server path.
- **Tests (`NeuronClientTest` for the gesture→intent mapping; manual on a Windows touch device):**
  - [ ] One-finger tap selects / smart-acts; tap-hold opens the radial; **two-finger never selects**
        (camera only) — the ambiguity is gone (R20); each maps to the right validated intent.
  - [ ] Touch and desktop produce **identical intents** for the same command (one game, not two).
- **Depends on:** K (the on-screen bars), the §23 desktop model. **Blocks:** N (the touch half of Done).

### N. End-to-end sandbox gate (the Done verification) (§17, R15)

- **Goal:** the milestone proof — a **full sandbox session** played end-to-end by **players +
  bots** on **both control schemes**, with **zero economy loss across a server restart** — the
  literal §17 Done clause.
- **Masterplan refs:** §17 M7 Done, §10.3 (ERHeadless many bot sessions; bots ≠ NPCs), §16.3 /
  App. B (load/perf gates; the contested-sector case with aggregation, R16/R24), §26 (warm-restart
  uptime SLA), R15 (validate the *fun*/depth with bots + a closed playtest before polish).
- **Current state:** M3 area H drives a few bots through the M3 loop with `SimHash` record/replay;
  M6 M ran balance fights on the prod topology. M7 N runs the **whole sandbox** on the deployed
  stack.
- **Work:**
  - [ ] **End-to-end ERHeadless sandbox run** (`ERHeadlessTest`): bots **onboard in high-sec**
        (J) → **harvest → refine → craft → fit** (C/M6) → **research** (D) → **run anomalies** (G) →
        **trade** on regional markets (E) → **push into null** (B) → **claim & hold territory**
        through a **contested fight** (F, on the M6 combat model) — the full §17 narrative, server-
        authoritative, on the **M6 prod topology** (K8s + Azure SQL).
  - [ ] **Zero economy loss across a restart:** mid-session, trigger a **warm restart** (M5 F / K8s
        rolling restart, M6 K); bots reconnect (M5 G); **every economy event** (crafts, trades,
        insurance, captures, killmails) survives — assert the wallet/ledger/territory reconstruct
        exactly (the literal Done clause).
  - [ ] **Scale gate with aggregation (I):** the contested territorial fight holds **App. B** (per-
        client downstream + **sim p99 + encode p99 + baseline RAM**, R24) at the **target hundreds**,
        degrading via time dilation, never ticket-dropping.
  - [ ] **Manual two-scheme playtest** (R15): a human plays the same session on **mouse+keyboard
        and touch** (M); a **closed playtest** validates the *fun*/depth before polish; tune the
        economy/PvE/balance dials as **data** (A) guided by telemetry (§21).
- **Tests (`ERHeadlessTest` on the Windows agent; `testrunner` mirrors for the pure sandbox-rule
  halves; manual client passes):**
  - [ ] The full bot sandbox session completes end-to-end on the prod stack; a deliberately broken
        economy invariant (lost trade on restart) **trips** the zero-loss assertion (the gate bites).
  - [ ] The contested-territory fight holds the App. B gates **with aggregation** (I) at the target
        count; the human two-scheme + closed-playtest pass is recorded.
- **Depends on:** A–M (it *is* the end-to-end verification). **Blocks:** the M7 Done gate.

---

## Suggested order / dependency notes

> M7 is the widest milestone (L–XL): four loosely-coupled tracks plus a gate. The **economy/
> progression track** (A→C→D→E, with B gating PvP space) and the **PvE-content track** (A→G→H via
> the event director) are server-sim and mostly parallel after A. The **scale track** (I) is
> independent server work cashing in M4/M6 measurements. The **client track** (K→{J,L,M}) is
> Windows-agent and parallel to the server tracks once the systems it drives exist. **N** is the
> end-to-end gate over all of them.

1. **A (game-data catalog)** first — every M7 system reads it; no balance/content literal in code.
2. **B (security enforcement)** early — it gates *where* PvP/claims happen (E piracy, F null claims).
3. **Economy/progression:** **C (crafting)** ↔ **D (research)** are mutually referential (D gates
   C's builds; C builds D's hulls) — land the data (A) then both; then **E (markets/sinks/
   insurance)** once there are goods (C) + kill events (M6 G).
4. **PvE content:** **G (anomalies)** before **H (invasions)** — they share the event director +
   templates (A); G is also D's datacore faucet, so G unblocks the progression loop.
5. **F (conquest)** once B (null gate), E (upkeep sink), and M6 (disable-not-destroy) exist — the
   endgame that ties economy + PvP + persistence together.
6. **I (aggregation/LOD)** in parallel with the server tracks — it's a snapshot-pipeline extension
   (M4) cashing in M6's projectile-pressure measurements; needed before the N scale gate.
7. **Client:** **K (UI suite)** is the spine — **J (onboarding)**, **L (mail/notifications)**, and
   **M (touch)** all build on or point at its screens; run them after the server systems they
   drive land, parallel to the remaining server work.
8. **N (end-to-end gate)** last — the full sandbox session on the prod topology, zero economy loss
   across a restart, scale-gated with aggregation, validated on both control schemes + a closed
   playtest.

**Cross-milestone:** M7 **consumes** M4 (interest pipeline — I extends it; G/H replicate through
it), M5 (the outbox — every M7 economy event is zero-loss; warm-restart — the N restart clause;
the M7 schema tables), and M6 (the combat model under conquest/anomalies/invasions; the AI
substrate G/H schedule; loot/kill events E/L consume; the prod topology N runs on). M7 is the
**last pre-launch milestone** — it turns the engine + mechanisms into the §13 game; what remains
after it is **post-launch** (§19).

## Done gate (mirrors §17 "Done")

- [ ] **Tiered security enforced** — high (PvP off + response), low (PvP, no claims), null (PvP +
      claims); base safe-zone bubble (B).
- [ ] **Player crafting economy** — refine → components → assemble, player-built, build-completion
      a zero-loss economy event (C), gated by **research** unlocks (D).
- [ ] **Regional markets + currency sinks + ship insurance** — per-region order books, sinks that
      hold inflation, insurance payout the tuned risk dial — all zero-loss on the outbox (E).
- [ ] **Territorial conquest** — claimable null structures, **clear + hold timer** capture,
      upkeep/yield, **persisted individual ownership**, push-off via disable-not-destroy (F).
- [ ] **Procedural anomalies/expeditions** — seed-deterministic scannable sites, scaling loot +
      the datacore faucet, richer in low/null (G).
- [ ] **Dynamic faction invasions + event director** — escalating incursions degrade a region
      until repelled, scheduled on the universe clock by region heat (H).
- [ ] **Entity aggregation/LOD + projectile batching** — distant fleets replicate as clusters;
      the contested-sector case holds App. B (downstream + sim/encode p99 + baseline RAM) at the
      target hundreds (I, R16/R24).
- [ ] **Protected onboarding + retention loop** — high-sec start + the full-loop objective chain;
      daily-reason hooks; loss-mitigation legible (J).
- [ ] **Full UI suite** — fitting / market / research / inventory / starmap / territory on the
      Canvas toolkit, actions as validated intents (K).
- [ ] **In-game mail + offline notifications** — persistent mail surviving offline; notifications
      for the events players miss, on login + live (L, §24).
- [ ] **Touch control scheme** — the overview model on tablets, select-vs-pan ambiguity designed
      out, identical intents to desktop (M, §23).
- [ ] **Full sandbox session end-to-end** — onboard → build & fit → run anomalies → trade → push
      into null → **claim & hold territory** through a contested fight, by players + bots on
      **mouse+keyboard and touch**, with **zero economy loss across a server restart** (N).
- [ ] All matching `<project>Test` suites green (§16.1) + Linux `testrunner` mirrors for the
      platform-independent sandbox/economy/PvE/aggregation logic (§16.2); per-milestone perf gates
      met (§16.3, App. B).
