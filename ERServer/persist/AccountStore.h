#pragma once
// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// AccountStore.h — registration + login against Accounts/Sessions (M5 area C, §14).
//
// Replaces the dev "pick a name" stub (ServerHost) with real auth over the existing
// encrypted, server-authenticated channel (§8.5 — credentials are only sent after
// ECDH + server-key verify; this store is what the server calls once the plaintext
// username/password arrive on the secure channel).
//
// Hashing (§14): PBKDF2-HMAC-SHA512 via the new CngCrypto::Pbkdf2HmacSha512 (which is
// byte-identical to the verified reference). A per-user random salt (Accounts.Salt) +
// a server-side pepper (PersistConfig.pepper, from ER_SERVER_PEPPER — applied in
// process, NEVER stored) are mixed in, with a high, tunable iteration count.
//
// Abuse controls (§14, R6): per-account lockout via Accounts.LoginFailures/LockedUntil
// AND a per-IP rate-limiter (in-process, since IPs are not an Accounts column). One
// active session per account: a successful login revokes the account's prior sessions
// before issuing the new token (the duplicate is kicked; reconnect is handled
// atomically by re-binding the same account, §14).
//
// SCHEMA-ASSUMPTION (iteration count): Accounts has PasswordHash VARBINARY(64) +
// PasswordSalt VARBINARY(32) but NO iterations column (schema.sql / migration 001).
// §14 wants the cost "stored per hash so it can be raised later." Until a migration
// adds an Iterations column, this store uses the configured constant
// (PersistConfig.pbkdf2Iterations) for ALL hashes and records a TODO for a possible
// migration 005 (do NOT edit Config/db here). Raising the cost without that column is
// a one-time global rehash-on-next-login, not a per-hash bump — see VerifyAndMaybeRehash.

#include "OdbcConnectionPool.h"
#include "PersistConfig.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Forward-declare the production crypto so this header stays light; the .cpp includes
// CngCrypto.h. AccountStore uses it for Pbkdf2HmacSha512 + RandomBytes only.
namespace Neuron::Net { class CngCrypto; }

namespace Neuron::Persist
{

inline constexpr size_t kSessionTokenBytes = 32; // Sessions.Token BINARY(32)
inline constexpr size_t kPwHashBytes       = 64; // Accounts.PasswordHash VARBINARY(64)
inline constexpr size_t kPwSaltBytes       = 32; // Accounts.PasswordSalt VARBINARY(32)

using SessionToken = std::array<uint8_t, kSessionTokenBytes>;

// Outcome of a register/login attempt — distinct codes so the server can map each to
// the right wire response + the §21 auth counters (area H).
enum class AuthResult : uint8_t
{
    Ok = 0,
    UsernameTaken,        // register: UQ_Accounts_Username violation
    InvalidCredentials,   // login: no such user / wrong password (same message — no oracle)
    AccountLocked,        // LockedUntil in the future (per-account lockout)
    RateLimited,          // per-IP rate limiter tripped
    AccountBanned,        // Accounts.Status = banned/suspended
    DbUnavailable,        // pool could not give a connection (degraded mode)
    BadInput,             // empty username/password, too long, etc.
};

// A logged-in session the server binds to the connection (§14 "binds the session to
// the player's Base/entity"). baseId is 0 until the base is spawned/restored.
struct SessionInfo
{
    int64_t      accountId{ 0 };
    SessionToken token{};
    int64_t      expiresAtUnix{ 0 };
    int64_t      baseId{ 0 };       // 0 = no base yet (first login → spawn; else restore)
    bool         tutorialDone{ false };
};

class AccountStore
{
public:
    // 'pool' and 'crypto' must outlive the store. 'crypto' must be Initialized().
    AccountStore(OdbcConnectionPool* pool, Neuron::Net::CngCrypto* crypto, PersistConfig cfg)
        : m_pool(pool), m_crypto(crypto), m_cfg(std::move(cfg)) {}

    AccountStore(const AccountStore&) = delete;
    AccountStore& operator=(const AccountStore&) = delete;

    // Register a new account: validate input, generate salt, hash (pepper appended in
    // process), INSERT Accounts (+ a zero Wallet row in the same transaction). On
    // success 'out' carries the new accountId; the caller then logs in / spawns.
    [[nodiscard]] AuthResult Register(std::string_view username, std::string_view password,
                                      int64_t nowUnix, SessionInfo& out);

    // Log in: enforce per-IP rate limit + per-account lockout, verify the password,
    // reset failures, revoke prior sessions (one-active-session rule), issue a new
    // expiring token, and return the account's base id (0 if none yet). 'clientIp' is
    // an opaque key (e.g. "1.2.3.4") for the per-IP limiter.
    [[nodiscard]] AuthResult Login(std::string_view username, std::string_view password,
                                   std::string_view clientIp, int64_t nowUnix, SessionInfo& out);

    // Validate a session token presented on reliable traffic (reconnect / per-request).
    // Returns the bound account+base if the token exists, is unrevoked and unexpired.
    [[nodiscard]] std::optional<SessionInfo> ValidateToken(const SessionToken& token, int64_t nowUnix);

    // Revoke a single session (logout / kick the duplicate) by token.
    [[nodiscard]] bool RevokeToken(const SessionToken& token, int64_t nowUnix);

    // Bind the player's base id to a logged-in account (called once ServerHost has
    // spawned/restored the base — §14 "login binds the session to the Base/entity").
    [[nodiscard]] bool SetAccountBase(int64_t accountId, int64_t baseId);

    // --- §21 auth telemetry (area H) -----------------------------------------
    struct AuthCounters
    {
        uint64_t loginAttempts{ 0 };
        uint64_t loginSuccess{ 0 };
        uint64_t loginFailures{ 0 };
        uint64_t lockouts{ 0 };      // attempts rejected because the account was locked
        uint64_t rateLimited{ 0 };   // attempts rejected by the per-IP limiter
        uint64_t registrations{ 0 };
    };
    [[nodiscard]] AuthCounters Counters() const;

private:
    // Hash a password with the per-user salt + the in-process pepper, 'iterations'
    // rounds, into a kPwHashBytes buffer via CngCrypto::Pbkdf2HmacSha512.
    std::array<uint8_t, kPwHashBytes> HashPassword(std::string_view password,
                                                   std::span<const uint8_t> salt,
                                                   uint32_t iterations) const;

    // Constant-time hash comparison (no early-out timing oracle).
    static bool ConstTimeEqual(std::span<const uint8_t> a, std::span<const uint8_t> b);

    // Per-IP fixed-window rate limiter. Returns false if the IP is over budget.
    bool CheckIpRateLimit(std::string_view ip, int64_t nowUnix);

    OdbcConnectionPool*    m_pool{ nullptr };
    Neuron::Net::CngCrypto* m_crypto{ nullptr };
    PersistConfig          m_cfg;

    mutable std::mutex     m_mutex;        // guards counters + the per-IP limiter
    AuthCounters           m_counters;

    struct IpWindow { int64_t windowStartUnix{ 0 }; uint32_t count{ 0 }; };
    std::unordered_map<std::string, IpWindow> m_ipWindows;
    static constexpr int64_t  kIpWindowSeconds = 60;
    static constexpr uint32_t kIpWindowMaxAttempts = 20; // per-IP login attempts / minute
};

} // namespace Neuron::Persist
