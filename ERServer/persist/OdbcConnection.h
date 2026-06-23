#pragma once
// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// OdbcConnection.h — thin custom ODBC Driver 18 wrapper (M5 area A, §15/§20).
//
// A small, dependency-free wrapper over the ODBC C API (sql.h/sqlext.h, odbc32.lib —
// already linked by ERServer.vcxproj). It exists to keep the rest of the persist layer
// (AccountStore, EconomyStore, SimSnapshotStore, PersistenceThread) free of raw
// SQLHANDLEs and to centralise:
//   * connecting with `Driver={ODBC Driver 18 for SQL Server};...;Encrypt=yes`
//     (SQLDriverConnect), where the connection string + credentials come from a
//     secret/env (PersistConfig, §20) — never hard-coded;
//   * BOTH a SQL-login connection string AND a managed-identity / Entra ID one
//     (`Authentication=ActiveDirectoryMsi`) selected by config — area J (Azure SQL
//     migration) needs the managed-identity path to exist now (§15 App→DB auth);
//   * parameterized statements / stored-proc execution (SQLBindParameter), result
//     binding (SQLBindCol / SQLGetData), and error mapping (SQLGetDiagRec).
//
// Azure-SQL compatibility: this wrapper issues only plain T-SQL the caller hands it —
// no cross-DB, no SQL Agent, no FILESTREAM (§15) — so the M6 migration is a
// connection-string + auth change, not a rewrite.
//
// The header keeps <windows.h>/<sql.h> out of the public surface (opaque void*
// handles, reinterpreted in the .cpp), mirroring the CngCrypto/IocpUdpListener style.

#include "PersistConfig.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Neuron::Persist
{

// A mapped ODBC error (one SQLGetDiagRec record). 'native' is the SQL Server error
// number; 'sqlState' is the 5-char ODBC SQLSTATE (e.g. "23000" unique-violation —
// which EconomyStore keys idempotency off, mirroring Outbox.h's idemKey dedupe).
struct OdbcError
{
    std::string sqlState;     // 5-char SQLSTATE
    int32_t     native{ 0 };  // driver/SQL Server native error code
    std::string message;      // human-readable diagnostic

    [[nodiscard]] bool IsUniqueViolation() const noexcept
    {
        // 23000 (integrity constraint) / 2627 / 2601 are SQL Server's dup-key codes.
        return sqlState == "23000" || native == 2627 || native == 2601;
    }
};

// A bound parameter for a parameterized statement. Kept as a tagged value so callers
// pass plain C++ types and the .cpp binds the right ODBC C/SQL types. Strings/blobs
// are owned by the caller for the lifetime of the Execute call (spans, not copies).
struct SqlParam
{
    enum class Kind : uint8_t { Null, Int64, Bytes, Utf8, DateTimeUtcSeconds } kind{ Kind::Null };
    int64_t                  i64{ 0 };          // Int64 / DateTimeUtcSeconds (unix seconds)
    std::span<const uint8_t> bytes{};           // Bytes (VARBINARY/BINARY)
    std::string_view         text{};            // Utf8 (NVARCHAR — converted to UTF-16 in .cpp)

    static SqlParam MakeNull()                       { return SqlParam{ Kind::Null }; }
    static SqlParam Make(int64_t v)                  { SqlParam p; p.kind = Kind::Int64; p.i64 = v; return p; }
    static SqlParam Make(std::span<const uint8_t> b) { SqlParam p; p.kind = Kind::Bytes; p.bytes = b; return p; }
    static SqlParam Make(std::string_view s)         { SqlParam p; p.kind = Kind::Utf8; p.text = s; return p; }
    static SqlParam MakeDateTimeUtc(int64_t unixSec) { SqlParam p; p.kind = Kind::DateTimeUtcSeconds; p.i64 = unixSec; return p; }
};

// One result cell, read back from a SELECT. The store maps these into its own structs.
struct SqlValue
{
    bool                 isNull{ false };
    int64_t              i64{ 0 };
    std::vector<uint8_t> bytes;
    std::string          text; // UTF-8 (from NVARCHAR via UTF-16 → UTF-8)
};

using SqlRow = std::vector<SqlValue>;

// Result set: a flat table of rows. Sized for the small result sets the M5 loop reads
// (a single account, a session, a wallet, a snapshot blob); bulk paths use TVP/bcp
// (PersistenceThread) rather than this.
struct SqlResult
{
    std::vector<SqlRow> rows;
    [[nodiscard]] bool Empty() const noexcept { return rows.empty(); }
};

// RAII ODBC connection. One per pooled slot (the PersistenceThread owns the pool); a
// connection is single-threaded (the §9 hot-path-isolation rule keeps SQL off the
// tick thread, so connections live on the persistence thread).
class OdbcConnection
{
public:
    OdbcConnection() = default;
    ~OdbcConnection();

    OdbcConnection(const OdbcConnection&) = delete;
    OdbcConnection& operator=(const OdbcConnection&) = delete;
    OdbcConnection(OdbcConnection&&) noexcept;
    OdbcConnection& operator=(OdbcConnection&&) noexcept;

    // Allocate the environment+connection handles and SQLDriverConnect using 'cfg'
    // (builds the credential/auth fragment for cfg.authMode and appends it to
    // cfg.connString — Encrypt=yes is expected to be present in connString). Returns
    // false on failure; LastError() then holds the mapped diagnostic.
    [[nodiscard]] bool Connect(const PersistConfig& cfg);

    // True once connected and not disconnected by an error. The pool calls this before
    // handing the connection out and reconnects a dropped one.
    [[nodiscard]] bool IsConnected() const noexcept { return m_connected; }

    void Disconnect();

    // -- statements ------------------------------------------------------------
    // Execute a parameterized non-query (INSERT/UPDATE/DELETE/EXEC). Returns the
    // affected-row count on success, or std::nullopt on error (see LastError()).
    [[nodiscard]] std::optional<int64_t>
    ExecNonQuery(std::string_view sql, std::span<const SqlParam> params = {});

    // Execute a parameterized query and bind the full (small) result set. The caller
    // supplies the column count it expects (the wrapper reads that many columns per
    // row via SQLGetData, so blobs of any size are supported). nullopt on error.
    [[nodiscard]] std::optional<SqlResult>
    ExecQuery(std::string_view sql, std::span<const SqlParam> params, size_t columnCount);

    // Convenience: a single-row scalar INSERT ... ; SELECT SCOPE_IDENTITY(); — returns
    // the new IDENTITY value (e.g. AccountId, OutboxId). nullopt on error.
    [[nodiscard]] std::optional<int64_t>
    ExecInsertReturningIdentity(std::string_view sql, std::span<const SqlParam> params);

    // -- transactions (the write-through outbox needs all-or-nothing, area D) ----
    // BeginTransaction turns off autocommit; Commit/Rollback restore it. A failed
    // statement inside a transaction leaves it open for the caller to Rollback.
    [[nodiscard]] bool BeginTransaction();
    [[nodiscard]] bool Commit();
    [[nodiscard]] bool Rollback();
    [[nodiscard]] bool InTransaction() const noexcept { return m_inTxn; }

    // The most recent mapped diagnostic (SQLGetDiagRec). Empty if the last call was OK.
    [[nodiscard]] const OdbcError& LastError() const noexcept { return m_lastError; }

    // Build the auth/credential fragment appended to the connection string for 'cfg'
    // (exposed for unit-testing the SQL-login vs managed-identity selection without a
    // live DB). For ManagedIdentity this is "Authentication=ActiveDirectoryMsi;".
    [[nodiscard]] static std::string BuildAuthFragment(const PersistConfig& cfg);

private:
    void   FreeHandles() noexcept;
    void   CaptureDiag(int handleType, void* handle); // SQLGetDiagRec → m_lastError
    void*  AllocStmt();                               // SQLAllocHandle(SQL_HANDLE_STMT)

    void* m_env{ nullptr };   // SQLHENV
    void* m_dbc{ nullptr };   // SQLHDBC
    bool  m_connected{ false };
    bool  m_inTxn{ false };
    OdbcError m_lastError;
};

} // namespace Neuron::Persist
