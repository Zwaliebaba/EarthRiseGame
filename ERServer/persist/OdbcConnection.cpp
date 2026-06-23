// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// OdbcConnection.cpp — ODBC Driver 18 wrapper implementation (M5 area A, §15/§20).
//
// LINK NOTE: requires odbc32.lib (and odbccp32.lib). ERServer.vcxproj already links
// both. Windows-only by design — the cross-platform sim/persist *model* (Outbox.h,
// WarmRestart.h, Pbkdf2.h) is what the Linux testrunner exercises; this TU is the SQL
// side validated on the Windows build agent against the dev SQL Server.
//
// API references (Microsoft ODBC):
//   SQLAllocHandle / SQLFreeHandle / SQLSetEnvAttr(SQL_ATTR_ODBC_VERSION)
//   SQLDriverConnect (Driver 18 keyword connection string, Encrypt=yes)
//   SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT) for transactions; SQLEndTran
//   SQLBindParameter / SQLExecDirect / SQLNumResultCols / SQLFetch / SQLGetData
//   SQLRowCount; SQLGetDiagRec for error mapping
//   Azure SQL managed identity: Authentication=ActiveDirectoryMsi (Driver 18)
#include "pch.h"
#include "OdbcConnection.h"

#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "odbc32.lib")

namespace Neuron::Persist
{
namespace
{

inline bool SqlOk(SQLRETURN r) noexcept
{
    return r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO;
}

inline SQLHANDLE AsHandle(void* p) noexcept { return reinterpret_cast<SQLHANDLE>(p); }
inline SQLHENV   AsEnv(void* p)    noexcept { return reinterpret_cast<SQLHENV>(p); }
inline SQLHDBC   AsDbc(void* p)    noexcept { return reinterpret_cast<SQLHDBC>(p); }
inline SQLHSTMT  AsStmt(void* p)   noexcept { return reinterpret_cast<SQLHSTMT>(p); }

// UTF-8 → UTF-16 for NVARCHAR parameters. ODBC's wide entry points (SQLExecDirectW,
// SQL_C_WCHAR) take UTF-16; the rest of the engine speaks UTF-8.
std::wstring Widen(std::string_view s)
{
    if (s.empty())
        return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), need);
    return w;
}

std::string Narrow(const wchar_t* w, int wlen)
{
    if (wlen <= 0)
        return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, wlen, s.data(), need, nullptr, nullptr);
    return s;
}

} // namespace

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

OdbcConnection::~OdbcConnection()
{
    Disconnect();
    FreeHandles();
}

OdbcConnection::OdbcConnection(OdbcConnection&& o) noexcept
    : m_env(o.m_env), m_dbc(o.m_dbc), m_connected(o.m_connected),
      m_inTxn(o.m_inTxn), m_lastError(std::move(o.m_lastError))
{
    o.m_env = nullptr;
    o.m_dbc = nullptr;
    o.m_connected = false;
    o.m_inTxn = false;
}

OdbcConnection& OdbcConnection::operator=(OdbcConnection&& o) noexcept
{
    if (this != &o) {
        Disconnect();
        FreeHandles();
        m_env = o.m_env; m_dbc = o.m_dbc; m_connected = o.m_connected;
        m_inTxn = o.m_inTxn; m_lastError = std::move(o.m_lastError);
        o.m_env = nullptr; o.m_dbc = nullptr; o.m_connected = false; o.m_inTxn = false;
    }
    return *this;
}

void OdbcConnection::FreeHandles() noexcept
{
    if (m_dbc) { SQLFreeHandle(SQL_HANDLE_DBC, AsDbc(m_dbc)); m_dbc = nullptr; }
    if (m_env) { SQLFreeHandle(SQL_HANDLE_ENV, AsEnv(m_env)); m_env = nullptr; }
}

std::string OdbcConnection::BuildAuthFragment(const PersistConfig& cfg)
{
    // The credential/auth fragment appended to the base connection string. Selected by
    // config so the SAME binary runs against a SQL login now and a managed identity on
    // Azure SQL later (area J) — a config flip, not a rewrite (§15 App→DB auth).
    switch (cfg.authMode) {
    case DbAuthMode::ManagedIdentity:
        // System-assigned managed identity — no secret in the string at all. (A
        // user-assigned identity would add "UID={client-id};".) Driver 18 obtains the
        // token from the Azure IMDS endpoint.
        return ";Authentication=ActiveDirectoryMsi;";
    case DbAuthMode::EntraPassword:
        return ";Authentication=ActiveDirectoryPassword;UID=" + cfg.sqlUser +
               ";PWD=" + cfg.sqlPassword + ";";
    case DbAuthMode::SqlLogin:
    default:
        // Plain SQL login. Encrypt=yes is expected in cfg.connString already (§15).
        return ";UID=" + cfg.sqlUser + ";PWD=" + cfg.sqlPassword + ";";
    }
}

bool OdbcConnection::Connect(const PersistConfig& cfg)
{
    if (m_connected)
        return true;
    if (!cfg.HasConnString()) {
        m_lastError = { "08001", 0, "no connection string configured (ER_DB_CONNSTR unset)" };
        return false;
    }

    SQLHENV env = nullptr;
    if (!SqlOk(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env))) {
        m_lastError = { "HY000", 0, "SQLAllocHandle(ENV) failed" };
        return false;
    }
    m_env = env;
    // ODBC 3.80 — required before allocating the DBC handle.
    if (!SqlOk(SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION,
                             reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3_80), 0))) {
        CaptureDiag(SQL_HANDLE_ENV, env);
        FreeHandles();
        return false;
    }

    SQLHDBC dbc = nullptr;
    if (!SqlOk(SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc))) {
        CaptureDiag(SQL_HANDLE_ENV, env);
        FreeHandles();
        return false;
    }
    m_dbc = dbc;

    // Compose the full connection string: base (Driver=...;Server=...;Encrypt=yes) +
    // the auth fragment for the chosen mode. Secrets come from cfg (env/secret store).
    const std::string full = cfg.connString + BuildAuthFragment(cfg);
    const std::wstring wfull = Widen(full);

    SQLSMALLINT outLen = 0;
    std::vector<SQLWCHAR> outConn(1024);
    const SQLRETURN r = SQLDriverConnectW(
        dbc, nullptr,
        reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(wfull.c_str())),
        SQL_NTS,
        outConn.data(), static_cast<SQLSMALLINT>(outConn.size()), &outLen,
        SQL_DRIVER_NOPROMPT);

    if (!SqlOk(r)) {
        CaptureDiag(SQL_HANDLE_DBC, dbc);
        // Leave handles allocated so the caller can read LastError(); Disconnect/dtor frees.
        m_connected = false;
        return false;
    }

    m_connected = true;
    m_inTxn = false;
    m_lastError = {};
    return true;
}

void OdbcConnection::Disconnect()
{
    if (m_connected && m_dbc) {
        if (m_inTxn) {
            SQLEndTran(SQL_HANDLE_DBC, AsDbc(m_dbc), SQL_ROLLBACK);
            m_inTxn = false;
        }
        SQLDisconnect(AsDbc(m_dbc));
    }
    m_connected = false;
}

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------

void OdbcConnection::CaptureDiag(int handleType, void* handle)
{
    SQLWCHAR    state[6]{};
    SQLINTEGER  native = 0;
    SQLWCHAR    msg[1024]{};
    SQLSMALLINT msgLen = 0;

    // First diagnostic record is the most actionable; deeper records exist but one is
    // enough for the operational log + the unique-violation classification (area D).
    const SQLRETURN r = SQLGetDiagRecW(static_cast<SQLSMALLINT>(handleType), AsHandle(handle), 1,
                                       state, &native, msg,
                                       static_cast<SQLSMALLINT>(std::size(msg)), &msgLen);
    if (SqlOk(r)) {
        m_lastError.sqlState = Narrow(reinterpret_cast<const wchar_t*>(state), 5);
        m_lastError.native   = static_cast<int32_t>(native);
        m_lastError.message  = Narrow(reinterpret_cast<const wchar_t*>(msg),
                                      msgLen > 0 ? msgLen : 0);
    } else {
        m_lastError = { "HY000", 0, "SQLGetDiagRec returned no record" };
    }
}

void* OdbcConnection::AllocStmt()
{
    if (!m_connected || !m_dbc)
        return nullptr;
    SQLHSTMT stmt = nullptr;
    if (!SqlOk(SQLAllocHandle(SQL_HANDLE_STMT, AsDbc(m_dbc), &stmt))) {
        CaptureDiag(SQL_HANDLE_DBC, m_dbc);
        return nullptr;
    }
    return stmt;
}

// -----------------------------------------------------------------------------
// Parameter binding helpers
// -----------------------------------------------------------------------------
namespace
{

// Bound-parameter backing storage kept alive for the duration of an Execute call.
// ODBC reads the bound buffers at SQLExecDirect time, so these must outlive the call.
struct ParamStorage
{
    std::vector<std::wstring>  wide;     // widened NVARCHAR params
    std::vector<SQLLEN>        lenInd;   // per-param StrLen_or_IndPtr
    std::vector<SQL_TIMESTAMP_STRUCT> timestamps;
    std::vector<int64_t>       i64;      // stable storage for Int64 values
};

// Convert unix seconds (UTC) to a SQL_TIMESTAMP_STRUCT (DATETIME2). Pure date math via
// the CRT; the server stores SYSUTCDATETIME() defaults, this is for explicit columns
// (Sessions.ExpiresAt, Accounts.LockedUntil).
SQL_TIMESTAMP_STRUCT ToTimestamp(int64_t unixSec)
{
    SQL_TIMESTAMP_STRUCT ts{};
    const __time64_t t = static_cast<__time64_t>(unixSec);
    tm g{};
    if (_gmtime64_s(&g, &t) == 0) {
        ts.year     = static_cast<SQLSMALLINT>(g.tm_year + 1900);
        ts.month    = static_cast<SQLUSMALLINT>(g.tm_mon + 1);
        ts.day      = static_cast<SQLUSMALLINT>(g.tm_mday);
        ts.hour     = static_cast<SQLUSMALLINT>(g.tm_hour);
        ts.minute   = static_cast<SQLUSMALLINT>(g.tm_min);
        ts.second   = static_cast<SQLUSMALLINT>(g.tm_sec);
        ts.fraction = 0;
    }
    return ts;
}

// Bind one SqlParam at 1-based 'index' onto 'stmt'. Storage for converted values is
// appended to 'store' (which must outlive the execute).
bool BindOne(SQLHSTMT stmt, SQLUSMALLINT index, const SqlParam& p, ParamStorage& store)
{
    switch (p.kind) {
    case SqlParam::Kind::Null: {
        store.lenInd.push_back(SQL_NULL_DATA);
        SQLLEN* ind = &store.lenInd.back();
        return SqlOk(SQLBindParameter(stmt, index, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                                      SQL_BIGINT, 0, 0, nullptr, 0, ind));
    }
    case SqlParam::Kind::Int64: {
        store.i64.push_back(p.i64);
        int64_t* val = &store.i64.back();
        store.lenInd.push_back(0);
        return SqlOk(SQLBindParameter(stmt, index, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                                      SQL_BIGINT, 0, 0, val, 0, nullptr));
    }
    case SqlParam::Kind::DateTimeUtcSeconds: {
        store.timestamps.push_back(ToTimestamp(p.i64));
        SQL_TIMESTAMP_STRUCT* ts = &store.timestamps.back();
        store.lenInd.push_back(sizeof(SQL_TIMESTAMP_STRUCT));
        return SqlOk(SQLBindParameter(stmt, index, SQL_PARAM_INPUT, SQL_C_TYPE_TIMESTAMP,
                                      SQL_TYPE_TIMESTAMP, 27, 7, ts,
                                      sizeof(SQL_TIMESTAMP_STRUCT), &store.lenInd.back()));
    }
    case SqlParam::Kind::Bytes: {
        store.lenInd.push_back(static_cast<SQLLEN>(p.bytes.size()));
        SQLLEN* ind = &store.lenInd.back();
        // VARBINARY/BINARY — the data span is owned by the caller for the call's life.
        return SqlOk(SQLBindParameter(stmt, index, SQL_PARAM_INPUT, SQL_C_BINARY,
                                      SQL_VARBINARY,
                                      p.bytes.empty() ? 1 : p.bytes.size(), 0,
                                      const_cast<uint8_t*>(p.bytes.data()),
                                      static_cast<SQLLEN>(p.bytes.size()), ind));
    }
    case SqlParam::Kind::Utf8: {
        store.wide.push_back(Widen(p.text));
        std::wstring& w = store.wide.back();
        store.lenInd.push_back(static_cast<SQLLEN>(w.size() * sizeof(wchar_t)));
        return SqlOk(SQLBindParameter(stmt, index, SQL_PARAM_INPUT, SQL_C_WCHAR,
                                      SQL_WVARCHAR, w.empty() ? 1 : w.size(), 0,
                                      reinterpret_cast<SQLWCHAR*>(w.data()),
                                      static_cast<SQLLEN>(w.size() * sizeof(wchar_t)),
                                      &store.lenInd.back()));
    }
    }
    return false;
}

bool BindAll(SQLHSTMT stmt, std::span<const SqlParam> params, ParamStorage& store)
{
    // Reserve so the vectors never reallocate mid-bind (the bound pointers must stay
    // stable until SQLExecDirect reads them).
    store.wide.reserve(params.size());
    store.lenInd.reserve(params.size());
    store.timestamps.reserve(params.size());
    store.i64.reserve(params.size());
    for (size_t i = 0; i < params.size(); ++i) {
        if (!BindOne(stmt, static_cast<SQLUSMALLINT>(i + 1), params[i], store))
            return false;
    }
    return true;
}

} // namespace

// -----------------------------------------------------------------------------
// Statement execution
// -----------------------------------------------------------------------------

std::optional<int64_t>
OdbcConnection::ExecNonQuery(std::string_view sql, std::span<const SqlParam> params)
{
    SQLHSTMT stmt = AsStmt(AllocStmt());
    if (!stmt)
        return std::nullopt;

    ParamStorage store;
    if (!params.empty() && !BindAll(stmt, params, store)) {
        CaptureDiag(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return std::nullopt;
    }

    const std::wstring wsql = Widen(sql);
    const SQLRETURN r = SQLExecDirectW(stmt,
                                       reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(wsql.c_str())),
                                       SQL_NTS);
    if (!SqlOk(r) && r != SQL_NO_DATA) {
        CaptureDiag(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return std::nullopt;
    }

    SQLLEN rows = 0;
    SQLRowCount(stmt, &rows);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    m_lastError = {};
    return static_cast<int64_t>(rows);
}

std::optional<SqlResult>
OdbcConnection::ExecQuery(std::string_view sql, std::span<const SqlParam> params, size_t columnCount)
{
    SQLHSTMT stmt = AsStmt(AllocStmt());
    if (!stmt)
        return std::nullopt;

    ParamStorage store;
    if (!params.empty() && !BindAll(stmt, params, store)) {
        CaptureDiag(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return std::nullopt;
    }

    const std::wstring wsql = Widen(sql);
    const SQLRETURN r = SQLExecDirectW(stmt,
                                       reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(wsql.c_str())),
                                       SQL_NTS);
    if (!SqlOk(r) && r != SQL_NO_DATA) {
        CaptureDiag(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return std::nullopt;
    }

    // Discover the actual column count; clamp the requested count to it.
    SQLSMALLINT nCols = 0;
    SQLNumResultCols(stmt, &nCols);
    const size_t cols = (nCols > 0) ? (std::min<size_t>(columnCount, static_cast<size_t>(nCols)))
                                    : columnCount;

    SqlResult result;
    // Read each row with SQLGetData per column so VARBINARY(MAX) blobs of any size load
    // (we grow a buffer until SQL_SUCCESS rather than SQL_SUCCESS_WITH_INFO truncation).
    while (true) {
        const SQLRETURN fr = SQLFetch(stmt);
        if (fr == SQL_NO_DATA)
            break;
        if (!SqlOk(fr)) {
            CaptureDiag(SQL_HANDLE_STMT, stmt);
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            return std::nullopt;
        }

        SqlRow row;
        row.reserve(cols);
        for (size_t c = 0; c < cols; ++c) {
            SqlValue v;
            // Read each column as binary and ACCUMULATE chunks until SQLGetData reports
            // SQL_SUCCESS (the final chunk) rather than SQL_SUCCESS_WITH_INFO (more to
            // come). This preserves VARBINARY(MAX) blobs of any size (SimSnapshots.Blob)
            // exactly, and lets callers reinterpret integers/strings (each store knows
            // its column types). Per ODBC: on SQL_SUCCESS_WITH_INFO the driver fills the
            // whole 'chunk' buffer; on SQL_SUCCESS 'ind' is the remaining byte count.
            std::vector<uint8_t> acc;
            std::vector<uint8_t> chunk(4096);
            bool done = false;
            while (!done) {
                SQLLEN ind = 0;
                const SQLRETURN gr = SQLGetData(stmt, static_cast<SQLUSMALLINT>(c + 1),
                                                SQL_C_BINARY, chunk.data(),
                                                static_cast<SQLLEN>(chunk.size()), &ind);
                if (gr == SQL_NO_DATA) {
                    done = true;
                } else if (gr == SQL_SUCCESS_WITH_INFO) {
                    // Truncated: the buffer is full; more data remains — append it all.
                    acc.insert(acc.end(), chunk.begin(), chunk.end());
                } else if (gr == SQL_SUCCESS) {
                    if (ind == SQL_NULL_DATA) {
                        v.isNull = true;
                    } else if (ind > 0) {
                        const size_t n = static_cast<size_t>(ind);
                        acc.insert(acc.end(), chunk.begin(),
                                   chunk.begin() + static_cast<ptrdiff_t>(std::min(n, chunk.size())));
                    }
                    done = true;
                } else {
                    CaptureDiag(SQL_HANDLE_STMT, stmt);
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    return std::nullopt;
                }
            }
            if (!v.isNull)
                v.bytes = std::move(acc);
            row.push_back(std::move(v));
        }
        result.rows.push_back(std::move(row));
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    m_lastError = {};
    return result;
}

std::optional<int64_t>
OdbcConnection::ExecInsertReturningIdentity(std::string_view sql, std::span<const SqlParam> params)
{
    // Append SCOPE_IDENTITY() so the new row's IDENTITY comes back in one round-trip.
    // (SCOPE_IDENTITY is connection+scope local — safe under the persistence thread's
    // serialized use; Azure-SQL-compatible.)
    std::string combined(sql);
    combined += "; SELECT CAST(SCOPE_IDENTITY() AS BIGINT);";

    auto res = ExecQuery(combined, params, 1);
    if (!res || res->Empty() || res->rows.front().empty())
        return std::nullopt;
    const SqlValue& cell = res->rows.front().front();
    if (cell.isNull || cell.bytes.size() < sizeof(int64_t))
        return std::nullopt;
    int64_t id = 0;
    std::memcpy(&id, cell.bytes.data(), sizeof(int64_t));
    return id;
}

// -----------------------------------------------------------------------------
// Transactions
// -----------------------------------------------------------------------------

bool OdbcConnection::BeginTransaction()
{
    if (!m_connected || !m_dbc)
        return false;
    if (m_inTxn)
        return true;
    if (!SqlOk(SQLSetConnectAttr(AsDbc(m_dbc), SQL_ATTR_AUTOCOMMIT,
                                 reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0))) {
        CaptureDiag(SQL_HANDLE_DBC, m_dbc);
        return false;
    }
    m_inTxn = true;
    return true;
}

bool OdbcConnection::Commit()
{
    if (!m_inTxn || !m_dbc)
        return false;
    const SQLRETURN r = SQLEndTran(SQL_HANDLE_DBC, AsDbc(m_dbc), SQL_COMMIT);
    const bool ok = SqlOk(r);
    if (!ok)
        CaptureDiag(SQL_HANDLE_DBC, m_dbc);
    // Restore autocommit regardless, so the connection returns to steady state.
    SQLSetConnectAttr(AsDbc(m_dbc), SQL_ATTR_AUTOCOMMIT,
                      reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0);
    m_inTxn = false;
    return ok;
}

bool OdbcConnection::Rollback()
{
    if (!m_inTxn || !m_dbc)
        return false;
    const SQLRETURN r = SQLEndTran(SQL_HANDLE_DBC, AsDbc(m_dbc), SQL_ROLLBACK);
    const bool ok = SqlOk(r);
    if (!ok)
        CaptureDiag(SQL_HANDLE_DBC, m_dbc);
    SQLSetConnectAttr(AsDbc(m_dbc), SQL_ATTR_AUTOCOMMIT,
                      reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0);
    m_inTxn = false;
    return ok;
}

} // namespace Neuron::Persist
