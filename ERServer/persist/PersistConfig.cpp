// PersistConfig.cpp — JSON config loading for the persist layer (§20).
//
// Reads the persistence/auth tunables from the parsed JSON config document so
// secrets come from the config file (a gitignored file / mounted secret), never
// from source. On a host whose config has no database.connectionString the
// returned config has an empty connString, which the persist layer treats as
// "no database configured" and skips the live paths (the §16.3 graceful-skip rule).
//
// This file is now platform-independent (no Win32) — the config arrives already
// parsed; ServerConfig::Load() owns reading the file off disk.

#include "pch.h"
#include "PersistConfig.h"

namespace Neuron::Persist
{
namespace
{

DbAuthMode ParseAuthMode(const std::string& v)
{
    if (v == "msi" || v == "managed" || v == "ActiveDirectoryMsi")
        return DbAuthMode::ManagedIdentity;
    if (v == "entra" || v == "aad" || v == "ActiveDirectoryPassword")
        return DbAuthMode::EntraPassword;
    return DbAuthMode::SqlLogin; // default / "sql"
}

} // namespace

PersistConfig PersistConfig::FromJson(const Neuron::Json::Value& root)
{
    PersistConfig c;

    const Neuron::Json::Value& db = root["database"];
    c.connString  = db.getString("connectionString");
    c.sqlUser     = db.getString("user");
    c.sqlPassword = db.getString("password");
    c.authMode    = ParseAuthMode(db.getString("authMode", "sql"));

    const Neuron::Json::Value& auth = root["auth"];
    c.pepper           = auth.getString("serverPepper");
    c.pbkdf2Iterations = auth.getUint32("pbkdf2Iterations", DEFAULT_PBKDF2_ITERATIONS);
    if (c.pbkdf2Iterations < MIN_PBKDF2_ITERATIONS)
        c.pbkdf2Iterations = MIN_PBKDF2_ITERATIONS; // never silently weaken the KDF
    c.maxLoginFailures  = auth.getUint32("maxLoginFailures", DEFAULT_MAX_LOGIN_FAILURES);
    c.lockoutSeconds    = auth.getUint64("lockoutSeconds", DEFAULT_LOCKOUT_SECONDS);
    c.sessionTtlSeconds = auth.getUint64("sessionTtlSeconds", DEFAULT_SESSION_TTL_SECONDS);

    const Neuron::Json::Value& dur = root["durability"];
    c.writeBehindRpoMs = dur.getUint64("writeBehindRpoMs", DEFAULT_WRITE_BEHIND_RPO_MS);
    c.snapshotMs       = dur.getUint64("snapshotMs", DEFAULT_SNAPSHOT_MS);

    const Neuron::Json::Value& pool = root["pool"];
    c.poolMin = pool.getInt("min", DEFAULT_POOL_MIN);
    c.poolMax = pool.getInt("max", DEFAULT_POOL_MAX);
    if (c.poolMax < c.poolMin)
        c.poolMax = c.poolMin;

    return c;
}

} // namespace Neuron::Persist
