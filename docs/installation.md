# EarthRise — Installation & Startup Guide (Windows + SQL Server, M5 persistence)

> **Scope of this guide.** How to build the dedicated server (`ERServer`), install and
> prepare a **SQL Server Developer** database, wire the two together, and run the full
> **M5 persistence** path (real accounts/login + warm-restart) on a **Windows** host.
>
> **Out of scope (on purpose):** containers/Docker/Kubernetes (`Config/deploy/`) and the
> Azure SQL / managed-identity production path (M6, `§20`). Those are a connection-string +
> auth change on top of this same binary, not a rewrite.
>
> **Reality check.** The `ERServer` ODBC/auth/warm-restart layer is **written but not yet
> verified on Windows against live SQL** — it is marked `[~]` throughout
> [`docs/implementation/M5-accounts-persistence.md`](implementation/M5-accounts-persistence.md)
> (this repo's CI is Linux-only and has no MSBuild/ODBC/SQL). This guide is the procedure to
> stand it up and validate it on a real Windows + SQL box; treat the first run as a
> bring-up, not a turnkey deploy.

---

## 0. What you're standing up

```
  EarthRise (UWP client)  ─┐                         ┌── ERServer.exe (this guide)
  ERHeadless (bot driver) ─┴── encrypted UDP 7777 ──▶│   30 Hz sim · auth · persistence
                                                      └────────┬─────────────────────────
                                                               │ ODBC / TCP 1433 (Encrypt=yes)
                                                               ▼
                                                      SQL Server Developer (separate service)
```

`ERServer` is a Win32 console daemon. It owns the authoritative simulation, terminates the
encrypted handshake, authenticates accounts, and persists state to SQL **off the tick** via
a dedicated persistence thread. SQL Server is always an **external network service** — even
locally it is reached over TCP 1433, never embedded.

---

## 1. Prerequisites

Install on the Windows host (10/11 or Server):

1. **Visual Studio 2022** with these workloads/components:
   - *Desktop development with C++* (MSVC toolset, C++23 / `/std:c++latest`)
   - *Universal Windows Platform development* (only needed if you also build the `EarthRise`
     UWP client; not required for `ERServer` + `ERHeadless`)
   - A **Windows 10/11 SDK** (DirectX 12, XAudio2, Winsock, CNG, ODBC headers)
2. **ODBC Driver 18 for SQL Server** —
   [download](https://learn.microsoft.com/sql/connect/odbc/download-odbc-driver-for-sql-server).
   The server links `odbc32.lib` and connects with `Driver={ODBC Driver 18 for SQL Server}`.
3. **SQL Server 2022 Developer edition** (free) —
   [download](https://www.microsoft.com/sql-server/sql-server-downloads). Developer edition is
   feature-equivalent to Enterprise and free for non-production.
4. **`sqlcmd`** (ships with SQL Server / the *Command Line Utilities*) to apply the schema.
   *(SSMS or Azure Data Studio work too if you prefer a GUI.)*

> Everything here uses `ERServer` + `ERHeadless`. You do **not** need the GPU/UWP client to
> get a working, persistent server.

---

## 2. Get the code & build the server

```bat
git clone https://github.com/Zwaliebaba/EarthRiseGame.git
cd EarthRiseGame
```

Build `ERServer` (and the bot driver) in **x64**. From a *Developer Command Prompt for VS
2022*:

```bat
msbuild EarthRise.slnx /p:Configuration=Release /p:Platform=x64 /t:ERServer /m
msbuild EarthRise.slnx /p:Configuration=Release /p:Platform=x64 /t:ERHeadless /m
```

Output lands in `x64\Release\ERServer.exe`. (Use `Configuration=Debug` for a debug build;
the path becomes `x64\Debug\`.)

> First build also compiles the HLSL shaders for the render projects; for a server-only
> build the `/t:ERServer` target skips that.

---

## 3. Install & prepare SQL Server

### 3.1 Install the engine

Run the SQL Server 2022 Developer installer → **Basic** install is enough. Note the instance
name (default is `MSSQLSERVER`, reachable as `localhost` / `.`).

### 3.2 Enable TCP/IP (required — the server connects over TCP, not shared memory)

1. Open **SQL Server Configuration Manager**.
2. *SQL Server Network Configuration → Protocols for MSSQLSERVER* → enable **TCP/IP**.
3. Confirm it's listening on **1433** (TCP/IP → Properties → IP Addresses → `IPAll` → TCP Port `1433`).
4. Restart the **SQL Server (MSSQLSERVER)** service.

### 3.3 Create a login for the server to use

M5's app→DB auth mode is a **SQL login** (`DbAuthMode::SqlLogin`, the default). Create a
dedicated login rather than reusing `sa`. In `sqlcmd` (Windows-auth as an admin):

```bat
sqlcmd -S localhost -E -Q "CREATE LOGIN er_app WITH PASSWORD = 'ChangeMe_Strong!1';"
```

You'll grant it rights to the `EarthRise` database in the next step.

### 3.4 Create the database and apply the schema

For a **fresh** database, apply `Config\db\schema.sql` — it is the *canonical full schema*
and already includes the M5 migration content (per-account PBKDF2 iterations, the outbox
idempotency key, and the snapshot watermark column):

```bat
sqlcmd -S localhost -E -Q "CREATE DATABASE EarthRise;"
sqlcmd -S localhost -E -d EarthRise -i Config\db\schema.sql
sqlcmd -S localhost -E -d EarthRise -Q "CREATE USER er_app FOR LOGIN er_app; ALTER ROLE db_owner ADD MEMBER er_app;"
```

> **`schema.sql` vs `migrations/`.** Use **`schema.sql` for a brand-new database**. The
> ordered files in `Config\db\migrations\` (`001`→`005`) are the **forward-only path for
> evolving an *existing* deployment** — you don't replay them onto an empty DB. (`schema.sql`
> already reflects migrations 004 and 005.) Verifying that `migrations` from empty → current
> produces a schema identical to `schema.sql` is one of the M5 build-agent checks; if you hit
> a discrepancy, `schema.sql` is authoritative for a fresh install.

> `db_owner` is convenient for dev; tighten to the minimum DML rights before any real
> deployment.

---

## 4. Configure the server (JSON config file)

`ERServer` reads **every secret and tunable from a single JSON config file** (see
[`ERServer/ServerConfig.h`](../ERServer/ServerConfig.h),
[`ERServer/persist/PersistConfig.cpp`](../ERServer/persist/PersistConfig.cpp) and
[`ERServer/ERServer.cpp`](../ERServer/ERServer.cpp)). **No environment variables are
consulted.** On startup the daemon looks for `erserver.config.json` in its working
directory, or the path given by `--config <path>`:

```bat
cd x64\Release
ERServer.exe                         rem loads .\erserver.config.json
ERServer.exe --config C:\secrets\er.json
```

Copy the template and fill it in:

```bat
copy ..\..\Config\erserver.config.example.json x64\Release\erserver.config.json
```

> **This file holds secrets** (`database.password`, `auth.serverPepper`). The real
> `erserver.config.json` is **gitignored** — keep it out of source control and protect it
> with filesystem permissions / a mounted secret. Commit only the `*.example.json` template.

A full config looks like this (every key is optional and falls back to the default shown):

```jsonc
{
  "server": {
    "listenPort": 7777,        // UDP port clients connect to
    "devAuthStub": false       // true = legacy "pick a name" identity, BYPASSES real auth
  },
  "database": {
    // Base connection string WITHOUT credentials; user/password are appended by the
    // server as ;UID=…;PWD=…; for the default SQL-login auth mode.
    "connectionString": "Driver={ODBC Driver 18 for SQL Server};Server=tcp:localhost,1433;Database=EarthRise;Encrypt=yes;TrustServerCertificate=yes",
    "user": "er_app",
    "password": "ChangeMe_Strong!1",
    "authMode": "sql"          // sql | msi | entra
  },
  "auth": {
    "serverPepper": "<stable-base64-pepper>",  // applied in-process, NEVER stored in SQL
    "pbkdf2Iterations": 210000,                // KDF cost (floor 100000)
    "maxLoginFailures": 5,
    "lockoutSeconds": 900,
    "sessionTtlSeconds": 86400
  },
  "durability": {
    "writeBehindRpoMs": 2000,  // position write-behind flush cadence (bounded loss)
    "snapshotMs": 60000        // warm-restart snapshot cadence
  },
  "pool": {
    "min": 1,
    "max": 4                   // ODBC connection-pool sizing
  }
}
```

> **`Encrypt=yes` + the certificate.** Driver 18 encrypts by default and validates the
> server certificate. A default local Developer install only has a **self-signed** cert, so
> use `TrustServerCertificate=yes` for local dev (as above). For anything beyond your own
> machine, install a trusted cert and set `TrustServerCertificate=no` instead.

> **The server pepper.** Auth is PBKDF2-HMAC-SHA512 with a per-user salt **plus** the
> server-side `auth.serverPepper`. Generate a strong random value once and keep it stable
> (changing it invalidates all existing password hashes):
> ```powershell
> [Convert]::ToBase64String((1..32 | ForEach-Object { Get-Random -Maximum 256 }))
> ```
> If `auth.serverPepper` is empty the server still runs but logs a warning and hashes
> **without** a pepper — fine for a throwaway smoke test, **not** for anything you'll keep.

> **No database?** Leave `database.connectionString` empty (or omit the `database`
> section) for a sim-only smoke run with no persistence.

---

## 5. Run the server

From a directory the daemon can write to (it persists its static signing key beside itself):

```bat
cd x64\Release
ERServer.exe
```

On a healthy M5 startup you should see, in order:

- `Crypto self-test ... PASSED` — the CNG handshake path is healthy.
- `Generated + persisted a new static server key to er_server_key.bin` (first run) /
  `Loaded persisted static server key ...` (later runs). It also writes `er_server_pub.bin`
  (the pinned public key clients/bots verify).
- `No warm-restart snapshot found - starting fresh (cold start).` (first run) or a
  `Restored snapshot: tick … bases … ships …` line on later runs.
- `Persistence thread started (write-through outbox + write-behind + snapshot cadence …)`.
- `Listening on UDP 0.0.0.0:7777 via IOCP …`.

**Diagnosing the DB connection:** if `database.connectionString` is set but
unreachable/misconfigured, you'll see `Persistence thread failed to start - degraded
no-persist mode.` If it's **empty/absent** you'll see `No database.connectionString in config
- running WITHOUT persistence`. Both mean the sim runs but
**nothing is saved and login reports `DbUnavailable`** — so for M5 you want neither of those
messages. Fix the connection string / credentials / TCP-1433 reachability until you get
`Persistence thread started`.

Stop the server with **Ctrl+C** — it flushes a final outbox drain + write-behind batch and
joins cleanly (this is the warm-restart path you'll exercise next).

---

## 6. Verify it works

### 6.1 Attach bots (no GPU needed)

In a second console (same machine is fine), run the headless bot driver — the supported way
to validate connection flow, auth, the sim loop, and persistence:

```bat
cd x64\Release
ERHeadless.exe
```

Watch the server console: you should see first-datagram → handshake → `fully connected`
lines, and the periodic `alive - tick …` heartbeat now reporting `persist - outbox depth …,
RPO watermark …, logins … ok` once accounts register/log in.

### 6.2 Confirm persistence in SQL

After bots have registered and played a little:

```bat
sqlcmd -S localhost -E -d EarthRise -Q "SELECT COUNT(*) AS Accounts FROM Accounts; SELECT COUNT(*) AS Bases FROM Bases; SELECT COUNT(*) AS Snapshots FROM SimSnapshots;"
```

You should see accounts, bases, and (after one `ER_SNAPSHOT_MS` interval) at least one
`SimSnapshots` row appear.

### 6.3 The warm-restart drill (the M5 "Done" proof)

1. With bots connected and economy events happening, **Ctrl+C** the server (graceful flush).
2. Start `ERServer.exe` again. It should log `Restored snapshot: tick … bases … ships …`
   followed by `Economy outbox replay: N row(s) after watermark …`.
3. Bots reconnect (backoff/jitter) and resume; the universe + bases + economy come back with
   **zero economy loss**.

> This drill (`docs/implementation/M5-accounts-persistence.md` area I) is the one item the
> design explicitly calls out as needing a real Windows + SQL box — it's the headline thing
> to confirm on first bring-up.

---

## 7. Should you load data into the database? — **No (don't preload).**

Short answer: **apply the schema and start the server. Do not bulk-load any game data, and
do not pre-create accounts or universe rows.** Here's the reasoning, straight from the schema
header and the persistence design:

- **Game data is not in SQL.** Item/hull/module **stats**, crafting **recipes**, research
  **costs/prereqs**, anomaly/invasion **definitions**, the **universe topology** (regions,
  jump-beacons, resource fields — authored in `Config/universe/sol-frontier.universe`, cooked
  via `NeuronTools/datacook`, loaded with `ServerUniverse::LoadUniverse`), and now the **M6
  combat catalog** (hull/module/damage-type/resist stats, authored in
  `NeuronCore/CombatData.h` as `DefaultCombatCatalog`) are all **versioned game data loaded
  into the in-memory sim by `NeuronCore`** — not rows in SQL. The schema header is explicit:
  *"Item stats, hull/module stats, crafting recipes … are GAME DATA … NOT stored here."* So
  there is nothing to "import" into SQL to make the world exist.

- **Player/economy state is created at runtime.** Accounts come from **in-game
  registration**; bases, ships, inventory, wallets, build-queue rows, snapshots, and the
  economy outbox are all written by the server **as the M3 loop produces them**. You start
  from an **empty** database on purpose — a new client converges from an empty baseline.

- **The one nuance (and why it still doesn't block a dev bring-up).** SQL keeps the
  *canonical item-id space* (`ItemDefs`) and `Regions` purely so persisted player rows have
  referential integrity. The schema comments call these *"seeded from game data … before
  go-live,"* but **there is no seed script in the repo yet** — and that gap is now slightly
  more than theoretical: with the M6 combat model implemented, ships are spawned from
  catalog **fits**, so a persisted ship's `Ships.HullItemDefId` is a real foreign key into
  `ItemDefs`. Persisting a fitted ship into an **unseeded** `ItemDefs` would fail that FK.
  The *catalog itself* now exists in-tree (`NeuronCore/CombatData.h`); what's missing is the
  step that **emits the matching `ItemDefs`/`Regions` rows to SQL** — the `datacook`
  text-DSL/seed tool is explicitly **deferred** in the M6 plan (areas A/B). For standing the
  server up now you still **don't** seed by hand: the no-DB/dev-stub smoke run persists
  nothing, and the M5 loop only writes the narrow subset it generates. But **before the live
  M5+M6 persistence drill** (a fitted ship surviving a warm restart on Windows + SQL),
  someone needs to produce that seed — auto-generated from the cooked game data so the SQL
  id-space and the sim's id-space can't drift. Track it as a real pre-drill item, not a
  vague "go-live" one.

**Bottom line:** empty schema → run the server → data populates itself. Loading data now
would be redundant at best and could conflict with the IDENTITY-driven rows the server
creates.

---

## 8. Troubleshooting

| Symptom (server log) | Likely cause / fix |
| --- | --- |
| `Could not load config '…'` | The config file is missing or malformed. Copy `Config/erserver.config.example.json` to `erserver.config.json` in the working dir, or pass `--config <path>`. The message includes the parse error's line:column. |
| `Persistence thread failed to start - degraded no-persist mode.` | DB unreachable or creds wrong. Check TCP/IP enabled + port 1433, `database.user`/`database.password`, and that `er_app` has access to `EarthRise`. |
| `No database.connectionString in config - running WITHOUT persistence` | `database.connectionString` is empty/absent in the config file. |
| Connection fails with a TLS/cert error | Driver 18 validates the cert by default. For local dev add `TrustServerCertificate=yes`; for real hosts install a trusted cert. |
| `Crypto self-test FAILED` | CNG path broken — clients can't complete the handshake. Investigate before anything else (don't proceed). |
| `Failed to start the IOCP UDP listener on port 7777 - is it in use?` | Another process owns the port, or `server.listenPort` collides. Pick a free port. |
| `No datagrams received yet` warning | Client/bot not reaching the server: check Windows Firewall (UDP `server.listenPort`) and that the bot targets the right host:port. For the **UWP** client specifically, loopback needs an exemption (Debug deploys add one). |
| Login reports `DbUnavailable` even though the server is up | You're in no-persist/degraded mode — see the first two rows. Real auth requires a working DB. |
| Want the old "pick a name" identity for a quick non-DB smoke run | Set `"devAuthStub": true` and leave `database.connectionString` empty. (Not M5 persistence — just the loop.) |

---

## 9. Quick start (copy-paste, after the one-time install in §§1–3)

```bat
rem One-time DB prep (§3) is already done, and erserver.config.json is filled in (§4).
cd x64\Release
copy ..\..\Config\erserver.config.example.json erserver.config.json   rem first time only — then edit it
start ERServer.exe
rem ...wait for "Persistence thread started" + "Listening on UDP", then:
ERHeadless.exe
```

> `ERHeadless` defaults to `127.0.0.1:7777` / 3 bots. To point it elsewhere, copy
> `Config/erheadless.config.example.json` to `erheadless.config.json` beside the exe
> (or pass `--config <path>`) and set `server.host` / `server.port`.

---

### References

- [`ERServer/ERServer.cpp`](../ERServer/ERServer.cpp) — startup sequence, config load, warm-restart, logs.
- [`ERServer/ServerConfig.h`](../ERServer/ServerConfig.h) — the JSON config schema + `--config` resolution.
- [`ERServer/persist/PersistConfig.{h,cpp}`](../ERServer/persist/PersistConfig.cpp) — every persist/auth config key + default.
- [`Config/erserver.config.example.json`](../Config/erserver.config.example.json) — the server config template.
- [`NeuronCore/Json.h`](../NeuronCore/Json.h) — the small JSON parser the loaders sit on.
- [`ERServer/persist/OdbcConnection.cpp`](../ERServer/persist/OdbcConnection.cpp) — how the connection string + credentials are assembled.
- [`Config/db/schema.sql`](../Config/db/schema.sql) — canonical schema + the durability/data-boundary notes quoted above.
- [`docs/implementation/M5-accounts-persistence.md`](implementation/M5-accounts-persistence.md) — M5 status (`[~]` = written, Windows-unverified) and the Done gate.
- [`Config/deploy/`](../Config/deploy/) — the container path (intentionally out of scope here).
