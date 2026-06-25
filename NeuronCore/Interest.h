#pragma once
// Interest.h — cell publish/subscribe interest grid (§8.4, §6.3; M4 area A).
//
// Replaces the M3 O(players × entities) per-tick fog rebuild
// (ServerUniverse::DetectedSet / BuildSnapshotFor) with a SectorId-keyed grid of
// cells. Each cell holds its resident entities and its subscriber players, so a
// mutation in a cell is enqueued *once* to that cell's subscribers — O(Σ subs),
// not a per-client scan of every entity. Entities enter/leave cells as they cross
// sector boundaries (the §8.4 tombstone rule consumes those leave events at
// area D); a player subscribes to the neighbourhood of cells around its sensor
// sources and, on warp/jump, pre-subscribes the destination (R21). Keyed by
// SectorHash (§6.3) — no 64-bit Morton key.
//
// Pure and platform-independent (mirrored on the Linux testrunner, §16.2). This is
// the *spatial* half of interest; the always-known overlays (own entities outside
// sensor range, the beacon graph, scanned contacts) stay layered on top in
// ServerUniverse, exactly as M3's DetectedSet folded them.

#include "UniversePos.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Neuron::Sim
{

using Neuron::Universe::SectorHash;
using Neuron::Universe::SectorId;

// Append every cell in the Chebyshev cube of half-width 'radiusCells' centred on
// 'center' to 'out' — the set a sensor source at 'center' subscribes to.
inline void CollectNeighbourhood(const SectorId& center, int radiusCells,
                                 std::vector<SectorId>& out)
{
    const int r = radiusCells < 0 ? 0 : radiusCells;
    for (int dx = -r; dx <= r; ++dx)
        for (int dy = -r; dy <= r; ++dy)
            for (int dz = -r; dz <= r; ++dz)
                out.push_back({ center.x + dx, center.y + dy, center.z + dz });
}

// Cell radius a metric sensor range spans: ceil(range / sector size), so a
// player's neighbourhood covers every cell its sensors can reach (§6.3).
[[nodiscard]] inline int SectorRadiusForRange(float metres) noexcept
{
    if (metres <= 0.0f) return 0;
    const double cells = static_cast<double>(metres) /
                         static_cast<double>(Neuron::Universe::SECTOR_SIZE);
    return static_cast<int>(std::ceil(cells));
}

class InterestGrid
{
public:
    // One residency change. 'changed' is false when an UpdateResidency call did
    // not move the entity across a sector boundary; 'hadPrevious' is false on the
    // entity's first residency (an enter with no matching leave).
    struct CrossEvent
    {
        bool     changed{ false };
        bool     hadPrevious{ false };
        SectorId from{};
        SectorId to{};
    };

    // The cells a SetSubscription call added / removed for a player.
    struct SubDelta
    {
        std::vector<SectorId> entered;
        std::vector<SectorId> left;
    };

    // --- residency (entities) ------------------------------------------------

    // Set/refresh an entity's residency cell. Crossing a boundary emits exactly
    // one leave (its previous cell) + one enter ('sector'); an unchanged sector is
    // a no-op. The first call for a netId is an enter only (hadPrevious == false).
    CrossEvent UpdateResidency(uint32_t netId, const SectorId& sector)
    {
        CrossEvent ev;
        ev.to = sector;
        auto it = m_entitySector.find(netId);
        if (it == m_entitySector.end()) {
            m_entitySector.emplace(netId, sector);
            AddResident(sector, netId);
            ev.changed = true;
            return ev;
        }
        ev.hadPrevious = true;
        ev.from = it->second;
        if (it->second == sector) return ev; // no crossing
        RemoveResident(it->second, netId);
        AddResident(sector, netId);
        it->second = sector;
        ev.changed = true;
        return ev;
    }

    // Drop an entity from the grid (despawn / destruction).
    void Remove(uint32_t netId)
    {
        auto it = m_entitySector.find(netId);
        if (it == m_entitySector.end()) return;
        RemoveResident(it->second, netId);
        m_entitySector.erase(it);
    }

    [[nodiscard]] bool ResidentCell(uint32_t netId, SectorId& out) const
    {
        auto it = m_entitySector.find(netId);
        if (it == m_entitySector.end()) return false;
        out = it->second;
        return true;
    }

    [[nodiscard]] const std::vector<uint32_t>& Residents(const SectorId& s) const
    {
        auto it = m_cells.find(s);
        return it == m_cells.end() ? EMPTY32 : it->second.residents;
    }

    // --- subscriptions (players) ---------------------------------------------

    // Subscribe / unsubscribe a player to a single cell. Idempotent: returns true
    // only when the membership actually changed.
    bool Subscribe(uint32_t player, const SectorId& s)
    {
        if (!m_playerSubs[player].insert(s).second) return false;
        AddSubscriber(s, player);
        return true;
    }

    bool Unsubscribe(uint32_t player, const SectorId& s)
    {
        auto pit = m_playerSubs.find(player);
        if (pit == m_playerSubs.end() || pit->second.erase(s) == 0) return false;
        RemoveSubscriber(s, player);
        return true;
    }

    [[nodiscard]] bool IsSubscribed(uint32_t player, const SectorId& s) const
    {
        auto pit = m_playerSubs.find(player);
        return pit != m_playerSubs.end() && pit->second.count(s) != 0;
    }

    [[nodiscard]] const std::vector<uint32_t>& Subscribers(const SectorId& s) const
    {
        auto it = m_cells.find(s);
        return it == m_cells.end() ? EMPTY32 : it->second.subscribers;
    }

    // Warp/jump prefetch (R21): pin the destination neighbourhood so it survives
    // the next SetSubscription until the player's own sensor neighbourhood covers
    // it (arrival), so fast cross-sector travel never stalls replication.
    void PreSubscribe(uint32_t player, const SectorId& center, int radiusCells)
    {
        std::vector<SectorId> cells;
        CollectNeighbourhood(center, radiusCells, cells);
        auto& pin = m_playerPin[player];
        for (const SectorId& c : cells) {
            pin.insert(c);
            Subscribe(player, c);
        }
    }

    // Replace a player's sensor-driven subscription with exactly 'cells' (∪ any
    // pinned prefetch). Returns the enter/leave delta against the prior set. A
    // pinned cell the new neighbourhood now covers is treated as "arrived" and
    // unpinned, so prefetch is held only until the player reaches it.
    SubDelta SetSubscription(uint32_t player, std::span<const SectorId> cells)
    {
        std::unordered_set<SectorId, SectorHash> target(cells.begin(), cells.end());
        auto pit = m_playerPin.find(player);
        if (pit != m_playerPin.end()) {
            for (auto pi = pit->second.begin(); pi != pit->second.end();) {
                if (target.count(*pi)) pi = pit->second.erase(pi); // covered → arrived
                else { target.insert(*pi); ++pi; }                 // still pinned
            }
            if (pit->second.empty()) m_playerPin.erase(pit);
        }
        return ApplyTarget(player, target);
    }

    [[nodiscard]] const std::unordered_set<SectorId, SectorHash>&
    Subscriptions(uint32_t player) const
    {
        auto it = m_playerSubs.find(player);
        return it == m_playerSubs.end() ? EMPTY_SECTORS : it->second;
    }

    // Drop a player entirely (disconnect): clears its subscriptions + any pins.
    void RemovePlayer(uint32_t player)
    {
        auto it = m_playerSubs.find(player);
        if (it != m_playerSubs.end()) {
            for (const SectorId& s : it->second) RemoveSubscriber(s, player);
            m_playerSubs.erase(it);
        }
        m_playerPin.erase(player);
    }

    // Entities visible to a player = the union of residents over its subscribed
    // cells (deduplicated). The spatial interest set; callers overlay always-known
    // contacts (own/beacons/scanned). 'out' is cleared first.
    void VisibleTo(uint32_t player, std::vector<uint32_t>& out) const
    {
        out.clear();
        auto it = m_playerSubs.find(player);
        if (it == m_playerSubs.end()) return;
        for (const SectorId& s : it->second)
            for (uint32_t e : Residents(s)) out.push_back(e);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    // Diagnostics: number of live cells (residents and/or subscribers present).
    [[nodiscard]] size_t CellCount() const noexcept { return m_cells.size(); }

private:
    struct Cell
    {
        std::vector<uint32_t> residents;   // sorted (deterministic across runs)
        std::vector<uint32_t> subscribers; // sorted (deterministic across runs)
    };
    using CellMap = std::unordered_map<SectorId, Cell, SectorHash>;

    // Sorted insert/erase keep per-cell lists order-stable regardless of ECS or
    // hash-map iteration order — the lists feed deterministic snapshot scheduling.
    static void SortedInsert(std::vector<uint32_t>& v, uint32_t x)
    {
        auto it = std::lower_bound(v.begin(), v.end(), x);
        if (it == v.end() || *it != x) v.insert(it, x);
    }
    static void SortedErase(std::vector<uint32_t>& v, uint32_t x)
    {
        auto it = std::lower_bound(v.begin(), v.end(), x);
        if (it != v.end() && *it == x) v.erase(it);
    }

    void AddResident(const SectorId& s, uint32_t e) { SortedInsert(m_cells[s].residents, e); }
    void RemoveResident(const SectorId& s, uint32_t e)
    {
        auto it = m_cells.find(s);
        if (it == m_cells.end()) return;
        SortedErase(it->second.residents, e);
        PruneCell(it);
    }
    void AddSubscriber(const SectorId& s, uint32_t p) { SortedInsert(m_cells[s].subscribers, p); }
    void RemoveSubscriber(const SectorId& s, uint32_t p)
    {
        auto it = m_cells.find(s);
        if (it == m_cells.end()) return;
        SortedErase(it->second.subscribers, p);
        PruneCell(it);
    }
    void PruneCell(CellMap::iterator it)
    {
        if (it->second.residents.empty() && it->second.subscribers.empty())
            m_cells.erase(it);
    }

    // Move a player's subscription set to exactly 'target', recording the delta.
    SubDelta ApplyTarget(uint32_t player, const std::unordered_set<SectorId, SectorHash>& target)
    {
        SubDelta delta;
        auto& cur = m_playerSubs[player];
        for (auto ci = cur.begin(); ci != cur.end();) {
            if (target.count(*ci) == 0) {
                RemoveSubscriber(*ci, player);
                delta.left.push_back(*ci);
                ci = cur.erase(ci);
            } else {
                ++ci;
            }
        }
        for (const SectorId& s : target) {
            if (cur.insert(s).second) {
                AddSubscriber(s, player);
                delta.entered.push_back(s);
            }
        }
        return delta;
    }

    CellMap                                m_cells;
    std::unordered_map<uint32_t, SectorId> m_entitySector; // netId → current cell
    std::unordered_map<uint32_t, std::unordered_set<SectorId, SectorHash>> m_playerSubs;
    std::unordered_map<uint32_t, std::unordered_set<SectorId, SectorHash>> m_playerPin;

    static const std::vector<uint32_t>                          EMPTY32;
    static const std::unordered_set<SectorId, SectorHash>       EMPTY_SECTORS;
};

inline const std::vector<uint32_t>                    InterestGrid::EMPTY32{};
inline const std::unordered_set<SectorId, SectorHash> InterestGrid::EMPTY_SECTORS{};

// ---------------------------------------------------------------------------
// Per-entity replication version + per-client baseline (M4 area B, §8.4)
// ---------------------------------------------------------------------------

// The replicated projection of an entity — the fields a snapshot carries (App. A).
// Two ticks with identical ReplFields replicate identically, so the version only
// advances when one of these changes; an idle/stationary entity holds its version
// and costs ≈0 downstream. (Quantising position to a delta step is area C; here a
// change is any change to the authoritative fields.)
struct ReplFields
{
    int64_t  x{ 0 }, y{ 0 }, z{ 0 };          // absolute position (metres)
    float    lox{ 0 }, loy{ 0 }, loz{ 0 };    // sector-local offset
    int32_t  hp{ 0 };
    uint32_t ownerPlayer{ 0 };
    uint16_t shapeId{ 0xFFFF };
    uint8_t  kind{ 0 };
    bool operator==(const ReplFields&) const = default;
};

// Server-global monotonic version per entity (§8.4 "per-entity version/dirty
// stamping"). Kept as a side table keyed by netId — like InterestGrid residency —
// so every entity with a NetId is covered with no spawn-site coupling and the wire
// format / client stay untouched. Stamp() bumps the version iff the replicated
// fields changed since the last stamp (a newly-seen entity bumps 0 → 1).
class ReplicationStamps
{
public:
    // Stamp 'cur' for 'netId'; returns the (possibly advanced) current version.
    uint32_t Stamp(uint32_t netId, const ReplFields& cur)
    {
        auto it = m_state.find(netId);
        if (it == m_state.end()) {
            m_state.emplace(netId, Entry{ 1, cur });
            return 1;
        }
        if (!(it->second.last == cur)) {
            ++it->second.version;
            it->second.last = cur;
        }
        return it->second.version;
    }

    [[nodiscard]] uint32_t Version(uint32_t netId) const
    {
        auto it = m_state.find(netId);
        return it == m_state.end() ? 0u : it->second.version;
    }

    void Remove(uint32_t netId) { m_state.erase(netId); }
    [[nodiscard]] size_t Size() const noexcept { return m_state.size(); }

private:
    struct Entry { uint32_t version{ 0 }; ReplFields last; };
    std::unordered_map<uint32_t, Entry> m_state;
};

// Per-client replication baseline (§8.4 / §8.3): the last *acked* version the
// client holds for each netId, plus the not-yet-acked snapshots we've sent it. The
// diff is "server version > the client's acked version" — never "last sent" — so a
// dropped (un-acked) snapshot simply re-deltas from the still-current acked
// baseline and converges with no explicit retransmit. Allocated per connection;
// its size is an App. B RAM gate (area I reads ApproxBytes()).
class ClientBaseline
{
public:
    using SentList = std::vector<std::pair<uint32_t, uint32_t>>; // (netId, version)

    // Does the client still lack 'currentVersion' for 'netId'? (version > acked)
    [[nodiscard]] bool Needs(uint32_t netId, uint32_t currentVersion) const
    {
        auto it = m_acked.find(netId);
        return currentVersion > (it == m_acked.end() ? 0u : it->second);
    }

    [[nodiscard]] uint32_t Acked(uint32_t netId) const
    {
        auto it = m_acked.find(netId);
        return it == m_acked.end() ? 0u : it->second;
    }

    // Record what a snapshot for 'tick' carried, so an ack of that tick can advance
    // the baseline. Bounded: only the most recent MAX_PENDING unacked ticks are
    // kept (older lost snapshots re-delta from 'acked' anyway, so dropping their
    // record is safe).
    void RecordSent(uint32_t tick, const SentList& sent)
    {
        if (sent.empty()) return;
        m_pending[tick] = sent;
        while (m_pending.size() > MAX_PENDING) m_pending.erase(m_pending.begin());
    }

    // Advance the acked baseline to 'tick': fold every pending snapshot with key
    // ≤ tick into 'acked' (max version per netId), then drop them. Monotonic and
    // idempotent (a stale/duplicate ack does nothing new).
    void Ack(uint32_t tick)
    {
        for (auto it = m_pending.begin(); it != m_pending.end() && it->first <= tick;) {
            for (const auto& [netId, version] : it->second) {
                uint32_t& a = m_acked[netId];
                if (version > a) a = version;
            }
            it = m_pending.erase(it);
        }
        // A tombstone clears once the client acks a snapshot that carried it (area
        // D): every snapshot re-emits the pending set, so any ack ≥ the earliest
        // carrying tick proves a leave record was delivered (§8.4 self-healing).
        for (auto it = m_tombstones.begin(); it != m_tombstones.end();) {
            if (it->second != UNSENT && it->second <= tick) it = m_tombstones.erase(it);
            else ++it;
        }
    }

    // Tombstone reconciliation (area D) drops a netId from the baseline once the
    // client has acked its removal.
    void Forget(uint32_t netId) { m_acked.erase(netId); }

    // Every netId the client is known to hold (its acked baseline). Area D reads
    // this to find entities that have left interest (acked, but no longer visible)
    // and must be tombstoned. Appended in netId order (deterministic).
    void CollectAcked(std::vector<uint32_t>& out) const
    {
        for (const auto& [netId, version] : m_acked) out.push_back(netId);
        std::sort(out.begin(), out.end());
    }

    // --- tombstone eviction (area D, §8.4 / App. A) --------------------------
    //
    // When an entity leaves a client's interest set (cell leave, area A) or is
    // destroyed, the server marks a *tombstone* here and re-emits a leave record
    // (netId + DeltaTomb) on every snapshot to this client until the client acks a
    // snapshot that carried it — so a single lost despawn on the Unreliable channel
    // never leaves a ghost forever (the M3 full-rebuild "evict by omission" bug at
    // delta scale). Self-healing: a dropped leave is simply re-sent next tick.

    // Mark 'netId' pending-removal for this client. Drops its acked baseline entry
    // (it is gone) and queues a tombstone record. Idempotent.
    void Tombstone(uint32_t netId)
    {
        m_acked.erase(netId);
        m_tombstones.try_emplace(netId, UNSENT); // not yet emitted
    }

    // The entity re-entered interest before its removal was acked: cancel the
    // tombstone (it is alive again; the normal version diff re-sends its state).
    void Untombstone(uint32_t netId) { m_tombstones.erase(netId); }

    [[nodiscard]] bool IsTombstoned(uint32_t netId) const { return m_tombstones.count(netId) != 0; }

    // Append every currently-pending tombstone netId (the scheduler emits one
    // DeltaTomb record each). netId order (std::map) — deterministic.
    void CollectTombstones(std::vector<uint32_t>& out) const
    {
        for (const auto& [netId, sentTick] : m_tombstones) out.push_back(netId);
    }

    // Record that the snapshot for 'tick' carried tombstone records for 'netIds',
    // so an ack of 'tick' (or any later tick — every snapshot re-emits the still-
    // pending set) clears them. Keeps the earliest carrying tick per tombstone.
    void RecordTombstonesSent(uint32_t tick, const std::vector<uint32_t>& netIds)
    {
        for (uint32_t n : netIds) {
            auto it = m_tombstones.find(n);
            if (it != m_tombstones.end() && it->second == UNSENT) it->second = tick;
        }
    }

    [[nodiscard]] size_t TombstoneCount() const noexcept { return m_tombstones.size(); }
    [[nodiscard]] size_t AckedCount() const noexcept { return m_acked.size(); }
    [[nodiscard]] size_t PendingCount() const noexcept { return m_pending.size(); }

    // Approximate resident bytes (App. B baseline-RAM gate; area I telemetry).
    [[nodiscard]] size_t ApproxBytes() const noexcept
    {
        size_t bytes = m_acked.size() * (sizeof(uint32_t) * 2);
        for (const auto& [tick, sent] : m_pending)
            bytes += sizeof(uint32_t) + sent.size() * (sizeof(uint32_t) * 2);
        bytes += m_tombstones.size() * (sizeof(uint32_t) * 2);
        return bytes;
    }

private:
    static constexpr size_t   MAX_PENDING = 64;          // bounded unacked-snapshot history
    static constexpr uint32_t UNSENT     = 0xFFFFFFFFu; // tombstone queued, not yet emitted
    std::unordered_map<uint32_t, uint32_t>      m_acked;   // netId → acked version
    std::map<uint32_t, SentList>                m_pending; // tick → sent (netId,version)
    std::map<uint32_t, uint32_t>                m_tombstones; // netId → earliest carrying tick
};

} // namespace Neuron::Sim
