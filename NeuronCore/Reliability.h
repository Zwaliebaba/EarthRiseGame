#pragma once
// Per-channel reliability — §8.3 (acks, 32-bit ack bitfield, RTT/RTO, dup detect).
//
// Platform-independent: no sockets, no clock calls. The owner feeds it a
// monotonic time in seconds (so tests can drive a virtual clock) and the raw
// payloads to send/receive. This is the heart of R10 (reliable-UDP correctness)
// and is exercised by the loss/reorder/dup unit tests.
//
// Reliability model:
//   - Each outgoing reliable message gets the next 16-bit sequence.
//   - Every datagram carries (ack, ackBits): ack = newest sequence received,
//     ackBits = the 32 sequences before it (bit i set ⇒ ack-1-i was received).
//   - On receiving an ack, matching unacked messages are retired and RTT updated.
//   - Unacked messages past their RTO are returned for retransmission.
//   - The receiver drops duplicates and (for ordered channels) buffers
//     out-of-order messages until the gap fills.

#include "SequenceMath.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Neuron::Net
{

// ---------------------------------------------------------------------------
// Outgoing-side reliability: sequence assignment, ack processing, RTO resend.
// ---------------------------------------------------------------------------
class ReliableSender
{
public:
    struct InFlight
    {
        uint16_t             sequence{ 0 };
        std::vector<uint8_t> payload;
        double               sentTime{ 0.0 };
        uint32_t             sendCount{ 0 };
    };

    // Assign the next sequence to a payload and track it for ack/resend.
    uint16_t Send(std::vector<uint8_t> payload, double now)
    {
        const uint16_t seq = m_nextSequence++;
        InFlight f;
        f.sequence  = seq;
        f.payload   = std::move(payload);
        f.sentTime  = now;
        f.sendCount = 1;
        m_inFlight[seq] = std::move(f);
        return seq;
    }

    // Process an incoming (ack, ackBits) pair: retire acked messages, update RTT.
    void OnAck(uint16_t ack, uint32_t ackBits, double now)
    {
        AckOne(ack, now);
        for (int i = 0; i < 32; ++i) {
            if ((ackBits >> i) & 1u) {
                const uint16_t seq = static_cast<uint16_t>(ack - 1 - i);
                AckOne(seq, now);
            }
        }
    }

    // Return sequences whose RTO has elapsed; caller resends their payloads.
    // Marks them resent (updates sentTime, bumps sendCount).
    std::vector<InFlight*> CollectTimedOut(double now)
    {
        std::vector<InFlight*> out;
        const double rto = CurrentRto();
        for (auto& [seq, f] : m_inFlight) {
            if (now - f.sentTime >= rto) {
                f.sentTime = now;
                ++f.sendCount;
                out.push_back(&f);
            }
        }
        return out;
    }

    [[nodiscard]] size_t InFlightCount() const noexcept { return m_inFlight.size(); }
    [[nodiscard]] double SmoothedRtt()   const noexcept { return m_srtt; }

    [[nodiscard]] double CurrentRto() const noexcept
    {
        // RFC 6298-style: RTO = SRTT + 4*RTTVAR, clamped.
        if (m_srtt <= 0.0) return kInitialRto;
        double rto = m_srtt + 4.0 * m_rttvar;
        if (rto < kMinRto) rto = kMinRto;
        if (rto > kMaxRto) rto = kMaxRto;
        return rto;
    }

private:
    void AckOne(uint16_t seq, double now)
    {
        auto it = m_inFlight.find(seq);
        if (it == m_inFlight.end()) return;
        // Only sample RTT for messages sent exactly once (Karn's algorithm).
        if (it->second.sendCount == 1)
            UpdateRtt(now - it->second.sentTime);
        m_inFlight.erase(it);
    }

    void UpdateRtt(double sample)
    {
        if (sample < 0.0) return;
        if (m_srtt <= 0.0) {
            m_srtt   = sample;
            m_rttvar = sample / 2.0;
        } else {
            constexpr double alpha = 1.0 / 8.0;
            constexpr double beta  = 1.0 / 4.0;
            const double diff = sample - m_srtt;
            m_rttvar = (1.0 - beta) * m_rttvar + beta * (diff < 0 ? -diff : diff);
            m_srtt   = (1.0 - alpha) * m_srtt + alpha * sample;
        }
    }

    static constexpr double kInitialRto = 0.2;  // 200 ms
    static constexpr double kMinRto     = 0.05; // 50 ms
    static constexpr double kMaxRto     = 2.0;  // 2 s

    uint16_t m_nextSequence{ 0 };
    std::unordered_map<uint16_t, InFlight> m_inFlight;
    double   m_srtt{ 0.0 };
    double   m_rttvar{ 0.0 };
};

// ---------------------------------------------------------------------------
// Incoming-side reliability: dup detection + ack-bitfield generation.
// ---------------------------------------------------------------------------
class ReliableReceiver
{
public:
    // Record a received sequence. Returns false if it is a duplicate
    // (already seen) — caller should drop the payload.
    bool OnReceive(uint16_t seq)
    {
        if (!m_seenAny) {
            m_seenAny  = true;
            m_latest   = seq;
            m_received.clear();
            m_received.insert(seq);
            return true;
        }

        if (m_received.count(seq))
            return false; // duplicate

        m_received.insert(seq);
        if (SeqGreater(seq, m_latest))
            m_latest = seq;

        // Bound memory: forget sequences far older than latest.
        PruneOld();
        return true;
    }

    // Newest received sequence — goes in the outgoing 'ack' field.
    [[nodiscard]] uint16_t Ack() const noexcept { return m_latest; }

    // 32-bit bitfield: bit i set ⇒ sequence (latest-1-i) was received.
    [[nodiscard]] uint32_t AckBits() const noexcept
    {
        uint32_t bits = 0;
        for (int i = 0; i < 32; ++i) {
            const uint16_t seq = static_cast<uint16_t>(m_latest - 1 - i);
            if (m_received.count(seq))
                bits |= (1u << i);
        }
        return bits;
    }

    [[nodiscard]] bool SeenAny() const noexcept { return m_seenAny; }

private:
    void PruneOld()
    {
        // Keep only sequences within 1024 of the latest.
        for (auto it = m_received.begin(); it != m_received.end();) {
            if (SeqDiff(m_latest, *it) > 1024)
                it = m_received.erase(it);
            else
                ++it;
        }
    }

    bool                          m_seenAny{ false };
    uint16_t                      m_latest{ 0 };
    std::unordered_set<uint16_t>  m_received;
};

} // namespace Neuron::Net
