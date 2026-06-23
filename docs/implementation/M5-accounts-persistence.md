# M5 — Accounts, Auth & Persistence (Implementation Plan)

> Derived from [`../masterplan.md`](../masterplan.md) §17 (milestone **M5**).
> **Status:** 🔨 **Implemented — portable cores verified on the Linux testrunner; the
> ODBC/auth/warm-restart wiring is written but UNVERIFIED here** (this is a Linux container:
> no MSBuild / ODBC Driver 18 / CNG / reachable SQL Server — see §16.3). Validate on the
> Windows build agent against the dev SQL Server.
> - **Verified now (testrunner, §16.2):** write-through outbox zero-loss + idempotent replay
>   (`OutboxTests`, area D); warm-restart blob serde + `StateHash` (`WarmRestartTests`) and the
>   `ServerUniverse`↔`PersistState` capture/restore round-trip incl. `SimHash` (`WarmRestartCaptureTests`,
>   area F); reconnect backoff/jitter (`ReconnectTests`, area G); PBKDF2-HMAC-SHA512 vs FIPS-180/
>   RFC-4231 KATs (`Pbkdf2Tests`, area C hash); persistence/auth telemetry (`PersistTelemetryTests`,
>   area H). **224 testrunner cases green.**
> - **Written, Windows-unverified:** the `ERServer/persist/*` ODBC layer (A), `CngCrypto::Pbkdf2HmacSha512`
>   (C — must match the portable reference, gated by the `ERServerTest` cross-check), `ServerHost`
>   account-bound auth flow (C), write-behind (E) + `SimSnapshotStore` (F) SQL, **and now the
>   `ERServer.cpp` `main()` bootstrap** that wires them together: `PersistConfig::LoadFromEnv` →
>   `OdbcConnectionPool` + `PersistenceThread` + `AccountStore` injected into `ServerHost`
>   (`SetPersistDeps`); the persisted static server key (file-backed, replacing the M1a ephemeral
>   key); startup warm-restart restore (`LoadLatestSnapshotForRestore` → `DecodeState` →
>   `ServerUniverse::RestoreState`, then `ReadOutboxSince` replay); the periodic snapshot capture
>   callback (`CaptureState` → `EncodeState` at the current outbox watermark); and build-completion
>   economy events routed to the outbox via the persistence thread. Schema: migrations **004** (outbox
>   idempotency key + snapshot watermark) and **005** (per-account PBKDF2 iterations) landed;
>   `schema.sql` aligned.
> - **Remaining (Windows build agent + dev SQL only):** the **live kill/restart zero-loss drill** on
>   Windows + SQL (I) — the one item that genuinely needs the agent. All wiring it exercises is written.
>
> _Original plan (drafted from `_template.md`, per [`README.md`](README.md)) follows._
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
- **Current state:** **written** — `ERServer/persist/` exists (`OdbcConnection`, `OdbcConnectionPool`,
  `PersistConfig`, `PersistQueue`, `PersistenceThread`, plus the stores). Windows/ODBC, **unverified
  on Linux** (no Driver 18 / reachable SQL here). The `Config/db/` schema + migrations are in place.
- **Work:**
  - [~] **`ERServer/persist/` ODBC wrapper:** `OdbcConnection` connects (`SQLDriverConnect`, Driver 18,
        `Encrypt=yes`), with parameterized statements, result binding + error mapping; the connection
        string + credentials come from `PersistConfig::LoadFromEnv` (§20), never hard-coded.
        *Written; validate on the build agent.*
  - [~] **Connection pooling** + a **persistence thread**: `OdbcConnectionPool` + `PersistenceThread`
        own both durability paths (D outbox drain, E write-behind batch) via the `PersistQueue` MPSC
        queues; the tick only ever does O(1) `Enqueue*` (`EnqueueEconomy`/`EnqueueWriteBehind`), so SQL
        latency never stalls the 30 Hz tick (§9). *Written; validate on the build agent.*
  - [~] **Azure-SQL compatibility guard:** the stores use single multi-statement transactions, no
        cross-DB / SQL Agent / FILESTREAM (§15) so the M6 migration is connection-string-only. *Written.*
  - [ ] **CI wiring (§16.3):** the `SessionStart` hook already targets a **dev SQL Server over the
        network** — point the persist-layer integration tests at it (skip gracefully if absent on a
        pure-Linux run). *Pending the build agent.*
- **Tests (`ERServerTest`, §16.1):**
  - [~] Connect / round-trip a parameterized insert+select against the dev SQL instance;
        connection-pool reuse; error path on a bad statement is mapped, not crashed. *ODBC — Windows.*
  - [~] The persistence thread drains a queue without blocking a simulated tick loop (hot-path
        isolation assert). *Windows (the queue model itself is the testrunner-verified `PersistQueue`).*
- **Depends on:** B (schema to talk to). **Blocks:** C, D, E, F.

### B. Schema & migrations (`Config/db/`) (§15)

- **Goal:** finalize the **forward-only, Azure-SQL-compatible** schema for exactly the rows the M5
  loop persists — accounts/sessions, wallet + ledger, bases, ships, inventory, build queue, universe
  /region rows, outbox, snapshots — extending the drafted schema only where the M3 loop needs it.
- **Masterplan refs:** §15 (schema inventory + catalog/balance boundary: stats/recipes/research/
  anomaly defs are **game data**, not SQL; SQL keeps canonical item ids + mutable state; transient
  sim state is **never normalized**, only in the `SimSnapshots` blob), R7 (Azure parity).
- **Current state:** **landed** — `schema.sql` + migrations 001–003 define the full M5→M7 table set,
  and the two M5 forward-only migrations are in place: **004** (`EconomyOutbox` idempotency key +
  the `SimSnapshots.OutboxWatermark` column) and **005** (per-account PBKDF2 iteration cost). DDL is
  authored; an apply against live SQL is the build-agent check.
- **Work:**
  - [~] **Finalize the M5-active tables:** `Accounts`/`Sessions` (C), `Wallets`/`CurrencyLedger` (D),
        `Bases`/`Ships`/itemized inventory/`BuildQueue` (D/E), `Regions`/universe rows, `EconomyOutbox`,
        `SimSnapshots`. Columns match the M3 sim state; authored — confirm on the SQL apply.
  - [x] **New forward-only migrations** — **004** (outbox `IdempotencyKey` UNIQUE filtered index +
        `OutboxWatermark`) and **005** (`Accounts.Pbkdf2Iterations`) landed; existing migrations are
        untouched (§15 forward-only rule). *(DDL authored; the live apply is the `[~]` test below.)*
  - [~] **Catalog/balance boundary check:** item/hull/module *stats*, recipes, research costs,
        nav/economy tuning stay **game data** (`datacook`, §12.6) — SQL holds only canonical
        `ItemDefs` ids + mutable player/economy/universe state. Transient sim (NPCs, projectiles,
        anomaly sites) is **not** normalized — it rides the `SimSnapshots` blob (F). *Authored.*
- **Tests (`ERServerTest` + a migration apply check in CI):**
  - [~] Migrations apply cleanly from empty → current on the dev SQL instance; re-apply is a no-op
        (idempotent / forward-only). *Windows + SQL.*
  - [~] A round-trip of each M5-active entity (account, wallet, base, ship, inventory line) matches
        the sim's view. *Windows + SQL.*
- **Depends on:** nothing (DDL). **Blocks:** A, C, D, E, F.

### C. Accounts & authentication (real login) (§14)

- **Goal:** replace the dev "pick a name" stub with **real registration/login** — **PBKDF2-HMAC-
  SHA512 via CNG** (per-user random salt + **server-side pepper**, high tunable iterations), an
  expiring **session token**, **rate-limit/lockout** (per-account + per-IP), and **one active session
  per account** — all over the existing encrypted, server-authenticated channel (§8.5).
- **Masterplan refs:** §14 (full auth spec), §8.5 (login over the encrypted channel after ECDH +
  server-key verify → expiring session token), §15 (Accounts/Sessions rows), R6 (credential/MITM),
  R13 (handshake DoS — the stateless cookie already guards ECDH).
- **Current state:** **written** — the portable PBKDF2-HMAC-SHA512 reference (`Pbkdf2.h`) is
  testrunner-verified; the CNG production path (`CngCrypto::Pbkdf2HmacSha512`) + `AccountStore`
  (register/login/lockout/one-session) + the `ServerHost` account-bound auth flow are written,
  Windows-unverified. `ServerHost` now gates gameplay on a logged-in account (not net id).
- **Work:**
  - [x] **Password hashing — algorithm:** PBKDF2-HMAC-SHA512 with per-user salt + iteration cost is
        proved against FIPS-180 / RFC-4231 KATs and its own HMAC construction (`Pbkdf2Tests`,
        salt+cost-sensitive). The **CNG** realisation `CngCrypto::Pbkdf2HmacSha512` must be byte-
        identical to this reference (cross-checked in `ERServerTest`) — `[~]` (Windows).
  - [~] **Registration & login flow** over the encrypted channel (§8.5): `ServerHost::OnAuthMessage`
        handles register/login on the established secure channel via `AccountStore::Register`/`Login`
        → an expiring 32-byte session token; credentials only flow after ECDH + server-key verify.
        Carried on reliable `Command` frames with a 1-byte auth opcode because the §8.5 wire types +
        the M1 `LoginRequest` body are frozen here (documented in `ServerHost.h`). *Windows.*
  - [~] **Abuse controls:** per-account lockout (`Accounts.LoginFailures`/`LockedUntil`) + an in-process
        per-IP rate limiter live in `AccountStore`; `ServerHost` records the §21 auth counters at the
        login sites. *Written (the limiter/lockout are in the Windows-unverified store).*
  - [~] **Session rules:** `ServerHost::ValidateSession` checks the token on reliable traffic;
        **one active session per account** — `AccountStore::Login` revokes prior sessions atomically and
        `ServerHost::KickExistingSessionForAccount` drops a duplicate live connection (no double-spawn);
        login **binds the base** via spawn (first login) or re-attach to the restored entity
        (`BindAccountBase` + `SetAccountBase`), replacing the cookie-time base spawn. *Windows.*
  - [~] **Persist the static server key** (§8.5): `ERServer` now loads the stored
        `BCRYPT_ECCPRIVATE_BLOB` (file-backed `er_server_key.bin`) or generates + writes one, replacing
        M1a's ephemeral key, so the client's pinned key survives restarts. *Windows/CNG.*
  - [x] **Keep the dev stub behind a flag** — `ServerHost` has `m_devAuthStub` (set via `SetPersistDeps`,
        env `ER_DEV_AUTH_STUB=1`), **default OFF = real auth**; ON restores the M1-M4 cookie-time
        "pick a name" base spawn. *(Flag + both code paths written; the real path is Windows-verified.)*
- **Tests (`ERServerTest` + the testrunner mirror for PBKDF2 vectors):**
  - [x] PBKDF2 produces stable, salt+cost-sensitive hashes; KATs match (`Pbkdf2Tests`). The CNG-vs-
        reference byte-identity + wrong-password reject are `ERServerTest` (`[~]`, Windows).
  - [~] Register → login → session token issued → token validates reliable traffic; expired/invalid
        token rejected. *Windows + SQL.*
  - [~] Rate-limit/lockout trips after N failures (per-account and per-IP); one-session rule kicks the
        duplicate atomically (no race double-spawn). *Windows + SQL.*
- **Depends on:** A, B. **Blocks:** G (reconnect uses the session token), D (economy is account-scoped).

### D. Write-through economy outbox (zero-loss) (§15)

- **Goal:** the **durability boundary's zero-loss half** — economy events (wallet/currency, build
  completion, inventory changes, and later kills/loot) are committed **transactionally, or staged to
  a durable outbox drained in order, *before* they are authoritative** — never lost on crash.
- **Masterplan refs:** §15 (write-through / transactional outbox; wallet + **append-only currency
  ledger**; "committed before considered authoritative — zero-loss"), §17 M5 Done ("**zero economy
  loss**"), R12.
- **Current state:** **written** — the portable zero-loss/idempotent model (`Outbox.h`) is
  testrunner-verified (`OutboxTests`); the SQL realisation (`EconomyStore`) + the `ERServer`
  build-completion hook are written, Windows-unverified.
- **Work:**
  - [x] **Ordered, idempotent, zero-loss model:** `Outbox.h` (`Outbox`/`EconomyState`) proves append +
        apply-in-one-transaction, ordered drain, and replay-with-no-double-credit by `idemKey`
        (`OutboxTests`: `ApplyUpdatesWalletAndLedgerAtomically`, `ApplyIsIdempotentByIdemKey`,
        `OrderedDrainAppliesAndAdvancesWatermark`, `CrashBeforeDrainReplaysWithZeroLossAndNoDoubleCredit`).
  - [~] **`EconomyOutbox` write path (SQL):** `EconomyStore::AppendWriteThrough` commits the outbox row
        + wallet + `CurrencyLedger` in ONE transaction; the `IdempotencyKey` UNIQUE index (migration
        004) makes a replayed drain exactly-once. *Windows + SQL.*
  - [~] **Hook the M3 economy events:** `ERServer` routes `ServerUniverse::DrainBuildCompleted` to
        `PersistenceThread::EnqueueEconomy` (build completion), and `ServerHost` enqueues the first-login
        wallet seed — both with deterministic `idemKey`s so a replay can't double-credit. *Windows.*
  - [~] **Ordered drain:** the persistence thread drains the outbox in order, idempotently. *Windows.*
- **Tests (`ERServerTest`; model mirrored on the testrunner):**
  - [x] Zero-loss + idempotent replay + atomic apply — proved in the portable model (`OutboxTests`).
  - [~] The SQL realisation commits atomically (forced-failure rollback) and replays with zero loss on
        restart (no double-credit). *Windows + SQL.*
- **Depends on:** A, B, C (account-scoped wallet). **Blocks:** I (restart drill measures zero-loss).

### E. Write-behind high-frequency state (bounded RPO) (§15)

- **Goal:** the **durability boundary's bounded-loss half** — position/velocity/transient AI state
  written **behind** the sim in **batches** with a **stated RPO** (a few seconds of movement may be
  lost on hard crash; **never** an economy event).
- **Masterplan refs:** §15 (write-behind, batched, stated RPO; "never an economy event"), §9 (DB out
  of the tick hot path), R12, §21 (RPO watermark telemetry).
- **Current state:** **written** — `WriteBehindStore` + the bounded `PersistQueue` + the
  `PersistenceThread` RPO-cadence flush are written, Windows-unverified. The bounded-queue
  drop-oldest semantics are the testrunner-verified `PersistQueue` model.
- **Work:**
  - [~] **Write-behind batcher** (sim → persistence thread): `EnqueueWriteBehind` pushes `WriteBehindRow`
        (pos / layered HP / state) to the bounded queue; `PersistenceThread::FlushWriteBehindIfDue`
        coalesces latest-wins per entity and flushes at the RPO cadence via `WriteBehindStore::FlushBatch`
        (per-row UPDATE in one transaction; TVP/bcp is the noted upgrade). *Windows + SQL.*
  - [~] **RPO watermark:** the thread advances the RPO watermark exposed via `PersistCounters`
        (`rpoWatermarkUnix`); `ERServer` feeds it into `PersistTelemetry::AdvanceRpoWatermark` (area H).
        *Written; the monotonic-advance property is `PersistTelemetryTests`-verified.*
  - [~] **Strict separation from economy (D):** the queue *kinds* make the asymmetry explicit
        (`MpscZeroLossQueue` for economy, `MpscBoundedQueue` for write-behind); write-behind carries no
        economy event. *Written (type-level + by inspection).*
- **Tests (`ERServerTest`):**
  - [~] Batched position writes land within the RPO cadence; the watermark advances monotonically.
        *Windows + SQL* (watermark monotonicity itself: `PersistTelemetryTests`).
  - [~] Crash → restored position is within the stated RPO bound; **no economy event** ever rode the
        write-behind path (separation invariant). *Windows + SQL.*
- **Depends on:** A, B. **Blocks:** F (snapshot includes restored position), I.

### F. Warm-restart — snapshot blob + event log (§15, §9)

- **Goal:** a periodic **binary state snapshot (blob) + an event log since the snapshot** (same
  serde, §7.2); a restart **replays the log onto the last snapshot** for a clean, verifiable state —
  not a reconstruction from normalized rows. ERServer stays **stateless**.
- **Masterplan refs:** §15 (warm restart: snapshot blob + event log; ERServer stateless → recover
  from snapshot + log; transient sim state lives **only** in the blob), §9 (stateless server), §26
  (warm-restart correctness is an **uptime SLA**), R12/R22.
- **Current state:** **written** — the portable blob codec (`WarmRestart.h`
  `PersistState`/`EncodeState`/`DecodeState`/`StateHash`) and the `ServerUniverse`↔`PersistState`
  capture/restore glue are testrunner-verified (`WarmRestartTests`, `WarmRestartCaptureTests`); the
  `SimSnapshotStore` SQL + the `ERServer` restore/snapshot bootstrap are written, Windows-unverified.
- **Work:**
  - [x] **Snapshot serializer (codec + capture):** `WarmRestart.h` versioned blob over the §7.2 serde
        + `ServerUniverse::CaptureState`/`RestoreState` cover bases/ships/build/NPC; round-trips are
        proved (`WarmRestartTests`: blob round-trip + `StateHash`, truncated/empty handling;
        `WarmRestartCaptureTests`: capture covers every category, is deterministic,
        Capture→Encode→Decode→Restore→Capture is stable incl. `SimHash`, restore rebinds ownership/
        build and never collides net ids).
  - [~] **Snapshot store + cadence (SQL):** `SimSnapshotStore::Save`/`LoadLatest` move the blob +
        `OutboxWatermark` (migration 004) to/from `SimSnapshots`; `ERServer`'s capture callback runs on
        the persistence thread at the snapshot cadence (`CaptureState` → `EncodeState`, watermark =
        `EconomyStore::MaxOutboxId`). *Windows + SQL.*
  - [~] **Event log since snapshot + restore path** (`ERServer` startup): `RestoreFromWarmRestart`
        loads the latest snapshot (`LoadLatestSnapshotForRestore` → `DecodeState` → `RestoreState`),
        then replays the post-watermark outbox rows (`ReadOutboxSince`); the model proves the replay is
        zero-loss + faithful (`OutboxTests`, `WarmRestartTests::SnapshotPlusLogEqualsContinuousRunWithZeroLoss`).
        *Windows + SQL.*
  - [~] **Stateless ERServer:** a fresh process restores purely from snapshot + outbox log (no durable
        state outside SQL except the file-backed static key). *Written; confirm on the build agent.*
- **Tests (`ERServerTest` + `ERHeadlessTest`; portable halves on the testrunner):**
  - [x] Snapshot → restore round-trip reproduces the sim; `SimHash` matches for the economy/ownership
        subset; snapshot + replayed log == continuous run; a second restart is stable — proved in the
        portable model (`WarmRestartTests` + `WarmRestartCaptureTests`).
  - [~] The same end-to-end through the SQL `SimSnapshotStore` + the real outbox. *Windows + SQL.*
- **Depends on:** D, E. **Blocks:** G, I.

### G. Reconnect & rolling-restart (24/7 uptime SLA) (§26, R22)

- **Goal:** clients **reconnect cleanly with backoff/jitter** after a warm restart (no thundering
  herd), via the session token + one-session rule; a **rolling-restart drill** proves warm-restart as
  the **uptime SLA** with no scheduled downtime.
- **Masterplan refs:** §26 (24/7, no scheduled downtime; rolling restarts rely on warm-restart;
  clients reconnect with backoff/jitter), §14 (session token + one-session reconnect, atomic), §8.5
  (handshake/clock-sync re-run on reconnect), R22.
- **Current state:** **written** — the portable backoff/jitter policy (`Reconnect.h`) is
  testrunner-verified (`ReconnectTests`); the `NeuronClient` reconnect loop + the server-side atomic
  re-bind are written/Windows-unverified (`ServerHost`'s one-session kick + `BindAccountBase`).
- **Work:**
  - [x] **Reconnect schedule:** `Reconnect.h` (`ReconnectPolicy` exponential ceiling + full-jitter
        `DelayMs`, deterministic `JitterRng`) is proved anti-herd (`ReconnectTests`:
        `CeilingGrowsExponentiallyThenCaps`, `FullJitterStaysWithinCeiling`, `AFleetSpreadsItsReconnects`,
        `JitterIsDeterministicPerSeed`).
  - [~] **Client reconnect** (`NeuronClient` session): re-run the handshake + re-present the session
        token with the `Reconnect.h` schedule, resume from ∅ baseline (M4 cold-start). *Windows/client.*
  - [~] **Server-side atomic reconnect:** the one-session rule handles a reconnect racing the reap —
        `AccountStore::Login` revokes the prior session atomically and `ServerHost::KickExistingSession-
        ForAccount` + `BindAccountBase` re-attach to the account's base with no double-spawn. *Windows.*
  - [ ] **Rolling-restart playbook:** restart the shard (warm-restart F), clients reconnect; document
        the drill as the §26 uptime SLA. *Pending the build agent.*
- **Tests (`ERHeadlessTest`; policy mirrored on the testrunner):**
  - [x] Backoff/jitter spreads a fleet's reconnects; the schedule is deterministic per seed
        (`ReconnectTests`).
  - [~] A bot fleet survives a warm-restart, re-binds to the same base, resumes; reconnect racing the
        reap binds one session, not two. *Windows + SQL.*
- **Depends on:** C, F. **Blocks:** I (the drill is part of the Done gate).

### H. Persistence & auth telemetry (§21)

- **Goal:** the **§21 persistence + auth counters** so the durability boundary and login health are
  measurable — extending the M4 telemetry (§21) to the new subsystems.
- **Masterplan refs:** §21 (outbox depth & drain latency; write-behind batch size/lag; **RPO
  watermark**; login attempts / lockouts / rate-limit hits), §16.3 (gates consumable by the harness).
- **Current state:** **written** — the aggregation core (`PersistTelemetry.h`) is testrunner-verified
  (`PersistTelemetryTests`); the `ERServer`/`ServerHost` sampling sites + export are written,
  Windows-unverified.
- **Work:**
  - [x] **Persistence + auth counters (aggregation):** `PersistTelemetry` aggregates outbox depth +
        drain-latency percentile (D), write-behind batch/lag + the monotonic **RPO watermark** (E), and
        login attempts/failures/lockouts/rate-limit hits (C) — proved by `PersistTelemetryTests`
        (`OutboxGaugesAndDrainPercentile`, `RpoWatermarkAdvancesMonotonically`, `AuthCountersIncrement`).
  - [~] **Sampling sites:** `ServerHost::OnAuthMessage` records the auth counters at the login sites;
        `ERServer` feeds outbox depth + RPO watermark from `PersistenceThread::Counters()` into
        `PersistTelemetry` each loop and logs them on the heartbeat. *Windows.*
  - [~] **Export** as structured logs + counters consumable by the ERHeadless drill (§16.3). *Windows.*
- **Tests (`ERServerTest`; aggregation on the testrunner):**
  - [x] Gauges/percentiles + auth counters aggregate correctly (`PersistTelemetryTests`).
  - [~] The live gauges track the actual queue + write state on the running server. *Windows + SQL.*
- **Depends on:** C, D, E. **Blocks:** I (the drill reads these).

### I. Integration — register/login + restart drill (the Done gate)

- **Goal:** the end-to-end milestone proof: **register/login works**, then **kill & restart the
  ERServer container** → universe + bases + economy **restore with zero economy loss**, and connected
  clients **reconnect cleanly**.
- **Masterplan refs:** §17 M5 Done, §10.3 (ERHeadless harness), §16.1/§16.2/§16.3, §26 (uptime SLA),
  R12/R22.
- **Current state:** **the drill harness is the only genuinely-remaining item** — every piece it
  exercises (auth, outbox, write-behind, warm-restart, reconnect, telemetry) is written, and the
  `ERServer.cpp` bootstrap that wires them is in place; the live kill/restart on Windows + SQL is what
  remains. M3 area H already drives the loop with bots.
- **Work:**
  - [~] **Register/login E2E:** a bot registers + logs in over the encrypted channel (C), gets a
        session token, plays the M3 loop (harvest→build), with economy mutations going write-through (D).
        *All server-side wiring written; the bot-side register/login + the run are Windows + SQL.*
  - [ ] **Kill/restart drill:** mid-play, **kill the ERServer container**; restart it (warm-restart F);
        assert **universe + bases + economy restore** — economy **zero-loss** (D, `SimHash`/ledger
        check), position within RPO (E). *The live container restart needs the build agent + SQL.*
  - [~] **Reconnect:** the connected bots reconnect with backoff/jitter (G), re-bind to their accounts'
        bases, and resume — no economy lost, no double-spawn. *Server re-bind written; live run Windows.*
  - [x] **Counters wired** (H) so the drill's zero-loss + RPO + reconnect-spread assertions are
        automated — `PersistTelemetry`/`ServerTelemetry` aggregation is verified and the `ERServer`
        sampling sites are wired (the live read is part of the Windows drill).
- **Tests (`ERHeadlessTest`):**
  - [ ] Full drill: login → play → kill/restart → restore → reconnect, with **zero economy loss**
        asserted via the ledger + `SimHash` economy subset. *Windows + SQL — the Done gate.*
  - [ ] Position restored within the stated RPO; clients reconnect without a storm. *Windows + SQL.*
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

## `ERServer.cpp` `main()` bootstrap — WRITTEN (Windows-unverified)

The entry-point glue that ties the components together is now **written in `ERServer.cpp`**
(unverified on Linux — no MSBuild/ODBC/CNG/SQL here; validate on the build agent). It follows
the shape below; the only remaining work is the live kill/restart drill (area I) on Windows + SQL.

```cpp
// 1. Config + persistence thread (owns the ODBC pool; off the 30 Hz tick, §9).
PersistConfig cfg = PersistConfig::LoadFromEnv();         // ER_DB_CONNSTR / ER_SERVER_PEPPER / cost
PersistenceThread persist(cfg);
persist.Start([&](int64_t nowUnix){ /* SnapshotRequest: CaptureState→EncodeState + watermark */ });
AccountStore accounts(persist.Pool(), &crypto, cfg);
PersistTelemetry persistTel;
host.SetPersistDeps(&accounts, &persist, /*devAuthStub=*/false, &persistTel);

// 2. Persist the CNG static server key (replace the ephemeral LoadOrGenerateStaticKey({})).
// 3. Warm-restart on startup (stateless server):
if (auto snap = persist.LoadLatestSnapshotForRestore()) {
    PersistState st; if (DecodeState(snap->blob, st)) universe.RestoreState(st);
    if (auto rows = persist.ReadOutboxSince(snap->outboxWatermark))
        /* replay each economy row onto the restored EconomyState (idempotent, Outbox.h) */;
}
// 4. Per loop: host.SetUnixTime(std::time(nullptr)); feed acc.DilationFactor() to the clock echo;
//    sample ServerTelemetry/PersistTelemetry. On shutdown: persist.Stop().
```

M4 perf items that ride here are also **written** (Windows-unverified): the loop is driven by the
**IOCP listener** + per-connection lane affinity, and snapshot encode runs via `EncodeClientsPooled`
over the post-tick view (`host.BroadcastSnapshots(out, encodeWorkers, &tel)`). See M4 areas F/G.

## Done gate (mirrors §17 "Done")

> Legend: `[x]` verified on the Linux testrunner · `[~]` written, Windows-unverified (no
> MSBuild/ODBC/CNG/SQL here) · `[ ]` needs the Windows build agent + dev SQL Server.

- [~] **Register/login works** — `ServerHost` auth flow + `AccountStore` (PBKDF2 + pepper +
      per-account/IP rate-limit + one-session + session token) written (C); the PBKDF2 algorithm is
      `[x]` (KAT-verified) and the CNG↔reference identity is gated by `ERServerTest`.
- [~] **Kill & restart → universe + bases + economy restore** — `CaptureState`/`RestoreState` +
      `SimSnapshotStore` written; the round-trip is `[x]` on the testrunner (`WarmRestartCaptureTests`),
      the live container restart is `[ ]`.
- [x]/[~] **Zero economy loss** — the outbox ordering/idempotent-replay contract is `[x]`
      (`OutboxTests`) and enforced in SQL by migration 004; the live drill is `[ ]` (I).
- [~] **Connected clients reconnect cleanly** — backoff/jitter is `[x]` (`ReconnectTests`); the
      session-token re-bind + storm-free reconnect on a real restart is `[ ]` (G, I).
- [~] High-frequency state restored **within RPO**; economy never on the write-behind path (E) —
      write-behind store + RPO watermark written; bound verified on the live drill `[ ]`.
- [x] **§21 persistence/auth counters** — `PersistTelemetry` `[x]` (`PersistTelemetryTests`);
      live sampling sites in `ERServer.cpp` `[~]`.
- [x] Linux `testrunner` mirrors green (§16.2) — **224 cases, 0 failed**. `[ ]` `ERServerTest` +
      migrations-apply on the Windows agent / dev SQL (§16.3).
- [ ] **The end-to-end kill/restart zero-loss + reconnect drill** (I) — Windows build agent + dev SQL.
