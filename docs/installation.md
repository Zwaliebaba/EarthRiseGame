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

## 4. Configure the server (environment variables)

`ERServer` reads **every secret and tunable from the process environment** (see
[`ERServer/persist/PersistConfig.cpp`](../ERServer/persist/PersistConfig.cpp) and
[`ERServer/ERServer.cpp`](../ERServer/ERServer.cpp)). Nothing is hard-coded or read from a
config file.

### 4.1 The connection string is assembled in two pieces

- `ER_DB_CONNSTR` is the **base** string **without credentials**.
- `ER_DB_USER` + `ER_DB_PASSWORD` are appended by the server as `;UID=…;PWD=…;` for the
  default SQL-login auth mode (`OdbcConnection::BuildAuthFragment`).

So set them separately:

```bat
rem --- Database connection (base string has NO credentials) ---
set "ER_DB_CONNSTR=Driver={ODBC Driver 18 for SQL Server};Server=tcp:localhost,1433;Database=EarthRise;Encrypt=yes;TrustServerCertificate=yes"
set "ER_DB_USER=er_app"
set "ER_DB_PASSWORD=ChangeMe_Strong!1"
rem ER_DB_AUTH defaults to "sql" (SQL login); leave unset for dev.
```

> **`Encrypt=yes` + the certificate.** Driver 18 encrypts by default and validates the
> server certificate. A default local Developer install only has a **self-signed** cert, so
> use `TrustServerCertificate=yes` for local dev (as above). For anything beyond your own
> machine, install a trusted cert and set `TrustServerCertificate=no` instead.

### 4.2 The server pepper (required for real auth)

Auth is PBKDF2-HMAC-SHA512 with a per-user salt **plus a server-side pepper applied
in-process and never stored in SQL**. Generate a strong random value once and keep it stable
(changing it invalidates all existing password hashes):

```powershell
# PowerShell — 32 random bytes, base64
[Convert]::ToBase64String((1..32 | ForEach-Object { Get-Random -Maximum 256 }))
```

```bat
set "ER_SERVER_PEPPER=<paste-the-generated-value>"
```

> If `ER_SERVER_PEPPER` is unset, the server still runs but logs a warning and hashes
> **without** a pepper — fine for a throwaway smoke test, **not** for anything you'll keep.

### 4.3 Other useful variables (all optional, sane defaults)

| Variable | Default | Meaning |
| --- | --- | --- |
| `ER_LISTEN_PORT` | `7777` | UDP port clients connect to |
| `ER_DB_AUTH` | `sql` | `sql` \| `msi` \| `entra` (use `sql` for this guide) |
| `ER_PBKDF2_ITERATIONS` | `210000` | KDF cost (floor `100000`); raise to taste |
| `ER_SESSION_TTL_SECONDS` | `86400` | Session-token lifetime |
| `ER_LOGIN_MAX_FAILURES` | `5` | Failures before lockout |
| `ER_LOGIN_LOCKOUT_SECONDS` | `900` | Lockout duration |
| `ER_WRITEBEHIND_RPO_MS` | `2000` | Position write-behind flush cadence (bounded loss) |
| `ER_SNAPSHOT_MS` | `60000` | Warm-restart snapshot cadence |
| `ER_DB_POOL_MIN` / `ER_DB_POOL_MAX` | `1` / `4` | ODBC connection-pool sizing |
| `ER_DEV_AUTH_STUB` | unset (off) | `1` = legacy "pick a name" identity, **bypasses real auth** — leave OFF for M5 |

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

**Diagnosing the DB connection:** if `ER_DB_CONNSTR` is set but unreachable/misconfigured,
you'll see `Persistence thread failed to start - degraded no-persist mode.` If it's **unset**
you'll see `No ER_DB_CONNSTR set - running WITHOUT persistence`. Both mean the sim runs but
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
  **costs/prereqs**, anomaly/invasion **definitions**, and the **universe topology** (regions,
  jump-beacons, resource fields — authored in `Config/universe/sol-frontier.universe`) are
  **versioned game data loaded into the in-memory sim by `NeuronCore`** (cooked via
  `NeuronTools/datacook`, loaded with `ServerUniverse::LoadUniverse`). The schema header is
  explicit: *"Item stats, hull/module stats, crafting recipes … are GAME DATA … NOT stored
  here."* So there is nothing to "import" into SQL to make the world exist.

- **Player/economy state is created at runtime.** Accounts come from **in-game
  registration**; bases, ships, inventory, wallets, build-queue rows, snapshots, and the
  economy outbox are all written by the server **as the M3 loop produces them**. You start
  from an **empty** database on purpose — a new client converges from an empty baseline.

- **The one nuance (and why it still doesn't block you).** SQL keeps the *canonical item-id
  space* (`ItemDefs`) and `Regions` purely so persisted player rows have referential
  integrity. The schema comments call these *"seeded from game data … before go-live,"* but
  **there is no seed script in the repo yet**, and M5 only persists the subset of rows the M3
  loop actually generates. So for standing the server up now you **don't** need to seed them.
  Producing an `ItemDefs`/`Regions` seed (derived from the cooked game data) is a **pre-go-live
  task**, not a startup step — flag it for whoever owns the M6/M7 data-cook pipeline.

**Bottom line:** empty schema → run the server → data populates itself. Loading data now
would be redundant at best and could conflict with the IDENTITY-driven rows the server
creates.

---

## 8. Troubleshooting

| Symptom (server log) | Likely cause / fix |
| --- | --- |
| `Persistence thread failed to start - degraded no-persist mode.` | DB unreachable or creds wrong. Check TCP/IP enabled + port 1433, `ER_DB_USER`/`ER_DB_PASSWORD`, and that `er_app` has access to `EarthRise`. |
| `No ER_DB_CONNSTR set - running WITHOUT persistence` | `ER_DB_CONNSTR` not set in this console. Env vars set with `set` only apply to the current `cmd` session. |
| Connection fails with a TLS/cert error | Driver 18 validates the cert by default. For local dev add `TrustServerCertificate=yes`; for real hosts install a trusted cert. |
| `Crypto self-test FAILED` | CNG path broken — clients can't complete the handshake. Investigate before anything else (don't proceed). |
| `Failed to start the IOCP UDP listener on port 7777 - is it in use?` | Another process owns the port, or `ER_LISTEN_PORT` collides. Pick a free port. |
| `No datagrams received yet` warning | Client/bot not reaching the server: check Windows Firewall (UDP `ER_LISTEN_PORT`) and that the bot targets the right host:port. For the **UWP** client specifically, loopback needs an exemption (Debug deploys add one). |
| Login reports `DbUnavailable` even though the server is up | You're in no-persist/degraded mode — see the first two rows. Real auth requires a working DB. |
| Want the old "pick a name" identity for a quick non-DB smoke run | Set `ER_DEV_AUTH_STUB=1` and leave `ER_DB_CONNSTR` unset. (Not M5 persistence — just the loop.) |

---

## 9. Quick start (copy-paste, after the one-time install in §§1–3)

```bat
rem One-time DB prep (§3) is already done. Each new console:
set "ER_DB_CONNSTR=Driver={ODBC Driver 18 for SQL Server};Server=tcp:localhost,1433;Database=EarthRise;Encrypt=yes;TrustServerCertificate=yes"
set "ER_DB_USER=er_app"
set "ER_DB_PASSWORD=ChangeMe_Strong!1"
set "ER_SERVER_PEPPER=<your-stable-base64-pepper>"

cd x64\Release
start ERServer.exe
rem ...wait for "Persistence thread started" + "Listening on UDP", then:
ERHeadless.exe
```

---

### References

- [`ERServer/ERServer.cpp`](../ERServer/ERServer.cpp) — startup sequence, env reads, warm-restart, logs.
- [`ERServer/persist/PersistConfig.{h,cpp}`](../ERServer/persist/PersistConfig.cpp) — every env var + default.
- [`ERServer/persist/OdbcConnection.cpp`](../ERServer/persist/OdbcConnection.cpp) — how the connection string + credentials are assembled.
- [`Config/db/schema.sql`](../Config/db/schema.sql) — canonical schema + the durability/data-boundary notes quoted above.
- [`docs/implementation/M5-accounts-persistence.md`](implementation/M5-accounts-persistence.md) — M5 status (`[~]` = written, Windows-unverified) and the Done gate.
- [`Config/deploy/`](../Config/deploy/) — the container path (intentionally out of scope here).
