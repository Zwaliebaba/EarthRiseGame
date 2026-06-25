#pragma once
// In-memory loopback network for unit/integration tests — §16, R10.
//
// Platform-independent (NO Winsock). Models a shared "wire" that connects several
// virtual LoopbackSockets keyed by local port, and injects the network
// impairments the M1a acceptance tests need:
//   * loss        — drop a datagram with probability lossProbability
//   * duplication — deliver a datagram twice with probability dupProbability
//   * reorder     — hold a datagram in a small buffer and release it out of order,
//                   up to reorderDepth datagrams deep
//
// Determinism (the whole point for R10): all random decisions are driven by a
// single seeded SplitMix64 PRNG. The same Impairments.seed + the same sequence of
// SendTo calls yields the same delivered/dropped/duplicated pattern on every run
// and every platform (we do not rely on <random> engine portability).
//
// Threading: not thread-safe. Tests drive it single-threaded.

#include "ISocket.h"

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace Neuron::Net
{

struct Impairments
{
    double   lossProbability = 0.0; // [0,1] chance a datagram is dropped
    double   dupProbability  = 0.0; // [0,1] chance a datagram is delivered twice
    uint32_t reorderDepth    = 0;   // how many datagrams may be held for reordering
    uint32_t seed            = 12345;
};

class LoopbackNetwork
{
public:
    LoopbackNetwork() { ResetRng(); }

    // (Re)configure impairments. Re-seeds the PRNG so a test that configures then
    // sends gets a reproducible sequence regardless of prior activity.
    void Configure(const Impairments& imp)
    {
        m_imp = imp;
        ResetRng();
        // Flush any packets being held for reorder so a fresh config starts clean.
        m_holdBuffer.clear();
    }

    const Impairments& GetImpairments() const noexcept { return m_imp; }

    // Register / unregister a virtual socket by its bound port.
    void Register(uint16_t port)
    {
        m_queues.try_emplace(port);
    }
    void Unregister(uint16_t port)
    {
        m_queues.erase(port);
        // Drop any held packets destined for this port.
        for (auto it = m_holdBuffer.begin(); it != m_holdBuffer.end();)
            it = (it->dstPort == port) ? m_holdBuffer.erase(it) : it + 1;
    }

    // A virtual socket sends a datagram. Applies loss / duplication / reordering.
    void Post(uint16_t srcPort, const Endpoint& srcEp, uint16_t dstPort,
              std::span<const uint8_t> data)
    {
        // 1) Loss.
        if (m_imp.lossProbability > 0.0 && NextDouble() < m_imp.lossProbability)
            return; // dropped

        // Build the datagram (one copy; duplication will copy again).
        Datagram dg;
        dg.from = srcEp;
        dg.from.port = srcPort;
        dg.dstPort = dstPort;
        dg.bytes.assign(data.begin(), data.end());

        // 2) Duplication: deliver an extra identical copy.
        const bool duplicate =
            m_imp.dupProbability > 0.0 && NextDouble() < m_imp.dupProbability;

        // 3) Reordering: if enabled, route through the hold buffer.
        if (m_imp.reorderDepth > 0) {
            m_holdBuffer.push_back(dg);
            if (duplicate)
                m_holdBuffer.push_back(dg);

            // When the hold buffer exceeds reorderDepth, release one packet chosen
            // pseudo-randomly from the buffer (this is what produces reordering).
            while (m_holdBuffer.size() > m_imp.reorderDepth)
                ReleaseOneHeld();
            return;
        }

        // No reordering: deliver immediately, in order.
        Deliver(dg);
        if (duplicate)
            Deliver(dg);
    }

    // Flush all packets currently held for reordering into their destination
    // queues. Tests call this after the send loop so nothing is left in flight.
    void Step()
    {
        while (!m_holdBuffer.empty())
            ReleaseOneHeld();
    }

    // Pop the next datagram for 'port'. Returns false if none queued.
    struct Inbound
    {
        Endpoint             from;
        std::vector<uint8_t> bytes;
    };
    bool Receive(uint16_t port, Inbound& out)
    {
        auto it = m_queues.find(port);
        if (it == m_queues.end() || it->second.empty())
            return false;
        out.from  = std::move(it->second.front().from);
        out.bytes = std::move(it->second.front().bytes);
        it->second.pop_front();
        return true;
    }

private:
    struct Datagram
    {
        Endpoint             from;
        uint16_t             dstPort = 0;
        std::vector<uint8_t> bytes;
    };

    void Deliver(const Datagram& dg)
    {
        auto it = m_queues.find(dg.dstPort);
        if (it == m_queues.end())
            return; // nothing bound on that port — dropped on the floor
        Datagram copy = dg;
        it->second.push_back(std::move(copy));
    }

    // Release one packet from the hold buffer, chosen by the PRNG, to produce
    // deterministic out-of-order delivery.
    void ReleaseOneHeld()
    {
        if (m_holdBuffer.empty())
            return;
        const size_t idx =
            static_cast<size_t>(NextU64() % m_holdBuffer.size());
        Deliver(m_holdBuffer[idx]);
        m_holdBuffer.erase(m_holdBuffer.begin() + static_cast<std::ptrdiff_t>(idx));
    }

    // --- Deterministic PRNG (SplitMix64) -----------------------------------
    void ResetRng()
    {
        // Mix the 32-bit seed into a 64-bit state.
        m_rngState = (static_cast<uint64_t>(m_imp.seed) << 32) ^ 0x9E3779B97F4A7C15ull;
        if (m_rngState == 0)
            m_rngState = 0x9E3779B97F4A7C15ull;
    }

    uint64_t NextU64()
    {
        uint64_t z = (m_rngState += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Uniform double in [0,1).
    double NextDouble()
    {
        // Use the top 53 bits for a full-precision double mantissa.
        return static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
    }

    Impairments m_imp{};
    uint64_t    m_rngState = 0;

    std::unordered_map<uint16_t, std::deque<Datagram>> m_queues;
    std::vector<Datagram>                              m_holdBuffer;
};

// ----------------------------------------------------------------------------
// LoopbackSocket — an ISocket backed by a LoopbackNetwork.
// ----------------------------------------------------------------------------
class LoopbackSocket final : public ISocket
{
public:
    explicit LoopbackSocket(LoopbackNetwork* net) : m_net(net) {}
    ~LoopbackSocket() override { Close(); }

    bool Open(uint16_t localPort) override
    {
        if (!m_net)
            return false;
        Close();
        // Port 0 = ephemeral: pick a deterministic-but-unique port.
        m_port = (localPort != 0) ? localPort : AllocEphemeral();
        m_net->Register(m_port);
        m_open = true;
        return true;
    }

    void Close() override
    {
        if (m_open && m_net)
            m_net->Unregister(m_port);
        m_open = false;
    }

    int SendTo(const Endpoint& to, std::span<const uint8_t> data) override
    {
        if (!m_open || !m_net)
            return -1;
        Endpoint self;
        self.ip   = "127.0.0.1";
        self.port = m_port;
        m_net->Post(m_port, self, to.port, data);
        return static_cast<int>(data.size());
    }

    int RecvFrom(Endpoint& from, std::span<uint8_t> buffer) override
    {
        if (!m_open || !m_net)
            return -1;
        LoopbackNetwork::Inbound in;
        if (!m_net->Receive(m_port, in))
            return 0; // nothing available
        from = in.from;
        const size_t n = (in.bytes.size() < buffer.size()) ? in.bytes.size()
                                                            : buffer.size();
        for (size_t i = 0; i < n; ++i)
            buffer[i] = in.bytes[i];
        return static_cast<int>(n); // truncates silently if buffer too small (like UDP)
    }

    [[nodiscard]] uint16_t LocalPort() const override { return m_port; }

private:
    static uint16_t AllocEphemeral()
    {
        // Deterministic monotonically increasing ephemeral ports in the dynamic
        // range. Static so multiple sockets on one process don't collide.
        static uint16_t s_next = 49152;
        if (s_next == 0) s_next = 49152;
        return s_next++;
    }

    LoopbackNetwork* m_net  = nullptr;
    uint16_t         m_port = 0;
    bool             m_open = false;
};

} // namespace Neuron::Net
