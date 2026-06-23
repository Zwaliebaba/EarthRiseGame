#pragma once
// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// PersistConfig.h — central persistence/auth configuration (M5 areas A/C/E/F, §15/§20).
//
// Every secret and tunable the persist layer needs is read from the **environment /
// a secret store, never hard-coded** (§20): the DB connection string, the App→DB auth
// mode (SQL login now → managed identity / Entra ID on Azure SQL, area J), the server
// pepper (§14), the PBKDF2 iteration count (§14, "high + tunable, stored per hash"),
// and the write-behind RPO / snapshot cadence (§15, §19 open questions).
//
// The struct is plain data so the platform-independent stores can take it by value and
// be unit-tested with literal values; LoadFromEnv() (in PersistConfig.cpp, Win32) is
// the only piece that touches the process environment.

#include <cstdint>
#include <string>

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
inline constexpr uint32_t kDefaultPbkdf2Iterations = 210000; // OWASP-tier first pass for SHA-512
inline constexpr uint32_t kMinPbkdf2Iterations     = 100000; // refuse to go below this

// Session token lifetime (§14, expiring session token). 24 h default; refreshed on use.
inline constexpr uint64_t kDefaultSessionTtlSeconds = 24ull * 60ull * 60ull;

// Abuse controls (§14 / R6): per-account + per-IP rate-limit and lockout.
inline constexpr uint32_t kDefaultMaxLoginFailures = 5;     // failures before lockout
inline constexpr uint64_t kDefaultLockoutSeconds   = 15 * 60; // 15 min account lockout

// Durability cadences (§15, §19 open questions — structure here, tuning is a config flip).
inline constexpr uint64_t kDefaultWriteBehindRpoMs = 2000;  // ≤ a few seconds of movement loss
inline constexpr uint64_t kDefaultSnapshotMs       = 60000; // warm-restart blob cadence

// Connection-pool sizing for the persistence thread (area A).
inline constexpr int kDefaultPoolMin = 1;
inline constexpr int kDefaultPoolMax = 4;

struct PersistConfig
{
    // -- connection (§20: from a secret/env, never hard-coded) ------------------
    // The full ODBC connection string sans credentials/auth, e.g.
    //   "Driver={ODBC Driver 18 for SQL Server};Server=tcp:host,1433;Database=earthrise;Encrypt=yes;TrustServerCertificate=no"
    // OdbcConnection appends the credential/auth fragment for the chosen DbAuthMode.
    std::string connString;       // ER_DB_CONNSTR
    std::string sqlUser;          // ER_DB_USER  (SqlLogin / EntraPassword)
    std::string sqlPassword;      // ER_DB_PASSWORD (SqlLogin / EntraPassword) — secret
    DbAuthMode  authMode{ DbAuthMode::SqlLogin }; // ER_DB_AUTH = sql|msi|entra

    // -- auth (§14) -------------------------------------------------------------
    std::string pepper;           // ER_SERVER_PEPPER — applied in-process, NEVER stored in SQL
    uint32_t    pbkdf2Iterations{ kDefaultPbkdf2Iterations };
    uint32_t    maxLoginFailures{ kDefaultMaxLoginFailures };
    uint64_t    lockoutSeconds{ kDefaultLockoutSeconds };
    uint64_t    sessionTtlSeconds{ kDefaultSessionTtlSeconds };

    // -- durability cadences (§15) ---------------------------------------------
    uint64_t    writeBehindRpoMs{ kDefaultWriteBehindRpoMs };
    uint64_t    snapshotMs{ kDefaultSnapshotMs };

    // -- pool ------------------------------------------------------------------
    int         poolMin{ kDefaultPoolMin };
    int         poolMax{ kDefaultPoolMax };

    [[nodiscard]] bool HasConnString() const noexcept { return !connString.empty(); }
    [[nodiscard]] bool HasPepper()     const noexcept { return !pepper.empty(); }

    // Read the whole config from the process environment (Win32, in the .cpp). On a
    // pure-Linux/CI run with no DB the connString is empty → callers skip the live
    // persist paths gracefully (the §16.3 "skip if absent" behaviour).
    [[nodiscard]] static PersistConfig LoadFromEnv();
};

} // namespace Neuron::Persist
