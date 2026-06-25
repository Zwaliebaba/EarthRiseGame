#pragma once
// SnapshotScheduler.h — priority / quota per-client snapshot scheduler (M4 area E,
// §8.4) plus the per-client delta-base cache the codec encodes against.
//
// Area B's version stamping answers *which* entities a client still lacks (version
// > acked); this layer answers *in what order and how many* go on the wire this
// tick. A named priority function ranks the lacked + tombstone keys, a hard
// per-client visible cap (R16) ages the lowest-priority ones out, and area C's MTU
// byte budget (BuildBudgetedSnapshot) keeps the prefix that fits — the remainder
// spills to later ticks (area B holds their version > acked, so none are dropped).
// The same machinery serves both steady-state spillover and cold-start from the
// empty (∅) baseline: a fresh client is just one whose acked baseline is empty, so
// its area of interest dribbles in closest/most-important first (§8.4, no Bulk
// channel, no separate transfer phase).
//
// Encoding against a per-client baseline: ReplicationStamps says an entity changed,
// but a minimal record (omit the sector when the client already knows it, omit
// unchanged fields) needs the client's last-*acked* full state. ClientKnownState
// holds exactly that — advanced on the §8.3 snapshot ack, so a dropped snapshot
// re-deltas from the still-current acked base (never the last sent). Its size is the
// App. B per-client baseline-RAM gate (area I reads ApproxBytes()).
//
// Pure and platform-independent (mirrored on the Linux testrunner, §16.2).

#include "Navigation.h" // UniverseDistance
#include "Snapshot.h"
#include "UniversePos.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace Neuron::Sim
{

// IFF classification of a candidate relative to the client (drives relevance).
enum class Iff : uint8_t { Own, Enemy, Neutral };

// Relevance / staleness weights (§8.4 named priority function). These are the §19
// open-question *tunables*, authored here so area J's load test can sweep them, not
// literals scattered through the scheduler. First-pass values; the contested-sector
// run validates them.
struct RelevanceWeights
{
    float own          = 4.0f;   // the client's own units
    float enemy        = 3.0f;   // hostile (IFF) contacts
    float neutral      = 1.0f;   // everything else
    float baseBonus    = 2.0f;   // added when the entity is a base/structure
    float targetBonus  = 3.0f;   // added when it is the client's current command target
    float distanceScale = 16000.0f; // metres at which distance halves relevance (~1 sector)
    float stalenessPerTick = 0.05f;  // relevance growth per tick since last sent (anti-starve)
};

// One entity in a client's interest set, projected for ranking this tick.
struct SchedCandidate
{
    uint32_t netId{ 0 };
    double   distance{ 0.0 };   // metres from the client's focus (its base)
    Iff      iff{ Iff::Neutral };
    bool     isBase{ false };
    bool     isTarget{ false };
    uint32_t staleness{ 0 };    // ticks since this client was last sent this netId
};

// priority = relevance(IFF, is-base, is-target) × distance-falloff × staleness.
// Higher is more urgent. Pure — the unit of the §8.4 priority model.
[[nodiscard]] inline double SnapshotPriority(const SchedCandidate& c,
                                             const RelevanceWeights& w) noexcept
{
    double rel = (c.iff == Iff::Own) ? w.own : (c.iff == Iff::Enemy) ? w.enemy : w.neutral;
    if (c.isBase)   rel += w.baseBonus;
    if (c.isTarget) rel += w.targetBonus;
    const double distFactor = 1.0 / (1.0 + c.distance / static_cast<double>(w.distanceScale));
    const double stale      = 1.0 + static_cast<double>(w.stalenessPerTick) * c.staleness;
    return rel * distFactor * stale;
}

// The scheduled order for a client this tick.
struct ScheduleResult
{
    std::vector<uint32_t> ordered; // netIds in descending priority, capped to N
    size_t                capped{ 0 }; // how many interest entities the cap dropped (R16 evidence)
};

// Rank 'cands' by priority and keep the top 'visibleCap' (R16 hard cap). Ties break
// by netId so the order is deterministic across runs (feeds the determinism gate).
// The dropped remainder ages out — its staleness grows, so it rises next tick and is
// never starved. Recording where the cap binds is the M4 evidence that aggregation /
// LOD is mandatory at M7 (App. B, R16), not built here.
[[nodiscard]] inline ScheduleResult ScheduleClient(std::vector<SchedCandidate> cands,
                                                   const RelevanceWeights& w, size_t visibleCap)
{
    std::sort(cands.begin(), cands.end(), [&](const SchedCandidate& a, const SchedCandidate& b) {
        const double pa = SnapshotPriority(a, w), pb = SnapshotPriority(b, w);
        if (pa != pb) return pa > pb;
        return a.netId < b.netId; // deterministic tie-break
    });
    ScheduleResult r;
    const size_t n = (visibleCap == 0 || cands.size() <= visibleCap) ? cands.size() : visibleCap;
    if (cands.size() > n) r.capped = cands.size() - n;
    r.ordered.reserve(n);
    for (size_t i = 0; i < n; ++i) r.ordered.push_back(cands[i].netId);
    return r;
}

// ---------------------------------------------------------------------------
// Per-client delta-base cache (the App. B per-client baseline, area E/B)
// ---------------------------------------------------------------------------
//
// The client's last-*acked* full state per netId — what a delta record is encoded
// against (area C's MakeDeltaRecord(cur, base)). Advanced only on the §8.3 ack
// (RecordSent stages the just-sent values per tick; Ack folds ticks ≤ acked into the
// known map), so an un-acked snapshot leaves the base untouched and the next tick
// re-deltas from it — convergent with no retransmit (§8.4). A cold client's map is
// empty, so every entity is first-sight (full record) until it acks: cold-start = ∅.
class ClientKnownState
{
public:
    // The client's acked state for 'netId' (nullptr = not yet known → first sight).
    [[nodiscard]] const SnapshotEntity* Base(uint32_t netId) const
    {
        auto it = m_known.find(netId);
        return it == m_known.end() ? nullptr : &it->second;
    }

    // Stage the full states a snapshot for 'tick' carried, so acking 'tick' makes
    // them the new base. Bounded to the most recent MAX_PENDING unacked ticks (older
    // lost snapshots re-delta from the acked base anyway).
    void RecordSent(uint32_t tick, std::vector<SnapshotEntity> sent)
    {
        if (sent.empty()) return;
        m_pending[tick] = std::move(sent);
        while (m_pending.size() > MAX_PENDING) m_pending.erase(m_pending.begin());
    }

    // Advance the known base to 'tick': fold every staged snapshot ≤ tick into the
    // known map (last wins by tick order), then drop them. Idempotent.
    void Ack(uint32_t tick)
    {
        for (auto it = m_pending.begin(); it != m_pending.end() && it->first <= tick;) {
            for (const SnapshotEntity& e : it->second) m_known[e.netId] = e;
            it = m_pending.erase(it);
        }
    }

    // Drop an entity once its removal is acked (tombstone reconcile, area D).
    void Forget(uint32_t netId) { m_known.erase(netId); }

    [[nodiscard]] size_t KnownCount() const noexcept { return m_known.size(); }

    // Approximate resident bytes (App. B per-client baseline-RAM gate; area I).
    [[nodiscard]] size_t ApproxBytes() const noexcept
    {
        size_t bytes = m_known.size() * sizeof(SnapshotEntity);
        for (const auto& [tick, sent] : m_pending)
            bytes += sizeof(uint32_t) + sent.size() * sizeof(SnapshotEntity);
        return bytes;
    }

private:
    static constexpr size_t MAX_PENDING = 64;
    std::unordered_map<uint32_t, SnapshotEntity>      m_known;   // netId → last-acked state
    std::map<uint32_t, std::vector<SnapshotEntity>>   m_pending; // tick → sent states
};

} // namespace Neuron::Sim
