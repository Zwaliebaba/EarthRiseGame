#pragma once
// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// PersistConfig.h — central persistence/auth configuration (M5 areas A/C/E/F, §15/§20).
//
// Every secret and tunable the persist layer needs is read from the **JSON config
// file, never hard-coded** (§20): the DB connection string, the App→DB auth mode
// (SQL login now → managed identity / Entra ID on Azure SQL, area J), the server
// pepper (§14), the PBKDF2 iteration count (§14, "high + tunable, stored per hash"),
// and the write-behind RPO / snapshot cadence (§15, §19 open questions). The config
// file replaces the former process-environment loading (the host no longer reads any
// ER_* environment variable); ServerConfig::Load() owns reading the file, and this
// struct is filled from the parsed JSON via FromJson() below.
//
// The struct is plain data so the platform-independent stores can take it by value and
// be unit-tested with literal values; FromJson() is the only config-aware entry point.

#include <cstdint>
#include <string>

#include "Json.h"

namespace Neuron::Persist
{

// App→DB authentication mode (§15 "SQL login now → managed identity / Entra ID").
// area J (Azure SQL migration, §20) needs the managed-identity path to *exist* now so
// the migration is a config flip, not a rewrite. Selected by ER_DB_AUTH.
enum class DbAuthMode : uint8_t
{
    SqlLogin = 0,        // UID/PWD (or full Authentication=SqlPassword) — the M5 default
    ManagedIdentity = 1, // Authentication=ActiveDirectoryMsi (Azure managed identity, M6/area J)
    EntraPassword = 2,   // Authentication=ActiveDirectoryPassword (federated Entra ID, post-launch)
};

// PBKDF2 cost (§14). High + tunable; the SCHEMA-ASSUMPTION note below applies.
inline constexpr uint32_t DEFAULT_PBKDF2_ITERATIONS = 210000; // OWASP-tier first pass for SHA-512
inline constexpr uint32_t MIN_PBKDF2_ITERATIONS     = 100000; // refuse to go below this

// Session token lifetime (§14, expiring session token). 24 h default; refreshed on use.
inline constexpr uint64_t DEFAULT_SESSION_TTL_SECONDS = 24ull * 60ull * 60ull;

// Abuse controls (§14 / R6): per-account + per-IP rate-limit and lockout.
inline constexpr uint32_t DEFAULT_MAX_LOGIN_FAILURES = 5;     // failures before lockout
inline constexpr uint64_t DEFAULT_LOCKOUT_SECONDS   = 15 * 60; // 15 min account lockout

// Durability cadences (§15, §19 open questions — structure here, tuning is a config flip).
inline constexpr uint64_t DEFAULT_WRITE_BEHIND_RPO_MS = 2000;  // ≤ a few seconds of movement loss
inline constexpr uint64_t DEFAULT_SNAPSHOT_MS       = 60000; // warm-restart blob cadence

// Connection-pool sizing for the persistence thread (area A).
inline constexpr int DEFAULT_POOL_MIN = 1;
inline constexpr int DEFAULT_POOL_MAX = 4;

struct PersistConfig
{
    // -- connection (§20: from a secret/env, never hard-coded) ------------------
    // The full ODBC connection string sans credentials/auth, e.g.
    //   "Driver={ODBC Driver 18 for SQL Server};Server=tcp:host,1433;Database=earthrise;Encrypt=yes;TrustServerCertificate=no"
    // OdbcConnection appends the credential/auth fragment for the chosen DbAuthMode.
    std::string connString;       // database.connectionString
    std::string sqlUser;          // database.user  (SqlLogin / EntraPassword)
    std::string sqlPassword;      // database.password (SqlLogin / EntraPassword) — secret
    DbAuthMode  authMode{ DbAuthMode::SqlLogin }; // database.authMode = sql|msi|entra

    // -- auth (§14) -------------------------------------------------------------
    std::string pepper;           // auth.serverPepper — applied in-process, NEVER stored in SQL
    uint32_t    pbkdf2Iterations{ DEFAULT_PBKDF2_ITERATIONS };
    uint32_t    maxLoginFailures{ DEFAULT_MAX_LOGIN_FAILURES };
    uint64_t    lockoutSeconds{ DEFAULT_LOCKOUT_SECONDS };
    uint64_t    sessionTtlSeconds{ DEFAULT_SESSION_TTL_SECONDS };

    // -- durability cadences (§15) ---------------------------------------------
    uint64_t    writeBehindRpoMs{ DEFAULT_WRITE_BEHIND_RPO_MS };
    uint64_t    snapshotMs{ DEFAULT_SNAPSHOT_MS };

    // -- pool ------------------------------------------------------------------
    int         poolMin{ DEFAULT_POOL_MIN };
    int         poolMax{ DEFAULT_POOL_MAX };

    [[nodiscard]] bool HasConnString() const noexcept { return !connString.empty(); }
    [[nodiscard]] bool HasPepper()     const noexcept { return !pepper.empty(); }

    // Fill the persist config from a parsed JSON config document (the whole root
    // object). Reads the "database", "auth", "durability" and "pool" sections; any
    // missing key keeps the default above. With no "database.connectionString" the
    // connString stays empty → callers skip the live persist paths gracefully (the
    // §16.3 "skip if absent" behaviour, e.g. a local smoke run with no DB).
    [[nodiscard]] static PersistConfig FromJson(const Neuron::Json::Value& root);
};

} // namespace Neuron::Persist
