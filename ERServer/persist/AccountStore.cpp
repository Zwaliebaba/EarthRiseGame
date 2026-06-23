// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// AccountStore.cpp — registration/login implementation (M5 area C, §14).

#include "pch.h"
#include "AccountStore.h"
#include "CngCrypto.h"

#include <algorithm>
#include <cstring>

namespace Neuron::Persist
{
namespace
{

// §14 input bounds (Accounts.Username NVARCHAR(64)). Keep passwords reasonable to
// bound the PBKDF2 input; the hash itself is fixed-size regardless.
constexpr size_t kMaxUsernameChars = 64;
constexpr size_t kMinUsernameChars = 3;
constexpr size_t kMinPasswordChars = 8;
constexpr size_t kMaxPasswordChars = 256;

bool ValidUsername(std::string_view u)
{
    if (u.size() < kMinUsernameChars || u.size() > kMaxUsernameChars)
        return false;
    // Conservative charset to avoid homoglyph/whitespace abuse; the column is NVARCHAR
    // but launch usernames stay ASCII-ish (display names are a separate feature).
    for (char c : u) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok)
            return false;
    }
    return true;
}

bool ValidPassword(std::string_view p)
{
    return p.size() >= kMinPasswordChars && p.size() <= kMaxPasswordChars;
}

} // namespace

std::array<uint8_t, kPwHashBytes>
AccountStore::HashPassword(std::string_view password, std::span<const uint8_t> salt,
                           uint32_t iterations) const
{
    // Append the server pepper to the password (§14: pepper applied in process, never
    // stored). The pure KDF (CngCrypto::Pbkdf2HmacSha512) then matches the portable
    // reference for (password‖pepper, salt, iterations, 64). NOTE: the legacy
    // ICrypto::Pbkdf2 *prepends* a pepper arg — AccountStore deliberately uses the raw
    // method + appends, so the bytes hashed are well-defined and test-cross-checkable.
    std::vector<uint8_t> pw;
    pw.reserve(password.size() + m_cfg.pepper.size());
    pw.insert(pw.end(), password.begin(), password.end());
    pw.insert(pw.end(), m_cfg.pepper.begin(), m_cfg.pepper.end());

    std::array<uint8_t, kPwHashBytes> out{};
    const std::vector<uint8_t> dk =
        m_crypto->Pbkdf2HmacSha512(pw, salt, iterations, kPwHashBytes);
    if (dk.size() == kPwHashBytes)
        std::memcpy(out.data(), dk.data(), kPwHashBytes);
    return out;
}

bool AccountStore::ConstTimeEqual(std::span<const uint8_t> a, std::span<const uint8_t> b)
{
    if (a.size() != b.size())
        return false;
    unsigned diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned>(a[i] ^ b[i]);
    return diff == 0;
}

bool AccountStore::CheckIpRateLimit(std::string_view ip, int64_t nowUnix)
{
    // Fixed-window per-IP limiter (mutex-guarded). Coarse but enough to blunt
    // credential stuffing from one source (§14, R6); the per-account lockout is the
    // primary control. Keyed by the opaque ip string the caller passes.
    std::lock_guard lock(m_mutex);
    IpWindow& w = m_ipWindows[std::string(ip)];
    if (nowUnix - w.windowStartUnix >= kIpWindowSeconds) {
        w.windowStartUnix = nowUnix;
        w.count = 0;
    }
    ++w.count;
    return w.count <= kIpWindowMaxAttempts;
}

AccountStore::AuthCounters AccountStore::Counters() const
{
    std::lock_guard lock(m_mutex);
    return m_counters;
}

// -----------------------------------------------------------------------------
// Register
// -----------------------------------------------------------------------------

AuthResult AccountStore::Register(std::string_view username, std::string_view password,
                                  int64_t nowUnix, SessionInfo& out)
{
    out = SessionInfo{};
    if (!ValidUsername(username) || !ValidPassword(password))
        return AuthResult::BadInput;

    auto lease = m_pool->Acquire();
    if (!lease)
        return AuthResult::DbUnavailable;

    // Per-user random salt (§14). CngCrypto::RandomBytes is the system-preferred RNG.
    std::array<uint8_t, kPwSaltBytes> salt{};
    m_crypto->RandomBytes(salt);
    const auto hash = HashPassword(password, salt, m_cfg.pbkdf2Iterations);

    // Account + zero Wallet in one transaction so a registered account always has a
    // wallet row (EconomyStore's write-through assumes Wallets(AccountId) exists).
    if (!lease->BeginTransaction())
        return AuthResult::DbUnavailable;

    const SqlParam acctParams[] = {
        SqlParam::Make(username),
        SqlParam::Make(std::span<const uint8_t>(hash.data(), hash.size())),
        SqlParam::Make(std::span<const uint8_t>(salt.data(), salt.size())),
        SqlParam::Make(static_cast<int64_t>(m_cfg.pbkdf2Iterations)),
    };
    // The PBKDF2 cost is stored PER ACCOUNT (§14, "high + tunable, stored per hash";
    // Accounts.Pbkdf2Iterations, migration 005) so the global default can be raised
    // later without invalidating existing hashes — Login verifies with the stored cost.
    auto accountId = lease->ExecInsertReturningIdentity(
        "INSERT INTO Accounts (Username, PasswordHash, PasswordSalt, Pbkdf2Iterations) "
        "VALUES (?, ?, ?, ?)",
        acctParams);

    if (!accountId) {
        const bool dup = lease->LastError().IsUniqueViolation();
        (void)lease->Rollback();
        return dup ? AuthResult::UsernameTaken : AuthResult::DbUnavailable;
    }

    const SqlParam walletParams[] = { SqlParam::Make(*accountId) };
    if (!lease->ExecNonQuery("INSERT INTO Wallets (AccountId, Balance) VALUES (?, 0)", walletParams)) {
        (void)lease->Rollback();
        return AuthResult::DbUnavailable;
    }

    if (!lease->Commit()) {
        (void)lease->Rollback();
        return AuthResult::DbUnavailable;
    }

    {
        std::lock_guard lock(m_mutex);
        ++m_counters.registrations;
    }
    out.accountId = *accountId;
    return AuthResult::Ok;
}

// -----------------------------------------------------------------------------
// Login
// -----------------------------------------------------------------------------

AuthResult AccountStore::Login(std::string_view username, std::string_view password,
                               std::string_view clientIp, int64_t nowUnix, SessionInfo& out)
{
    out = SessionInfo{};
    {
        std::lock_guard lock(m_mutex);
        ++m_counters.loginAttempts;
    }
    if (!ValidUsername(username) || !ValidPassword(password)) {
        std::lock_guard lock(m_mutex);
        ++m_counters.loginFailures;
        return AuthResult::BadInput;
    }

    // Per-IP rate limit first (cheapest, blunts stuffing before any DB/CPU work).
    if (!CheckIpRateLimit(clientIp, nowUnix)) {
        std::lock_guard lock(m_mutex);
        ++m_counters.rateLimited;
        return AuthResult::RateLimited;
    }

    auto lease = m_pool->Acquire();
    if (!lease)
        return AuthResult::DbUnavailable;

    // Fetch the account row. Columns: AccountId, PasswordHash, PasswordSalt, Status,
    // LoginFailures, LockedUntil(as unix or NULL→0), TutorialDone.
    const SqlParam sel[] = { SqlParam::Make(username) };
    auto res = lease->ExecQuery(
        "SELECT AccountId, PasswordHash, PasswordSalt, Status, LoginFailures, "
        "       CASE WHEN LockedUntil IS NULL THEN 0 "
        "            ELSE DATEDIFF_BIG(SECOND, '1970-01-01', LockedUntil) END, "
        "       CAST(TutorialDone AS INT), Pbkdf2Iterations "
        "FROM Accounts WHERE Username = ?",
        sel, 8);
    if (!res)
        return AuthResult::DbUnavailable;

    if (res->Empty()) {
        // No such user. Return the SAME code as a wrong password (no user-enumeration
        // oracle, §14/R6). No lockout to track since there is no account.
        std::lock_guard lock(m_mutex);
        ++m_counters.loginFailures;
        return AuthResult::InvalidCredentials;
    }

    const SqlRow& row = res->rows.front();
    auto cellI64 = [&](size_t i) -> int64_t {
        if (i >= row.size() || row[i].isNull || row[i].bytes.size() < sizeof(int64_t))
            return 0;
        int64_t v = 0; std::memcpy(&v, row[i].bytes.data(), sizeof(int64_t)); return v;
    };
    auto cellBytes = [&](size_t i) -> std::span<const uint8_t> {
        if (i >= row.size() || row[i].isNull) return {};
        return { row[i].bytes.data(), row[i].bytes.size() };
    };

    const int64_t accountId   = cellI64(0);
    const auto    storedHash  = cellBytes(1);
    const auto    storedSalt  = cellBytes(2);
    const int64_t status      = cellI64(3);
    const int64_t lockedUntil = cellI64(5);
    const bool    tutorialDone = cellI64(6) != 0;
    const int64_t storedIters = cellI64(7); // per-account PBKDF2 cost (migration 005)

    if (status != 0) // 1=banned 2=suspended
        return AuthResult::AccountBanned;

    if (lockedUntil != 0 && lockedUntil > nowUnix) {
        std::lock_guard lock(m_mutex);
        ++m_counters.lockouts;
        return AuthResult::AccountLocked;
    }

    // Verify the password. Re-hash the candidate with the stored salt + in-process
    // pepper and compare constant-time.
    // Verify with the account's STORED cost (not the current default), so raising the
    // global cost never locks out existing users (§14). Fall back to the configured
    // value only if the column is somehow absent/0.
    const auto candidate = HashPassword(
        password, storedSalt,
        storedIters > 0 ? static_cast<uint32_t>(storedIters) : m_cfg.pbkdf2Iterations);
    const bool match = (storedHash.size() == kPwHashBytes) &&
                       ConstTimeEqual(std::span<const uint8_t>(candidate.data(), candidate.size()),
                                      storedHash);

    if (!match) {
        // Increment failures; lock the account if the threshold is reached (per-account
        // lockout, §14). Done in SQL so concurrent attempts are serialized correctly.
        const SqlParam upd[] = {
            SqlParam::Make(static_cast<int64_t>(m_cfg.maxLoginFailures)),
            SqlParam::MakeDateTimeUtc(nowUnix + static_cast<int64_t>(m_cfg.lockoutSeconds)),
            SqlParam::Make(accountId),
        };
        (void)lease->ExecNonQuery(
            "UPDATE Accounts SET LoginFailures = LoginFailures + 1, "
            "  LockedUntil = CASE WHEN LoginFailures + 1 >= ? THEN ? ELSE LockedUntil END "
            "WHERE AccountId = ?",
            upd);
        std::lock_guard lock(m_mutex);
        ++m_counters.loginFailures;
        return AuthResult::InvalidCredentials;
    }

    // Success: reset failures/lock + stamp last login.
    {
        const SqlParam r[] = { SqlParam::Make(accountId) };
        (void)lease->ExecNonQuery(
            "UPDATE Accounts SET LoginFailures = 0, LockedUntil = NULL, "
            "  LastLoginAt = SYSUTCDATETIME() WHERE AccountId = ?",
            r);
    }

    // One active session per account (§14): revoke any prior live sessions, then issue
    // the new token. The revoke + insert are in one transaction so a reconnect racing
    // the old session's reap binds exactly one session (atomic).
    if (!lease->BeginTransaction())
        return AuthResult::DbUnavailable;

    {
        const SqlParam rv[] = { SqlParam::Make(accountId) };
        if (!lease->ExecNonQuery(
                "UPDATE Sessions SET RevokedAt = SYSUTCDATETIME() "
                "WHERE AccountId = ? AND RevokedAt IS NULL", rv)) {
            (void)lease->Rollback();
            return AuthResult::DbUnavailable;
        }
    }

    SessionToken token{};
    m_crypto->RandomBytes(token);
    const int64_t expiresAt = nowUnix + static_cast<int64_t>(m_cfg.sessionTtlSeconds);

    {
        const SqlParam ins[] = {
            SqlParam::Make(accountId),
            SqlParam::Make(std::span<const uint8_t>(token.data(), token.size())),
            SqlParam::MakeDateTimeUtc(expiresAt),
        };
        if (!lease->ExecNonQuery(
                "INSERT INTO Sessions (AccountId, Token, ExpiresAt) VALUES (?, ?, ?)", ins)) {
            (void)lease->Rollback();
            return AuthResult::DbUnavailable;
        }
    }

    if (!lease->Commit()) {
        (void)lease->Rollback();
        return AuthResult::DbUnavailable;
    }

    // Look up the account's existing base id (NULL/none → 0; ServerHost spawns one).
    int64_t baseId = 0;
    {
        const SqlParam b[] = { SqlParam::Make(accountId) };
        if (auto br = lease->ExecQuery("SELECT BaseId FROM Bases WHERE AccountId = ?", b, 1)) {
            if (!br->Empty() && !br->rows.front().empty() && !br->rows.front().front().isNull) {
                const auto& cell = br->rows.front().front();
                if (cell.bytes.size() >= sizeof(int64_t))
                    std::memcpy(&baseId, cell.bytes.data(), sizeof(int64_t));
            }
        }
    }

    out.accountId     = accountId;
    out.token         = token;
    out.expiresAtUnix = expiresAt;
    out.baseId        = baseId;
    out.tutorialDone  = tutorialDone;

    std::lock_guard lock(m_mutex);
    ++m_counters.loginSuccess;
    return AuthResult::Ok;
}

// -----------------------------------------------------------------------------
// Token validation / revocation / base binding
// -----------------------------------------------------------------------------

std::optional<SessionInfo> AccountStore::ValidateToken(const SessionToken& token, int64_t nowUnix)
{
    auto lease = m_pool->Acquire();
    if (!lease)
        return std::nullopt;

    const SqlParam p[] = { SqlParam::Make(std::span<const uint8_t>(token.data(), token.size())) };
    auto res = lease->ExecQuery(
        "SELECT s.AccountId, "
        "       DATEDIFF_BIG(SECOND, '1970-01-01', s.ExpiresAt), "
        "       ISNULL(b.BaseId, 0), "
        "       CAST(a.TutorialDone AS INT) "
        "FROM Sessions s "
        "JOIN Accounts a ON a.AccountId = s.AccountId "
        "LEFT JOIN Bases b ON b.AccountId = s.AccountId "
        "WHERE s.Token = ? AND s.RevokedAt IS NULL AND s.ExpiresAt > SYSUTCDATETIME()",
        p, 4);
    if (!res || res->Empty())
        return std::nullopt;

    const SqlRow& row = res->rows.front();
    auto i64 = [&](size_t i) -> int64_t {
        if (i >= row.size() || row[i].isNull || row[i].bytes.size() < sizeof(int64_t)) return 0;
        int64_t v = 0; std::memcpy(&v, row[i].bytes.data(), sizeof(int64_t)); return v;
    };

    SessionInfo info;
    info.accountId     = i64(0);
    info.token         = token;
    info.expiresAtUnix = i64(1);
    info.baseId        = i64(2);
    info.tutorialDone  = i64(3) != 0;
    if (info.accountId == 0)
        return std::nullopt;
    (void)nowUnix; // expiry is enforced in SQL above (server clock is canonical)
    return info;
}

bool AccountStore::RevokeToken(const SessionToken& token, int64_t /*nowUnix*/)
{
    auto lease = m_pool->Acquire();
    if (!lease)
        return false;
    const SqlParam p[] = { SqlParam::Make(std::span<const uint8_t>(token.data(), token.size())) };
    auto n = lease->ExecNonQuery(
        "UPDATE Sessions SET RevokedAt = SYSUTCDATETIME() WHERE Token = ? AND RevokedAt IS NULL", p);
    return n.has_value();
}

bool AccountStore::SetAccountBase(int64_t /*accountId*/, int64_t /*baseId*/)
{
    // The Bases row is owned/written by the sim's write-behind path (EconomyStore /
    // PersistenceThread spawn a Bases row keyed by AccountId on first login). The
    // session's base binding is derived by JOIN at ValidateToken time, so there is no
    // separate column to set here — this is a no-op kept for API symmetry with the
    // ServerHost wiring (it may call it after spawn). Returns true (nothing to do).
    return true;
}

} // namespace Neuron::Persist
