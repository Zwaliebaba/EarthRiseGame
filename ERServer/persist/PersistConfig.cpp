// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// PersistConfig.cpp — environment/secret loading for the persist layer (§20).
//
// Win32-only: uses GetEnvironmentVariableA so secrets come from the process
// environment / a secret store, never source. On a host with none of these set the
// config comes back with an empty connString, which the persist layer treats as
// "no database configured" and skips the live paths (the §16.3 graceful-skip rule).

#include "pch.h"
#include "PersistConfig.h"

#include <cstdlib>
#include <string>

namespace Neuron::Persist
{
namespace
{

// Read a single environment variable into a std::string ("" if unset). A 2-call
// pattern sizes the buffer exactly so long connection strings are not truncated.
std::string EnvStr(const char* name)
{
    const DWORD need = GetEnvironmentVariableA(name, nullptr, 0);
    if (need == 0)
        return {}; // unset (or empty)
    std::string out(need, '\0');
    const DWORD got = GetEnvironmentVariableA(name, out.data(), need);
    if (got == 0 || got >= need)
        return {};
    out.resize(got); // drop the trailing NUL the API counts in 'need'
    return out;
}

uint32_t EnvU32(const char* name, uint32_t fallback)
{
    const std::string v = EnvStr(name);
    if (v.empty())
        return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(v.c_str(), &end, 10);
    if (end == v.c_str())
        return fallback;
    return static_cast<uint32_t>(parsed);
}

uint64_t EnvU64(const char* name, uint64_t fallback)
{
    const std::string v = EnvStr(name);
    if (v.empty())
        return fallback;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(v.c_str(), &end, 10);
    if (end == v.c_str())
        return fallback;
    return static_cast<uint64_t>(parsed);
}

DbAuthMode ParseAuthMode(const std::string& v)
{
    if (v == "msi" || v == "managed" || v == "ActiveDirectoryMsi")
        return DbAuthMode::ManagedIdentity;
    if (v == "entra" || v == "aad" || v == "ActiveDirectoryPassword")
        return DbAuthMode::EntraPassword;
    return DbAuthMode::SqlLogin; // default / "sql"
}

} // namespace

PersistConfig PersistConfig::LoadFromEnv()
{
    PersistConfig c;
    c.connString  = EnvStr("ER_DB_CONNSTR");
    c.sqlUser     = EnvStr("ER_DB_USER");
    c.sqlPassword = EnvStr("ER_DB_PASSWORD");
    c.authMode    = ParseAuthMode(EnvStr("ER_DB_AUTH"));

    c.pepper           = EnvStr("ER_SERVER_PEPPER");
    c.pbkdf2Iterations = EnvU32("ER_PBKDF2_ITERATIONS", kDefaultPbkdf2Iterations);
    if (c.pbkdf2Iterations < kMinPbkdf2Iterations)
        c.pbkdf2Iterations = kMinPbkdf2Iterations; // never silently weaken the KDF
    c.maxLoginFailures  = EnvU32("ER_LOGIN_MAX_FAILURES", kDefaultMaxLoginFailures);
    c.lockoutSeconds    = EnvU64("ER_LOGIN_LOCKOUT_SECONDS", kDefaultLockoutSeconds);
    c.sessionTtlSeconds = EnvU64("ER_SESSION_TTL_SECONDS", kDefaultSessionTtlSeconds);

    c.writeBehindRpoMs = EnvU64("ER_WRITEBEHIND_RPO_MS", kDefaultWriteBehindRpoMs);
    c.snapshotMs       = EnvU64("ER_SNAPSHOT_MS", kDefaultSnapshotMs);

    c.poolMin = static_cast<int>(EnvU32("ER_DB_POOL_MIN", static_cast<uint32_t>(kDefaultPoolMin)));
    c.poolMax = static_cast<int>(EnvU32("ER_DB_POOL_MAX", static_cast<uint32_t>(kDefaultPoolMax)));
    if (c.poolMax < c.poolMin)
        c.poolMax = c.poolMin;

    return c;
}

} // namespace Neuron::Persist
