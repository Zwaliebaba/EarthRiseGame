#pragma once
// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// OdbcConnectionPool.h — small ODBC connection pool (M5 area A, §15).
//
// §15 calls for **connection pooling** behind the persistence thread. The pool keeps a
// bounded set of live OdbcConnections; a caller leases one (reconnecting it if it was
// dropped), uses it, and returns it. Because the persistence thread is the only thread
// that touches SQL (the §9 hot-path-isolation rule), contention is low — the pool's job
// is mostly to amortise the SQLDriverConnect cost across many small statements and to
// transparently re-establish a connection that the server dropped (an Azure SQL
// transient fault, area J).
//
// A lease is an RAII handle (Lease) that returns the connection on destruction, so
// "use a connection" is always exception/early-return safe.

#include "OdbcConnection.h"
#include "PersistConfig.h"

#include <memory>
#include <mutex>
#include <vector>

namespace Neuron::Persist
{

class OdbcConnectionPool
{
public:
    // RAII lease: holds a connection borrowed from the pool and returns it on scope
    // exit. Access the connection via operator-> / Get(). Move-only.
    class Lease
    {
    public:
        Lease() = default;
        Lease(OdbcConnectionPool* pool, std::unique_ptr<OdbcConnection> conn)
            : m_pool(pool), m_conn(std::move(conn)) {}
        ~Lease() { Release(); }

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& o) noexcept : m_pool(o.m_pool), m_conn(std::move(o.m_conn)) { o.m_pool = nullptr; }
        Lease& operator=(Lease&& o) noexcept
        {
            if (this != &o) { Release(); m_pool = o.m_pool; m_conn = std::move(o.m_conn); o.m_pool = nullptr; }
            return *this;
        }

        [[nodiscard]] bool            Valid() const noexcept { return m_conn && m_conn->IsConnected(); }
        [[nodiscard]] OdbcConnection* Get() const noexcept { return m_conn.get(); }
        OdbcConnection* operator->() const noexcept { return m_conn.get(); }
        explicit operator bool() const noexcept { return Valid(); }

    private:
        void Release()
        {
            if (m_pool && m_conn)
                m_pool->Return(std::move(m_conn));
            m_pool = nullptr;
        }
        OdbcConnectionPool* m_pool{ nullptr };
        std::unique_ptr<OdbcConnection> m_conn;
    };

    OdbcConnectionPool() = default;
    ~OdbcConnectionPool() = default;

    OdbcConnectionPool(const OdbcConnectionPool&) = delete;
    OdbcConnectionPool& operator=(const OdbcConnectionPool&) = delete;

    // Configure + warm the pool to poolMin connections. Returns false (and leaves the
    // pool empty) if the first connection cannot be established — the caller then runs
    // in the degraded "no DB configured" mode (the §16.3 graceful-skip path). 'cfg'
    // is copied; later leases reconnect against it.
    [[nodiscard]] bool Initialize(const PersistConfig& cfg);

    // Lease a connection. Reuses an idle one (reconnecting it if the server dropped it)
    // or opens a new one up to poolMax. An invalid Lease (==false) means no connection
    // could be obtained (DB unreachable) — the caller must handle that without crashing.
    [[nodiscard]] Lease Acquire();

    // Drop all pooled connections (shutdown).
    void Shutdown();

    [[nodiscard]] size_t Size() const;
    [[nodiscard]] const PersistConfig& Config() const noexcept { return m_cfg; }

private:
    friend class Lease;
    void Return(std::unique_ptr<OdbcConnection> conn);

    std::unique_ptr<OdbcConnection> OpenOne();

    mutable std::mutex m_mutex;
    PersistConfig      m_cfg;
    std::vector<std::unique_ptr<OdbcConnection>> m_idle;
    int                m_live{ 0 };   // total open connections (idle + leased)
    bool               m_initialized{ false };
};

} // namespace Neuron::Persist
