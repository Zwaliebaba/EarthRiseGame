# M5 — Accounts, Auth & Persistence (Implementation Plan)

> Derived from [`../masterplan.md`](../masterplan.md) §17 (milestone **M5**).
> **Status:** ⏳ Not started (M0/M1a/M1b/M2 complete; M3 active; M4 planned). Drafted from
> `_template.md`, per [`README.md`](README.md).
> **Plan style:** feature-area sections (see [`README.md`](README.md)).
> **Verification:** M5's gates are **real, enforceable** — the register/login flow, persistence
> layer, and the kill/restart **zero-loss drill** (areas A/C/I) run on the **Windows build agent
> against a dev SQL Server reached over the network** (§16.3, §20); platform-independent hashing
> /serde logic is mirrored on the Linux `testrunner` (§16.2). Assumes M3 (incl. its Windows
> surfaces) is closed when M5 starts.

## Milestone goal (verbatim from §17)

> **M5 — Accounts, auth & persistence** *(M–L)* — real login (custom username/password,
> PBKDF2 + pepper + rate-limit); ODBC persist layer + schema/migrations + **outbox
> (write-through) + write-behind + snapshot/log warm-restart**; **SQL Server over the
> network (self-hosted)**; ERServer stateless; **24/7 rolling-restart drill** (warm-restart
> as an uptime SLA) + **client reconnect with backoff/jitter** (§26, R22).
> **Done:** register/login works; kill & restart the ERServer container → universe + bases +
> economy restore with **zero economy loss**, and connected clients reconnect cleanly.

## Scope at a glance

- **In scope:** turning the in-memory, "pick a name" shard into a **persistent, restartable**
  service. **Real accounts & login** (custom username/password, **PBKDF2-HMAC-SHA512 via CNG** +
  per-user salt + server pepper + rate-limit/lockout, one active session) over the existing
  encrypted channel; a **thin ODBC persistence layer** in `ERServer/persist/` against **SQL Server
  reached over the network** (self-hosted); the **durability boundary** — economy **write-through /
  transactional outbox** (zero-loss) vs high-frequency **write-behind** (bounded RPO); **warm
  restart** via a periodic binary **snapshot blob + event log**; **client reconnect with
  backoff/jitter** and a **24/7 rolling-restart drill** that proves warm-restart as an uptime SLA;
  and the **§21 persistence/auth telemetry**.
- **Out of scope (later milestones):**
  - **Combat model, conquest, markets, invasions, anomalies, insurance, mail/notifications UI** are
    M6/M7. The **schema already provisions** those tables (`Config/db/schema.sql` +
    migrations 001–003 target M5→M7), but M5 only **reads/writes the rows the M3 loop produces**:
    accounts/sessions, wallet + currency ledger, bases, ships, itemized inventory, build queue,
    universe/region rows. Don't wire market/insurance/territory persistence here.
  - **Azure SQL migration + Kubernetes prod deploy** are **M6** (§20). M5 runs against **self-hosted
    SQL Server over the network**, but every statement stays **Azure-SQL-compatible** (§15) so M6 is
    a connection-string + auth change, not a rewrite. **App→DB auth is a SQL login** now (managed
    identity / Entra ID is M6).
  - **Scale/interest** is **M4** — M5 persists whatever the M4 sim produces; it does not change the
    snapshot pipeline. The **per-client baseline/interest state stays session-only** (never
    normalized — §15), recreated on reconnect.
- **Open questions that touch M5** (from §19, §15):
  - **PBKDF2 iteration count** — high and **tunable** (§14); pick a first-pass cost calibrated to
    login latency on the dev box; store the cost per-hash so it can be raised later. *Blocks the auth
    work item; decide in area C.*
  - **Write-behind RPO** — the bounded movement loss allowed on hard crash (e.g. ≤ a few seconds,
    §15); sets the write-behind batch cadence. **Economy is never in this budget** (zero-loss).
    *Blocks the write-behind cadence in area E; decide before E.*
  - **Snapshot cadence** — how often the warm-restart blob is taken vs how long the replayed log
    grows (§15); trades restart time against snapshot overhead. *Blocks area F tuning, not structure.*
  - **Reconnect backoff/jitter curve** — the schedule that avoids a thundering-herd reconnect storm
    after a rolling restart (R22). *Blocks area G tuning, not structure.*

## Current state (what M1a–M4 left us)

> File-level baseline. M5 replaces the dev identity stub and the in-memory durability gap.

- **Identity is a dev stub.** `ServerHost::OnDatagram` spawns a base on cookie-validation and
  identifies the player by **net id** — there is **no account, no credential check, no session
  token validation**. §14 names this the "pick a name behind a dev flag for M1–M4" stub that "real
  auth lands with persistence (M5)" replaces. The encrypted, server-authenticated channel (§8.5,
  CNG ECDH + server-key verify + AES-GCM) it rides on **already exists** (M1a).
- **No persistence layer exists.** `ERServer.cpp` comments "Persistence (ODBC outbox + write-behind)
  lands in M5"; there is **no `ERServer/persist/`** directory, no ODBC wrapper, no connection pool.
  All sim state (`ServerUniverse`: bases, ships, cargo/storage, build queue, fuel, ownership) lives
  **in memory** — an ERServer restart **loses everything** (M3 said so explicitly).
- **The schema is already drafted.** `Config/db/schema.sql` + `Config/db/migrations/{001_initial,
  002_gameplay_v0_9, 003_mail_notifications}.sql` define **Accounts, Sessions, Wallets,
  CurrencyLedger, PlayerStandings, ItemDefs, Regions, Bases, Ships, inventory, EconomyOutbox,
  SimSnapshots** and more — Azure-SQL-compatible, with the durability boundary documented in the
  header. M5 **implements against this schema** (extending only where the M3 loop needs a column),
  not from a blank slate.
- **Crypto for hashing is in place.** `CngCrypto`/`ICrypto` already wrap CNG (ECDH, AES-GCM, HKDF);
  M5 adds the **PBKDF2** path. `ERServer.cpp` notes the **static server key** is ephemeral at M1a
  and "M5 persists it" — pinning/persisting the static key is an M5 item.
- **Serde primitives for the warm-restart blob exist** (`Serde.h`/`BitStream.h`, §7.2) — the same
  versioned binary used on the wire serializes the snapshot blob + event log. `ServerUniverse::
  SimHash` (M3) gives a verifiable post-restart state check.
- **Sim state is "snapshot-shaped" by design** — M3 explicitly kept cargo/storage/build-queue/fuel/
  ownership "in a shape M5 can later snapshot." M5 cashes that in.

---

## Feature areas

### A. ODBC persistence layer (`ERServer/persist/`) (§15, §20)

- **Goal:** a thin custom **ODBC Driver 18** wrapper + a dedicated **persistence thread**, off the
  tick hot path, against **SQL Server reached over TCP 1433, `Encrypt=yes`** — the substrate for
  outbox (D), write-behind (E), accounts (C) and warm-restart (F).
- **Masterplan refs:** §15 (ODBC wrapper, connection pooling, parameterized statements/procs, TVP/
  `bcp` for big checkpoints, `Encrypt=yes`), §9 ("DB is out of the tick hot path"), §20 (external SQL
  over the network; connection string from a secret/env), R4.
- **Current state:** net-new — no `persist/` directory exists. The `Config/db/` schema + migrations
  are drafted (area B).
- **Work:**
  - [ ] **`ERServer/persist/` ODBC wrapper:** connect (`SQLDriverConnect`, Driver 18, `Encrypt=yes`),
        **parameterized statements / stored procs**, result binding, error mapping — thin, custom, no
        third-party. Connection string + credentials from a **secret/env** (§20), never hard-coded.
  - [ ] **Connection pooling** + a **persistence thread** that owns both durability paths (D outbox
        drain, E write-behind batch) via MPSC queues from the sim; **SQL latency never stalls the
        30 Hz tick** (§9).
  - [ ] **Azure-SQL compatibility guard:** avoid cross-DB queries / SQL Agent / FILESTREAM (§15) so
        the M6 migration is connection-string-only.
  - [ ] **CI wiring (§16.3):** the `SessionStart` hook already targets a **dev SQL Server over the
        network** — point the persist-layer integration tests at it (skip gracefully if absent on a
        pure-Linux run).
- **Tests (`ERServerTest`, §16.1):**
  - [ ] Connect / round-trip a parameterized insert+select against the dev SQL instance;
        connection-pool reuse; error path on a bad statement is mapped, not crashed.
  - [ ] The persistence thread drains a queue without blocking a simulated tick loop (hot-path
        isolation assert).
- **Depends on:** B (schema to talk to). **Blocks:** C, D, E, F.

### B. Schema & migrations (`Config/db/`) (§15)

- **Goal:** finalize the **forward-only, Azure-SQL-compatible** schema for exactly the rows the M5
  loop persists — accounts/sessions, wallet + ledger, bases, ships, inventory, build queue, universe
  /region rows, outbox, snapshots — extending the drafted schema only where the M3 loop needs it.
- **Masterplan refs:** §15 (schema inventory + catalog/balance boundary: stats/recipes/research/
  anomaly defs are **game data**, not SQL; SQL keeps canonical item ids + mutable state; transient
  sim state is **never normalized**, only in the `SimSnapshots` blob), R7 (Azure parity).
- **Current state:** **already drafted** — `schema.sql` + migrations 001–003 define the full M5→M7
  table set. M5 **reviews/finalizes the M5 subset** and adds migrations only for gaps the M3 loop
  surfaces.
- **Work:**
  - [ ] **Finalize the M5-active tables:** `Accounts`/`Sessions` (C), `Wallets`/`CurrencyLedger` (D),
        `Bases`/`Ships`/itemized inventory/`BuildQueue` (D/E), `Regions`/universe rows, `EconomyOutbox`,
        `SimSnapshots`. Confirm columns match the M3 sim state (cargo/storage/fuel/ownership/nav).
  - [ ] **New forward-only migration** (e.g. `004_*`) for any column the M3 loop needs that 001–003
        lack — never edit an existing migration (§15 forward-only rule).
  - [ ] **Catalog/balance boundary check:** item/hull/module *stats*, recipes, research costs,
        nav/economy tuning stay **game data** (`datacook`, §12.6) — SQL holds only canonical
        `ItemDefs` ids + mutable player/economy/universe state. Transient sim (NPCs, projectiles,
        anomaly sites) is **not** normalized — it rides the `SimSnapshots` blob (F).
- **Tests (`ERServerTest` + a migration apply check in CI):**
  - [ ] Migrations apply cleanly from empty → current on the dev SQL instance; re-apply is a no-op
        (idempotent / forward-only).
  - [ ] A round-trip of each M5-active entity (account, wallet, base, ship, inventory line) matches
        the sim's view.
- **Depends on:** nothing (DDL). **Blocks:** A, C, D, E, F.

### C. Accounts & authentication (real login) (§14)

- **Goal:** replace the dev "pick a name" stub with **real registration/login** — **PBKDF2-HMAC-
  SHA512 via CNG** (per-user random salt + **server-side pepper**, high tunable iterations), an
  expiring **session token**, **rate-limit/lockout** (per-account + per-IP), and **one active session
  per account** — all over the existing encrypted, server-authenticated channel (§8.5).
- **Masterplan refs:** §14 (full auth spec), §8.5 (login over the encrypted channel after ECDH +
  server-key verify → expiring session token), §15 (Accounts/Sessions rows), R6 (credential/MITM),
  R13 (handshake DoS — the stateless cookie already guards ECDH).
- **Current state:** net-new auth; the **encrypted channel + handshake exist** (M1a). `CngCrypto`
  wraps CNG but has **no PBKDF2 path** yet; `ServerHost` identifies by net id, not account.
- **Work:**
  - [ ] **Password hashing** (`CngCrypto`/`persist`): **PBKDF2-HMAC-SHA512**, per-user random salt
        + server **pepper** (from secret/env, never in SQL), **high tunable iteration count stored per
        hash** (so cost can be raised later) — never plaintext, never reversible (§14).
  - [ ] **Registration & login flow** over the encrypted channel (§8.5): credentials sent **only
        after** ECDH + server-key verification; server verifies vs `Accounts` → issues an expiring
        **session token** bound to the connection (`Sessions` row).
  - [ ] **Abuse controls:** per-account **and** per-IP **rate-limit + lockout/backoff** to blunt
        credential stuffing (§14, R6).
  - [ ] **Session rules:** token validation on reliable traffic; **one active session per account**
        (deny/kick the duplicate; reconnect handled **atomically** to avoid races, §14); login **binds
        the session to the player's `Base`/entity** — replacing the cookie-time base spawn in
        `ServerHost` with account-bound spawn/restore.
  - [ ] **Persist the static server key** (§8.5 / `ERServer.cpp` note: M1a's key is ephemeral) so the
        client's pinned key stays valid across restarts.
  - [ ] **Keep the dev stub behind a flag** for M-series iteration (§14) — real auth is the default,
        the stub is opt-in.
- **Tests (`ERServerTest` + `NeuronCoreTest` for the hash; testrunner mirror for PBKDF2 vectors):**
  - [ ] PBKDF2 produces stable, salt+pepper-dependent hashes; wrong password rejected; iteration
        cost round-trips.
  - [ ] Register → login → session token issued → token validates reliable traffic; expired/invalid
        token rejected.
  - [ ] Rate-limit/lockout trips after N failures (per-account and per-IP); one-session rule kicks the
        duplicate atomically (no race double-spawn).
- **Depends on:** A, B. **Blocks:** G (reconnect uses the session token), D (economy is account-scoped).

### D. Write-through economy outbox (zero-loss) (§15)

- **Goal:** the **durability boundary's zero-loss half** — economy events (wallet/currency, build
  completion, inventory changes, and later kills/loot) are committed **transactionally, or staged to
  a durable outbox drained in order, *before* they are authoritative** — never lost on crash.
- **Masterplan refs:** §15 (write-through / transactional outbox; wallet + **append-only currency
  ledger**; "committed before considered authoritative — zero-loss"), §17 M5 Done ("**zero economy
  loss**"), R12.
- **Current state:** net-new. M3 economy (cargo/storage/build queue) is **in-memory only**; there is
  no ledger, no outbox.
- **Work:**
  - [ ] **`EconomyOutbox` write path** (sim → persistence thread, area A): every economy mutation
        appends an **ordered, durable** outbox row in the **same transaction** as the balance change;
        the event is authoritative only once committed/staged (§15).
  - [ ] **Wallet + append-only `CurrencyLedger`:** every balance change appends a ledger row in the
        same transaction (anti-dupe audit) and mirrors to the outbox (per `schema.sql`).
  - [ ] **Hook the M3 economy events:** build completion (`DrainBuildCompleted`), deposit/storage
        changes, ownership — route through the outbox so they survive a restart. (Markets/insurance/
        loot stay M6/M7, but the **mechanism** is proven here.)
  - [ ] **Ordered drain:** the persistence thread drains the outbox **in order**; idempotent apply so
        a crash mid-drain replays cleanly.
- **Tests (`ERServerTest`):**
  - [ ] A balance change + ledger row + outbox row commit atomically (all-or-nothing on a forced
        failure).
  - [ ] Crash *before* drain → on restart the outbox replays the economy event with **zero loss**;
        replay is idempotent (no double-credit).
- **Depends on:** A, B, C (account-scoped wallet). **Blocks:** I (restart drill measures zero-loss).

### E. Write-behind high-frequency state (bounded RPO) (§15)

- **Goal:** the **durability boundary's bounded-loss half** — position/velocity/transient AI state
  written **behind** the sim in **batches** with a **stated RPO** (a few seconds of movement may be
  lost on hard crash; **never** an economy event).
- **Masterplan refs:** §15 (write-behind, batched, stated RPO; "never an economy event"), §9 (DB out
  of the tick hot path), R12, §21 (RPO watermark telemetry).
- **Current state:** net-new — positions live only in the in-memory sim.
- **Work:**
  - [ ] **Write-behind batcher** (sim → persistence thread, area A): periodically batch base/ship
        `Pos`/`*Hp`/`State` rows (`Bases`/`Ships`, per `schema.sql`) at the **RPO cadence** (the §19
        open question), `bcp`/TVP for big checkpoints (§15).
  - [ ] **RPO watermark:** track the timestamp up to which high-frequency state is durable, exposed to
        telemetry (area H) and asserted in the drill (I).
  - [ ] **Strict separation from economy (D):** write-behind carries **no** economy event — the two
        paths share the persistence thread but not the durability guarantee (§15).
- **Tests (`ERServerTest`):**
  - [ ] Batched position writes land within the RPO cadence; the watermark advances monotonically.
  - [ ] Crash → restored position is within the stated RPO bound; **no economy event** ever rode the
        write-behind path (separation invariant).
- **Depends on:** A, B. **Blocks:** F (snapshot includes restored position), I.

### F. Warm-restart — snapshot blob + event log (§15, §9)

- **Goal:** a periodic **binary state snapshot (blob) + an event log since the snapshot** (same
  serde, §7.2); a restart **replays the log onto the last snapshot** for a clean, verifiable state —
  not a reconstruction from normalized rows. ERServer stays **stateless**.
- **Masterplan refs:** §15 (warm restart: snapshot blob + event log; ERServer stateless → recover
  from snapshot + log; transient sim state lives **only** in the blob), §9 (stateless server), §26
  (warm-restart correctness is an **uptime SLA**), R12/R22.
- **Current state:** net-new. `SimSnapshots` table is drafted; `Serde.h`/`SimHash` exist; no
  snapshot/restore code yet.
- **Work:**
  - [ ] **Snapshot serializer:** serialize the authoritative `ServerUniverse` (bases, ships, cargo/
        storage, build queue, fuel, nav, ownership, **transient** NPC/anomaly state) to a versioned
        binary blob (§7.2) → `SimSnapshots`, on the **snapshot cadence** (the §19 open question).
  - [ ] **Event log since snapshot:** the economy outbox (D) + a sim event log form the "since last
        snapshot" stream; restart loads the latest blob and **replays the log onto it**.
  - [ ] **Restore path** (`ERServer` startup): load latest snapshot + replay log → reconstruct the sim;
        **verify with `SimHash`** (M3) that pre-crash and post-restart state match (modulo the
        write-behind RPO for position).
  - [ ] **Stateless ERServer:** confirm no durable state lives outside SQL — a fresh container restores
        purely from snapshot + log (§9).
- **Tests (`ERServerTest` + `ERHeadlessTest`):**
  - [ ] Snapshot → restore round-trip reproduces the sim (economy exact; position within RPO);
        `SimHash` matches for the economy/ownership subset.
  - [ ] Snapshot + replayed log == continuous run for the same input (log replay is faithful).
  - [ ] A second restart from the restored state is stable (idempotent recovery).
- **Depends on:** D, E. **Blocks:** G, I.

### G. Reconnect & rolling-restart (24/7 uptime SLA) (§26, R22)

- **Goal:** clients **reconnect cleanly with backoff/jitter** after a warm restart (no thundering
  herd), via the session token + one-session rule; a **rolling-restart drill** proves warm-restart as
  the **uptime SLA** with no scheduled downtime.
- **Masterplan refs:** §26 (24/7, no scheduled downtime; rolling restarts rely on warm-restart;
  clients reconnect with backoff/jitter), §14 (session token + one-session reconnect, atomic), §8.5
  (handshake/clock-sync re-run on reconnect), R22.
- **Current state:** net-new reconnect logic. `ServerHost::PruneStale` reaps idle/disconnected peers
  (M1a); the client has no reconnect-with-backoff path; rolling restart is undrilled.
- **Work:**
  - [ ] **Client reconnect** (`NeuronClient` session): on disconnect/timeout, re-run the handshake
        (§8.5) and re-present the **session token** (C) with **exponential backoff + jitter** (the §19
        open question) to avoid a reconnect storm (R22); resume the snapshot loop from ∅ baseline (the
        M4 cold-start path converges it).
  - [ ] **Server-side atomic reconnect:** the **one-session rule** (C) handles a reconnect that races
        the old session's reap — bind the restored session to the account's `Base`/entity atomically,
        no double-spawn.
  - [ ] **Rolling-restart playbook:** restart the shard (warm-restart F), clients reconnect — a brief
        blip is acceptable; document the drill as the §26 uptime SLA.
- **Tests (`ERHeadlessTest`):**
  - [ ] A bot fleet survives a server warm-restart: reconnect with backoff/jitter, re-bind to the same
        base, resume play (state restored by F).
  - [ ] N bots reconnecting after a restart spread their attempts (no synchronized storm — backoff/
        jitter assert, R22).
  - [ ] Reconnect racing the reap binds one session, not two (one-session atomicity).
- **Depends on:** C, F. **Blocks:** I (the drill is part of the Done gate).

### H. Persistence & auth telemetry (§21)

- **Goal:** the **§21 persistence + auth counters** so the durability boundary and login health are
  measurable — extending the M4 telemetry (§21) to the new subsystems.
- **Masterplan refs:** §21 (outbox depth & drain latency; write-behind batch size/lag; **RPO
  watermark**; login attempts / lockouts / rate-limit hits), §16.3 (gates consumable by the harness).
- **Current state:** net-new for persistence/auth (M4 added sim/net counters).
- **Work:**
  - [ ] **Persistence counters:** outbox depth + drain latency (D), write-behind batch size + lag +
        **RPO watermark** (E).
  - [ ] **Auth counters:** login attempts, lockouts, rate-limit hits (C).
  - [ ] **Export** as structured logs + lightweight counters (MS-only), consumable by the ERHeadless
        drill (§16.3) so the restart/zero-loss gate is automated.
- **Tests (`ERServerTest`):**
  - [ ] Outbox-depth / drain-latency / RPO-watermark gauges track the actual queue + write state;
        auth counters increment on attempt/lockout.
- **Depends on:** C, D, E. **Blocks:** I (the drill reads these).

### I. Integration — register/login + restart drill (the Done gate)

- **Goal:** the end-to-end milestone proof: **register/login works**, then **kill & restart the
  ERServer container** → universe + bases + economy **restore with zero economy loss**, and connected
  clients **reconnect cleanly**.
- **Masterplan refs:** §17 M5 Done, §10.3 (ERHeadless harness), §16.1/§16.2/§16.3, §26 (uptime SLA),
  R12/R22.
- **Current state:** net-new. M3 area H drives the loop with bots; M5 adds accounts + the kill/restart
  durability drill.
- **Work:**
  - [ ] **Register/login E2E:** a bot registers + logs in over the encrypted channel (C), gets a
        session token, plays the M3 loop (harvest→build), with economy mutations going write-through (D).
  - [ ] **Kill/restart drill:** mid-play, **kill the ERServer container**; restart it (warm-restart F);
        assert **universe + bases + economy restore** — economy **zero-loss** (D, `SimHash`/ledger
        check), position within RPO (E).
  - [ ] **Reconnect:** the connected bots reconnect with backoff/jitter (G), re-bind to their accounts'
        bases, and resume — no economy lost, no double-spawn.
  - [ ] **Counters wired** (H) so the drill's zero-loss + RPO + reconnect-spread assertions are
        automated.
- **Tests (`ERHeadlessTest`):**
  - [ ] Full drill: login → play → kill/restart → restore → reconnect, with **zero economy loss**
        asserted via the ledger + `SimHash` economy subset.
  - [ ] Position restored within the stated RPO; clients reconnect without a storm.
- **Depends on:** A–H. **Blocks:** Done gate (it *is* the end-to-end verification).

---

## Suggested order / dependency notes

> The substrate comes first, then the two durability halves, then warm-restart and reconnect, with
> the drill as the gate. **B (schema)** and **A (ODBC layer)** unblock everything; **C (auth)** can
> proceed in parallel with **D/E (durability)** once A+B exist.

1. **B (schema/migrations)** + **A (ODBC layer)** first — the persistence substrate.
2. **C (accounts & auth)** in parallel with the durability work once A+B land (auth needs the
   `Accounts`/`Sessions` rows and the ODBC layer, nothing else).
3. **D (write-through outbox)** and **E (write-behind)** — the two durability halves; D is the
   zero-loss path the Done gate hinges on, E carries the bounded-RPO state.
4. **F (warm-restart snapshot + log)** once D+E exist — it composes them into a restartable shard.
5. **G (reconnect + rolling-restart)** once C (session token) + F (restored state) exist.
6. **H (telemetry)** as C/D/E land, **before** the drill.
7. **I (register/login + restart drill)** last — the zero-loss/reconnect gate that closes M5.

**Cross-milestone:** M5 persists whatever the **M4** sim produces; the M4 per-client interest/baseline
state stays **session-only** (never normalized, §15) and is recreated on reconnect via the M4
cold-start path. **Azure SQL + K8s + managed-identity auth are M6** (§20) — keep every statement
Azure-SQL-compatible so that migration is a connection-string + auth change.

## Done gate (mirrors §17 "Done")

- [ ] **Register/login works** — custom username/password, PBKDF2 + pepper + rate-limit, session
      token over the encrypted channel (C).
- [ ] **Kill & restart the ERServer container → universe + bases + economy restore** via warm-restart
      (snapshot + log), ERServer stateless (F, A, B).
- [ ] **Zero economy loss** across the restart — write-through outbox + append-only ledger, verified
      (D, I).
- [ ] **Connected clients reconnect cleanly** — session token + backoff/jitter, no storm, atomic
      one-session re-bind (G, I).
- [ ] High-frequency state restored **within the stated RPO**; economy never on the write-behind path
      (E).
- [ ] **§21 persistence/auth counters** wired and read by the automated drill (H).
- [ ] All matching `<project>Test` suites green (§16.1) + Linux `testrunner` mirrors for the
      platform-independent hashing/serde logic (§16.2); migrations apply cleanly in CI (§16.3).
