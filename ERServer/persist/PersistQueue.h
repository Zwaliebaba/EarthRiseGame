#pragma once
// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// PersistQueue.h — MPSC hand-off from the sim to the persistence thread (M5 area A, §9).
//
// §9: "DB is out of the tick hot path." The sim (the 30 Hz tick thread) MUST NOT block
// on SQL. It hands work to the persistence thread through these queues; the producer
// side only ever does an O(1) push under a short mutex (never any I/O, never any wait
// on the DB) so a slow/stalled SQL Server can never back-pressure or stall the tick.
//
// A single-producer (the tick thread) / single-consumer (the persistence thread) queue
// would suffice, but a small mutex-guarded MPSC vector is used: it is trivially correct
// (this code can't be run here, so correctness-by-inspection matters more than shaving
// a CAS), the contention is negligible (one short critical section per tick on each
// side), and it tolerates extra producers if the design later sheds work across threads.
// The consumer drains by swapping the whole buffer out under the lock, so the producer
// is blocked only for the duration of a pointer swap, not for the DB round-trips.
//
// Bounded: if the consumer falls behind (DB outage), the queue caps and DROPS oldest
// WRITE-BEHIND items (bounded RPO is explicitly allowed to lose a few seconds of
// movement, §15) — but the ECONOMY queue is UNBOUNDED-by-policy (it must be zero-loss;
// if it ever cannot drain, that is a hard fault the server escalates, never a silent
// drop). The two queue kinds make that asymmetry explicit in the type system.

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace Neuron::Persist
{

// Zero-loss queue: never drops. Used for economy mutations (area D). If it grows without
// bound the DB is down and the caller must escalate (Depth() is exposed for that).
template <class T>
class MpscZeroLossQueue
{
public:
    // Producer side (tick thread): O(1), no I/O, no wait. Returns the new depth.
    size_t Push(T&& item)
    {
        std::lock_guard lock(m_mutex);
        m_buf.push_back(std::move(item));
        return m_buf.size();
    }
    size_t Push(const T& item)
    {
        std::lock_guard lock(m_mutex);
        m_buf.push_back(item);
        return m_buf.size();
    }

    // Consumer side (persistence thread): swap the whole buffer out under the lock so
    // the producer is blocked only for a pointer swap, then process 'out' lock-free.
    void DrainInto(std::vector<T>& out)
    {
        std::lock_guard lock(m_mutex);
        out.clear();
        out.swap(m_buf);
    }

    [[nodiscard]] size_t Depth() const
    {
        std::lock_guard lock(m_mutex);
        return m_buf.size();
    }

private:
    mutable std::mutex m_mutex;
    std::vector<T>     m_buf;
};

// Bounded write-behind queue: caps at 'capacity' and DROPS the OLDEST items past the
// cap (bounded RPO — losing a few seconds of position on a hard backlog is allowed,
// §15; economy NEVER uses this). Coalescing by key (latest-wins per entity) is the
// caller's job (WriteBehindBatch keys by netId), so a drop just loses an interim point.
template <class T>
class MpscBoundedQueue
{
public:
    explicit MpscBoundedQueue(size_t capacity) : m_capacity(capacity == 0 ? 1 : capacity) {}

    // Producer side: O(1). Returns true if accepted, false if it had to drop-oldest to
    // make room (the §21 write-behind "lag/drop" signal, area H).
    bool Push(T&& item)
    {
        std::lock_guard lock(m_mutex);
        bool dropped = false;
        if (m_buf.size() >= m_capacity) {
            // Drop the oldest (front). A vector erase-front is O(n) but the cap is small
            // and the alternative (deque) churns more; the bounded path is cold anyway.
            m_buf.erase(m_buf.begin());
            ++m_dropped;
            dropped = true;
        }
        m_buf.push_back(std::move(item));
        return !dropped;
    }

    void DrainInto(std::vector<T>& out)
    {
        std::lock_guard lock(m_mutex);
        out.clear();
        out.swap(m_buf);
    }

    [[nodiscard]] size_t   Depth()   const { std::lock_guard l(m_mutex); return m_buf.size(); }
    [[nodiscard]] uint64_t Dropped() const { std::lock_guard l(m_mutex); return m_dropped; }

private:
    mutable std::mutex m_mutex;
    std::vector<T>     m_buf;
    size_t             m_capacity;
    uint64_t           m_dropped{ 0 };
};

} // namespace Neuron::Persist
