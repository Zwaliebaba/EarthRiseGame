#pragma once
// Fragmentation & reassembly — §8.3 (~1200 B safe payload).
//
// Large messages (initial world sync on the Bulk channel) are split into
// fragments that each fit a single safe-MTU datagram, then reassembled on the
// receiver. Platform-independent and unit-tested for ordered, reordered and
// duplicate fragment delivery.

#include "Protocol.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Neuron::Net
{

// A single fragment ready to be placed in a datagram payload.
struct Fragment
{
    FragmentHeader       header;
    std::vector<uint8_t> data;
};

// Split a message into fragments of at most 'maxFragmentBytes' each.
[[nodiscard]] inline std::vector<Fragment>
Fragmentize(uint16_t messageId, const std::vector<uint8_t>& message,
            uint16_t maxFragmentBytes = kMaxPayloadBytes - 64)
{
    std::vector<Fragment> out;
    if (maxFragmentBytes == 0) return out;

    const size_t total = message.size();
    const size_t count = total == 0 ? 1 : (total + maxFragmentBytes - 1) / maxFragmentBytes;

    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t begin = i * maxFragmentBytes;
        const size_t end   = (begin + maxFragmentBytes < total) ? begin + maxFragmentBytes : total;

        Fragment f;
        f.header.messageId     = messageId;
        f.header.fragmentIndex = static_cast<uint16_t>(i);
        f.header.fragmentCount = static_cast<uint16_t>(count);
        f.data.assign(message.begin() + static_cast<std::ptrdiff_t>(begin),
                      message.begin() + static_cast<std::ptrdiff_t>(end));
        out.push_back(std::move(f));
    }
    return out;
}

// Accumulates fragments and yields the complete message once all arrive.
class Reassembler
{
public:
    // Feed a fragment. Returns the reassembled message when the last missing
    // fragment for its messageId arrives; std::nullopt otherwise.
    std::optional<std::vector<uint8_t>> Add(const Fragment& frag)
    {
        if (frag.header.fragmentCount == 0) return std::nullopt;

        auto& entry = m_partial[frag.header.messageId];
        if (entry.parts.empty()) {
            entry.count = frag.header.fragmentCount;
            entry.parts.resize(frag.header.fragmentCount);
            entry.have.assign(frag.header.fragmentCount, false);
            entry.received = 0;
        }

        // Guard against inconsistent fragmentCount or out-of-range index.
        if (frag.header.fragmentCount != entry.count) return std::nullopt;
        if (frag.header.fragmentIndex >= entry.count) return std::nullopt;

        const uint16_t idx = frag.header.fragmentIndex;
        if (!entry.have[idx]) {            // ignore duplicates
            entry.parts[idx] = frag.data;
            entry.have[idx]  = true;
            ++entry.received;
        }

        if (entry.received == entry.count) {
            std::vector<uint8_t> full;
            for (auto& p : entry.parts)
                full.insert(full.end(), p.begin(), p.end());
            m_partial.erase(frag.header.messageId);
            return full;
        }
        return std::nullopt;
    }

    [[nodiscard]] size_t PendingMessages() const noexcept { return m_partial.size(); }

private:
    struct Partial
    {
        uint16_t                          count{ 0 };
        uint16_t                          received{ 0 };
        std::vector<std::vector<uint8_t>> parts;
        std::vector<bool>                 have;
    };
    std::unordered_map<uint16_t, Partial> m_partial;
};

} // namespace Neuron::Net
