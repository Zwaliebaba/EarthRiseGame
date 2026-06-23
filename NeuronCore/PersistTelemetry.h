#pragma once
// PersistTelemetry.h — §21 persistence + auth counters (M5 area H).
//
// Extends the M4 sim/net telemetry (Telemetry.h) to the durability boundary and
// login health so the M5 restart/zero-loss drill (area I) is *measurable* and
// automatable: outbox depth + drain latency (area D), write-behind batch size/lag +
// the RPO watermark (area E), and login attempts / lockouts / rate-limit hits
// (area C). Pure aggregation core (mirrored on the Linux testrunner, §16.2); the
// ERServer/ERHeadless sampling sites + export (perf counters / ETW / structured logs)
// are the Win32 side, consumed by the headless drill (§16.3).

#include "Telemetry.h" // PercentileWindow (nearest-rank p50/p99)

#include <cstdint>

namespace Neuron::Sim
{

class PersistTelemetry
{
public:
    // --- write-through outbox (area D, §15) ----------------------------------
    void RecordOutboxDepth(uint64_t pendingRows) noexcept { m_outboxDepth = pendingRows; }
    void RecordOutboxDrainMs(double ms) { m_outboxDrainMs.Add(ms); }
    [[nodiscard]] uint64_t OutboxDepth() const noexcept { return m_outboxDepth; }
    [[nodiscard]] double   OutboxDrainP99() const { return m_outboxDrainMs.Percentile(0.99); }

    // --- write-behind high-frequency state (area E, §15) ---------------------
    void RecordWriteBehindBatch(uint64_t rows) noexcept { m_wbBatchRows = rows; m_wbBatches++; }
    void RecordWriteBehindLagMs(double ms) { m_wbLagMs.Add(ms); }
    // The RPO watermark: the sim-time (ms) up to which high-frequency state is durable.
    // Must advance monotonically; the drill asserts the restored position is within the
    // stated RPO of it. Economy is NEVER on this path (zero-loss, area D).
    void AdvanceRpoWatermark(uint64_t simTimeMs) noexcept
    {
        if (simTimeMs > m_rpoWatermarkMs) m_rpoWatermarkMs = simTimeMs;
    }
    [[nodiscard]] uint64_t WriteBehindBatchRows() const noexcept { return m_wbBatchRows; }
    [[nodiscard]] uint64_t WriteBehindBatches() const noexcept { return m_wbBatches; }
    [[nodiscard]] double   WriteBehindLagP99() const { return m_wbLagMs.Percentile(0.99); }
    [[nodiscard]] uint64_t RpoWatermarkMs() const noexcept { return m_rpoWatermarkMs; }

    // --- auth (area C, §14) --------------------------------------------------
    void RecordLoginAttempt() noexcept { ++m_loginAttempts; }
    void RecordLoginFailure() noexcept { ++m_loginFailures; }
    void RecordLockout() noexcept { ++m_lockouts; }
    void RecordRateLimitHit() noexcept { ++m_rateLimitHits; }
    [[nodiscard]] uint64_t LoginAttempts() const noexcept { return m_loginAttempts; }
    [[nodiscard]] uint64_t LoginFailures() const noexcept { return m_loginFailures; }
    [[nodiscard]] uint64_t Lockouts() const noexcept { return m_lockouts; }
    [[nodiscard]] uint64_t RateLimitHits() const noexcept { return m_rateLimitHits; }

private:
    uint64_t         m_outboxDepth{ 0 };
    PercentileWindow m_outboxDrainMs{ 256 };
    uint64_t         m_wbBatchRows{ 0 };
    uint64_t         m_wbBatches{ 0 };
    PercentileWindow m_wbLagMs{ 256 };
    uint64_t         m_rpoWatermarkMs{ 0 };
    uint64_t         m_loginAttempts{ 0 };
    uint64_t         m_loginFailures{ 0 };
    uint64_t         m_lockouts{ 0 };
    uint64_t         m_rateLimitHits{ 0 };
};

} // namespace Neuron::Sim
