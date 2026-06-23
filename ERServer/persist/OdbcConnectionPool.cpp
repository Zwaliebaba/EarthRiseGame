// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// OdbcConnectionPool.cpp — connection-pool implementation (M5 area A, §15).

#include "pch.h"
#include "OdbcConnectionPool.h"

namespace Neuron::Persist
{

std::unique_ptr<OdbcConnection> OdbcConnectionPool::OpenOne()
{
    auto conn = std::make_unique<OdbcConnection>();
    if (!conn->Connect(m_cfg))
        return nullptr;
    return conn;
}

bool OdbcConnectionPool::Initialize(const PersistConfig& cfg)
{
    std::lock_guard lock(m_mutex);
    m_cfg = cfg;
    m_idle.clear();
    m_live = 0;
    m_initialized = true;

    if (!cfg.HasConnString())
        return false; // no DB configured → degraded mode, not an error to crash on

    // Warm to poolMin so the first economy write does not pay the connect cost.
    const int warm = m_cfg.poolMin < 1 ? 1 : m_cfg.poolMin;
    for (int i = 0; i < warm; ++i) {
        auto conn = OpenOne();
        if (!conn)
            return m_live > 0; // at least one connected → usable; else report failure
        ++m_live;
        m_idle.push_back(std::move(conn));
    }
    return true;
}

OdbcConnectionPool::Lease OdbcConnectionPool::Acquire()
{
    std::unique_ptr<OdbcConnection> conn;
    {
        std::lock_guard lock(m_mutex);
        if (!m_idle.empty()) {
            conn = std::move(m_idle.back());
            m_idle.pop_back();
        } else if (m_live < m_cfg.poolMax) {
            conn = OpenOne();
            if (conn)
                ++m_live;
        }
    }

    // Reconnect a connection the server dropped (Azure SQL transient fault, area J).
    if (conn && !conn->IsConnected()) {
        if (!conn->Connect(m_cfg)) {
            // Could not revive — discard and account for it.
            std::lock_guard lock(m_mutex);
            --m_live;
            return Lease{};
        }
    }
    if (!conn)
        return Lease{}; // pool exhausted / DB unreachable — caller handles gracefully

    return Lease{ this, std::move(conn) };
}

void OdbcConnectionPool::Return(std::unique_ptr<OdbcConnection> conn)
{
    if (!conn)
        return;
    std::lock_guard lock(m_mutex);
    // A connection that died (or is mid-transaction after an error) is dropped rather
    // than pooled, so a poisoned handle never gets reused; the count is decremented.
    if (!conn->IsConnected() || conn->InTransaction()) {
        --m_live;
        return;
    }
    m_idle.push_back(std::move(conn));
}

void OdbcConnectionPool::Shutdown()
{
    std::lock_guard lock(m_mutex);
    m_idle.clear();
    m_live = 0;
    m_initialized = false;
}

size_t OdbcConnectionPool::Size() const
{
    std::lock_guard lock(m_mutex);
    return m_idle.size();
}

} // namespace Neuron::Persist
