#pragma once
// Tick-hot-path allocators — §7.2 of the masterplan.
//
// The 30 Hz sim tick must not allocate from the global heap. Provide:
//   FrameArena  — monotonic buffer, reset each tick; O(1) alloc, zero free.
//   TickPool    — std::pmr::unsynchronized_pool_resource for pool-managed objects.
//
// Both implement std::pmr::memory_resource for STL container compatibility.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <new>

namespace Neuron::Alloc
{

// ---------------------------------------------------------------------------
// FrameArena — monotonic_buffer_resource over a fixed inline buffer.
// Reset each sim tick. All allocations within a tick come from here.
// ---------------------------------------------------------------------------
template <size_t Capacity = 512 * 1024> // default 512 KB
class FrameArena : public std::pmr::memory_resource
{
public:
    FrameArena() noexcept : m_cursor(0) {}

    // Reset at the start of each tick — O(1), no destructors called.
    void Reset() noexcept { m_cursor = 0; }

    [[nodiscard]] size_t Used()      const noexcept { return m_cursor; }
    [[nodiscard]] size_t Remaining() const noexcept { return Capacity - m_cursor; }

    // pmr interface
    void* do_allocate(size_t bytes, size_t alignment) override
    {
        // Align cursor
        const size_t pad  = (-m_cursor) & (alignment - 1);
        const size_t next = m_cursor + pad + bytes;
        assert(next <= Capacity && "FrameArena capacity exceeded");
        void* ptr  = m_buf + m_cursor + pad;
        m_cursor   = next;
        return ptr;
    }

    void do_deallocate(void*, size_t, size_t) override
    {
        // Monotonic: individual frees are no-ops; Reset() reclaims all.
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
    {
        return this == &other;
    }

private:
    alignas(std::max_align_t) uint8_t m_buf[Capacity];
    size_t m_cursor;
};

// ---------------------------------------------------------------------------
// TickPool — unsynchronized_pool_resource backed by an upstream arena.
// For small, frequently-allocated objects during a tick.
// ---------------------------------------------------------------------------
class TickPool
{
public:
    explicit TickPool(std::pmr::memory_resource* upstream = std::pmr::get_default_resource())
        : m_pool(std::pmr::pool_options{ .max_blocks_per_chunk = 256,
                                         .largest_required_pool_block = 4096 },
                 upstream) {}

    [[nodiscard]] std::pmr::memory_resource* Resource() noexcept { return &m_pool; }

    // Release all memory back to the upstream resource.
    void Release() { m_pool.release(); }

private:
    std::pmr::unsynchronized_pool_resource m_pool;
};

// ---------------------------------------------------------------------------
// Helper: pmr-aware vector/string type aliases for tick use
// ---------------------------------------------------------------------------
template <typename T>
using PmrVector = std::pmr::vector<T>;

} // namespace Neuron::Alloc
