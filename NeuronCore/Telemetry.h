#pragma once
// Telemetry.h — §21 observability counters (M4 area I).
//
// "You cannot hold the M4 bandwidth budget you cannot measure." The load gate (area
// J) reads these: per-tick sim time p50/p99, per-client encode time p99, per-client
// down/upstream bytes, loss/retransmit/reorder, cold-start convergence, AEAD-auth
// failures, replay rejects, and the per-client baseline-RAM gauge (App. B). Wired
// *before* the run, not during (§17). This is the pure aggregation core (mirrored on
// the Linux testrunner, §16.2); ERServer/ERHeadless export it as structured logs +
// lightweight counters (MS-only: perf counters / ETW), consumed by the harness.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Neuron::Sim
{

// Bounded sample window with nearest-rank percentiles (sim tick p50/p99, encode p99).
// Keeps the most recent 'cap' samples in a ring; Percentile sorts a copy (cap is
// small — per-second aggregation), so old samples age out and the gate reads a
// rolling distribution. Pure.
class PercentileWindow
{
public:
    explicit PercentileWindow(size_t cap = 256) : m_cap(cap == 0 ? 1 : cap) {}

    void Add(double v)
    {
        if (m_ring.size() < m_cap) m_ring.push_back(v);
        else { m_ring[m_head] = v; m_head = (m_head + 1) % m_cap; }
    }

    void Clear() { m_ring.clear(); m_head = 0; }
    [[nodiscard]] size_t Count() const noexcept { return m_ring.size(); }
    [[nodiscard]] bool   Empty() const noexcept { return m_ring.empty(); }

    // Nearest-rank percentile, q ∈ [0, 1]. p50 = Percentile(0.50), p99 = (0.99).
    [[nodiscard]] double Percentile(double q) const
    {
        if (m_ring.empty()) return 0.0;
        std::vector<double> s = m_ring;
        std::sort(s.begin(), s.end());
        const double clamped = q < 0.0 ? 0.0 : (q > 1.0 ? 1.0 : q);
        size_t rank = static_cast<size_t>(std::ceil(clamped * static_cast<double>(s.size())));
        if (rank == 0) rank = 1;
        if (rank > s.size()) rank = s.size();
        return s[rank - 1];
    }

    [[nodiscard]] double Max() const
    {
        return m_ring.empty() ? 0.0 : *std::max_element(m_ring.begin(), m_ring.end());
    }
    [[nodiscard]] double Mean() const
    {
        if (m_ring.empty()) return 0.0;
        double sum = 0.0;
        for (double v : m_ring) sum += v;
        return sum / static_cast<double>(m_ring.size());
    }

private:
    std::vector<double> m_ring;
    size_t              m_cap;
    size_t              m_head{ 0 };
};

// Network counters (§21). Monotonic; the harness diffs them across the run window.
struct NetCounters
{
    uint64_t downstreamBytes{ 0 }; // server → clients
    uint64_t upstreamBytes{ 0 };   // clients → server
    uint64_t datagramsOut{ 0 };
    uint64_t datagramsIn{ 0 };
    uint64_t loss{ 0 };            // detected gaps in the reliable sequence
    uint64_t retransmit{ 0 };
    uint64_t reorder{ 0 };
    uint64_t authFailures{ 0 };    // AEAD verify failures (§8.1)
    uint64_t replayRejects{ 0 };   // replay-window rejections

    void AddDown(uint64_t bytes) noexcept { downstreamBytes += bytes; ++datagramsOut; }
    void AddUp(uint64_t bytes)   noexcept { upstreamBytes += bytes;   ++datagramsIn; }
};

// The §21 server telemetry, sized to the App. B gates. The harness samples per tick
// (sim/encode ms, entity count), per snapshot (per-client bytes), on ack (cold-start
// convergence), and on the dilation state; the load gate reads the percentiles +
// gauges. All aggregation is pure and unit-tested; only the *sampling sites* live in
// the Win32 server/harness.
class ServerTelemetry
{
public:
    // --- per-tick sim (§21) --------------------------------------------------
    void RecordTickMs(double ms)   { m_simMs.Add(ms); }
    void RecordEncodeMs(double ms) { m_encodeMs.Add(ms); }
    void RecordEntityCount(uint64_t n) { m_lastEntities = n; }
    void RecordDilation(double factor) { m_dilation = factor; }

    [[nodiscard]] double SimP50() const { return m_simMs.Percentile(0.50); }
    [[nodiscard]] double SimP99() const { return m_simMs.Percentile(0.99); }
    [[nodiscard]] double EncodeP99() const { return m_encodeMs.Percentile(0.99); }
    [[nodiscard]] uint64_t EntityCount() const noexcept { return m_lastEntities; }
    [[nodiscard]] double Dilation() const noexcept { return m_dilation; }

    // --- per-client downstream (App. B bandwidth gate) -----------------------
    void RecordClientDownstream(uint64_t bytesThisTick) { m_clientDownPerTick.Add(static_cast<double>(bytesThisTick)); }
    [[nodiscard]] double ClientDownstreamP99() const { return m_clientDownPerTick.Percentile(0.99); }
    [[nodiscard]] double ClientDownstreamMax() const { return m_clientDownPerTick.Max(); }

    // --- per-client baseline RAM (App. B gauge) ------------------------------
    void RecordBaselineBytes(uint64_t totalBytes) noexcept { m_baselineBytes = totalBytes; }
    [[nodiscard]] uint64_t BaselineBytes() const noexcept { return m_baselineBytes; }

    // --- cold-start convergence (§21) ----------------------------------------
    // Ticks from a client's empty (∅) baseline to its interest set fully converged.
    void RecordColdStartTicks(uint32_t ticks) { m_coldStart.Add(static_cast<double>(ticks)); }
    [[nodiscard]] double ColdStartP99Ticks() const { return m_coldStart.Percentile(0.99); }
    [[nodiscard]] double ColdStartMaxTicks() const { return m_coldStart.Max(); }

    // --- visible-cap binding (R16 evidence) ----------------------------------
    void RecordCapBind(size_t cappedThisTick) noexcept
    {
        if (cappedThisTick > m_maxCapBind) m_maxCapBind = cappedThisTick;
        if (cappedThisTick > 0) ++m_capBindTicks;
    }
    [[nodiscard]] size_t MaxCapBind() const noexcept { return m_maxCapBind; }
    [[nodiscard]] uint64_t CapBindTicks() const noexcept { return m_capBindTicks; }

    [[nodiscard]] NetCounters& Net() noexcept { return m_net; }
    [[nodiscard]] const NetCounters& Net() const noexcept { return m_net; }

private:
    PercentileWindow m_simMs{ 300 };           // ~10 s of 30 Hz ticks
    PercentileWindow m_encodeMs{ 300 };
    PercentileWindow m_clientDownPerTick{ 600 };
    PercentileWindow m_coldStart{ 256 };
    NetCounters      m_net;
    uint64_t         m_lastEntities{ 0 };
    uint64_t         m_baselineBytes{ 0 };
    double           m_dilation{ 1.0 };
    size_t           m_maxCapBind{ 0 };
    uint64_t         m_capBindTicks{ 0 };
};

} // namespace Neuron::Sim
