#pragma once
// Snapshot encoding — §8.4 state replication.
//
// M1a: a full per-tick snapshot of the player's visible entities (interest =
// "everything" until sector subscriptions + delta compression land at M4). The
// format is forward-compatible: clients read entityCount then that many records,
// so adding fields per record is a versioned change. Built on the tested serde
// primitives (versioned WriteBuffer/ReadBuffer).
//
// This loop is also the cold-start path: there is no bulk universe sync (§8.4). A
// freshly connected client starts from an empty baseline and converges as these
// interest-scoped snapshots arrive, so no transfer ever exceeds the safe MTU.
// Records are keyed by netId and applied last-writer-wins by snapshot `tick`, so
// reordered/duplicate snapshots are idempotent and need no inter-packet sequencing
// (snapshots ride the Unreliable channel). Per-tick MTU budgeting, baseline delta
// and interest eviction land with sector subscriptions at M4.
//
// Record: netId u32 · kind u8 · pos(x,y,z) i64 · localOffset(x,y,z) f32 · hp i32
//         · shapeId u16 (index into ShapeCatalog; selects the client mesh)
//         · ownerPlayer u32 (0 = NPC/unowned; drives client IFF + overview, M3 area G)

#include "Components.h"
#include "Serde.h"
#include "UniversePos.h"

#include <cmath>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace Neuron::Sim
{

// One replicated entity as seen on the wire / decoded on the client.
struct SnapshotEntity
{
    uint32_t                netId{ 0 };
    EntityKind              kind{ EntityKind::Unknown };
    Neuron::Universe::UniversePos pos{};
    DirectX::XMFLOAT3       localOffset{ 0, 0, 0 };
    int32_t                 hp{ 0 };
    uint16_t                shapeId{ 0xFFFF }; // index into ShapeCatalog (kInvalidShapeId)
    uint32_t                ownerPlayer{ 0 };  // owning player net id (0 = NPC/unowned); IFF
};

struct Snapshot
{
    uint32_t                    tick{ 0 };
    std::vector<SnapshotEntity> entities;
};

// Encode a snapshot into a message body.
inline std::vector<uint8_t> EncodeSnapshot(const Snapshot& snap)
{
    Neuron::Serde::WriteBuffer wb(64 + snap.entities.size() * 48);
    wb.WriteUint32(snap.tick);
    wb.WriteUint16(static_cast<uint16_t>(snap.entities.size()));
    for (const auto& e : snap.entities) {
        wb.WriteUint32(e.netId);
        wb.WriteUint8(static_cast<uint8_t>(e.kind));
        wb.WriteInt64(e.pos.x);
        wb.WriteInt64(e.pos.y);
        wb.WriteInt64(e.pos.z);
        wb.WriteFloat(e.localOffset.x);
        wb.WriteFloat(e.localOffset.y);
        wb.WriteFloat(e.localOffset.z);
        wb.WriteUint32(static_cast<uint32_t>(e.hp));
        wb.WriteUint16(e.shapeId);
        wb.WriteUint32(e.ownerPlayer);
    }
    wb.Finalise();
    auto data = wb.Data();
    return { data.begin(), data.end() };
}

// Decode a snapshot from a message body. Returns false on malformed/short data.
[[nodiscard]] inline bool DecodeSnapshot(std::span<const uint8_t> body, Snapshot& out)
{
    Neuron::Serde::ReadBuffer rb(body);
    if (!rb.IsGood()) return false;

    out.tick = rb.ReadUint32();
    const uint16_t count = rb.ReadUint16();
    out.entities.clear();
    out.entities.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        SnapshotEntity e;
        e.netId         = rb.ReadUint32();
        e.kind          = static_cast<EntityKind>(rb.ReadUint8());
        e.pos.x         = rb.ReadInt64();
        e.pos.y         = rb.ReadInt64();
        e.pos.z         = rb.ReadInt64();
        e.localOffset.x = rb.ReadFloat();
        e.localOffset.y = rb.ReadFloat();
        e.localOffset.z = rb.ReadFloat();
        e.hp            = static_cast<int32_t>(rb.ReadUint32());
        e.shapeId       = rb.ReadUint16();
        e.ownerPlayer   = rb.ReadUint32();
        if (!rb.IsGood()) return false;
        out.entities.push_back(e);
    }
    return rb.IsGood();
}

// ===========================================================================
// M4 area C — quantized sector-local delta record (App. A, §8.4)
// ===========================================================================
//
// The scale wire format that replaces the fixed ~46 B absolute-int64 record
// above: a per-entity record is netId + a changed-field-mask flags byte, and
// only the changed fields follow. Position ships as a *sector-local quantized
// delta* — the entity's sector is known because it is in the client's interest
// set (area A), so an entity inside one sector costs only the bit-packed local
// position, and a stationary entity costs nothing (its version doesn't bump at
// area B, so it never enters the diff). No absolute int64 reaches the wire (R2).
//
// This is the codec only (encode/decode + a stateful client decoder + an
// MTU-byte-budget primitive). Which baseline a client diffs against, priority
// ordering, and cross-tick spillover are the area-E scheduler's job; the M3
// full-snapshot path above is untouched until E switches the live path over.

// Changed-field mask bits carried in a record's flags byte (App. A).
enum DeltaFlag : uint8_t
{
    DeltaPos    = 1 << 0, // sector-local quantized position (3 axes) present
    DeltaSector = 1 << 1, // sector id present (first sight / sector crossing)
    DeltaHp     = 1 << 2,
    DeltaShape  = 1 << 3,
    DeltaOwner  = 1 << 4,
    DeltaKind   = 1 << 5,
    DeltaTomb   = 1 << 7, // tombstone (left interest / destroyed) — emitted at area D
};

// Quantization: the sector-local span (kSectorSize m) maps to 2^bits steps per
// axis. 20 bits → step ≈ 16384 / 2^20 ≈ 0.0156 m (~1.6 cm), far under the visible
// jitter at the ~100 ms interpolation delay (§8.4) while keeping a moving entity's
// record near the App. B ~16 B/delta target. A tunable (the §19 open question;
// area J's load test sweeps it), not a literal.
inline constexpr int kPosQuantBitsPerAxis = 20;

// Header bits of an encoded delta snapshot (Serde version u32 · tick u32 · count u16).
inline constexpr int kDeltaHeaderBits = 32 + 32 + 16;

// ZigZag map a signed sector coordinate to an unsigned wire value (small magnitudes
// stay small). Sector coords fit int32 within the §6.3 region bounds and ride the
// wire only on a crossing/first sight, not every tick.
[[nodiscard]] inline uint32_t ZigZag32(int32_t v) noexcept
{
    return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
}
[[nodiscard]] inline int32_t UnZigZag32(uint32_t v) noexcept
{
    return static_cast<int32_t>((v >> 1) ^ (~(v & 1u) + 1u));
}

// Quantize a sector-local metre offset in [0, kSectorSize) to a bits-per-axis index.
[[nodiscard]] inline uint32_t QuantizeSectorLocal(double localMetres) noexcept
{
    constexpr double   span = static_cast<double>(Neuron::Universe::kSectorSize);
    constexpr uint64_t steps = uint64_t(1) << kPosQuantBitsPerAxis;
    constexpr uint32_t maxq = static_cast<uint32_t>(steps - 1);
    double clamped = localMetres < 0.0 ? 0.0 : (localMetres >= span ? span : localMetres);
    double q = std::round(clamped / span * static_cast<double>(steps));
    if (q < 0.0) q = 0.0;
    if (q > static_cast<double>(maxq)) q = static_cast<double>(maxq);
    return static_cast<uint32_t>(q);
}
[[nodiscard]] inline double DequantizeSectorLocal(uint32_t q) noexcept
{
    constexpr double   span = static_cast<double>(Neuron::Universe::kSectorSize);
    constexpr uint64_t steps = uint64_t(1) << kPosQuantBitsPerAxis;
    return (static_cast<double>(q) / static_cast<double>(steps)) * span;
}

// Full sector-local position (metres incl. fraction, each axis in [0, kSectorSize))
// of an absolute position + its sector-local float offset.
inline void SectorLocalMetres(const Neuron::Universe::UniversePos& pos,
                              const DirectX::XMFLOAT3& localOffset,
                              Neuron::Universe::SectorId& sectorOut, double outMetres[3]) noexcept
{
    sectorOut = Neuron::Universe::UniverseToSector(pos);
    const Neuron::Universe::UniversePos origin = Neuron::Universe::SectorToOrigin(sectorOut);
    outMetres[0] = static_cast<double>(pos.x - origin.x) + localOffset.x;
    outMetres[1] = static_cast<double>(pos.y - origin.y) + localOffset.y;
    outMetres[2] = static_cast<double>(pos.z - origin.z) + localOffset.z;
}

// One decoded/decodable delta record. The intermediate the codec reads/writes; the
// stateful decoder applies it onto the client's last-known entity (LWW by tick).
struct DeltaRecord
{
    uint32_t                   netId{ 0 };
    uint8_t                    mask{ 0 };
    Neuron::Universe::SectorId sector{};            // valid iff mask & DeltaSector
    uint32_t                   qpos[3]{ 0, 0, 0 };  // valid iff mask & DeltaPos
    int32_t                    hp{ 0 };             // valid iff mask & DeltaHp
    uint16_t                   shapeId{ 0xFFFF };   // valid iff mask & DeltaShape
    uint32_t                   ownerPlayer{ 0 };    // valid iff mask & DeltaOwner
    uint8_t                    kind{ 0 };           // valid iff mask & DeltaKind
};

struct DeltaSnapshot
{
    uint32_t                 tick{ 0 };
    std::vector<DeltaRecord> records;
};

// Build the minimal delta record for 'cur' against the client's last-known 'base'
// (nullptr = first sight → full record). Position is compared at quantized
// resolution, so sub-quantum jitter sets no bit and an unchanged entity yields
// mask 0 (no record). A sector change forces the position to be re-sent so the
// client's local frame is unambiguous.
[[nodiscard]] inline DeltaRecord MakeDeltaRecord(const SnapshotEntity& cur, const SnapshotEntity* base)
{
    DeltaRecord r;
    r.netId = cur.netId;
    Neuron::Universe::SectorId sec; double loc[3];
    SectorLocalMetres(cur.pos, cur.localOffset, sec, loc);
    const uint32_t q[3] = { QuantizeSectorLocal(loc[0]), QuantizeSectorLocal(loc[1]), QuantizeSectorLocal(loc[2]) };
    r.sector = sec; r.qpos[0] = q[0]; r.qpos[1] = q[1]; r.qpos[2] = q[2];
    r.hp = cur.hp; r.shapeId = cur.shapeId; r.ownerPlayer = cur.ownerPlayer;
    r.kind = static_cast<uint8_t>(cur.kind);

    if (!base) { // first sight → every field present
        r.mask = DeltaPos | DeltaSector | DeltaHp | DeltaShape | DeltaOwner | DeltaKind;
        return r;
    }
    Neuron::Universe::SectorId bsec; double bloc[3];
    SectorLocalMetres(base->pos, base->localOffset, bsec, bloc);
    const uint32_t bq[3] = { QuantizeSectorLocal(bloc[0]), QuantizeSectorLocal(bloc[1]), QuantizeSectorLocal(bloc[2]) };
    if (!(sec == bsec))                              r.mask |= DeltaSector;
    if (q[0] != bq[0] || q[1] != bq[1] || q[2] != bq[2]) r.mask |= DeltaPos;
    if (r.mask & DeltaSector)                        r.mask |= DeltaPos; // re-anchor on crossing
    if (cur.hp != base->hp)                          r.mask |= DeltaHp;
    if (cur.shapeId != base->shapeId)                r.mask |= DeltaShape;
    if (cur.ownerPlayer != base->ownerPlayer)        r.mask |= DeltaOwner;
    if (static_cast<uint8_t>(cur.kind) != static_cast<uint8_t>(base->kind)) r.mask |= DeltaKind;
    return r;
}

// Encoded bit length of a record (for the MTU budget; matches WriteDeltaRecord).
[[nodiscard]] inline int DeltaRecordBits(const DeltaRecord& r) noexcept
{
    int bits = 32 + 8; // netId + flags
    if (r.mask & DeltaTomb) return bits;
    if (r.mask & DeltaSector) bits += 3 * 32;
    if (r.mask & DeltaPos)    bits += 3 * kPosQuantBitsPerAxis;
    if (r.mask & DeltaHp)     bits += 32;
    if (r.mask & DeltaShape)  bits += 16;
    if (r.mask & DeltaOwner)  bits += 32;
    if (r.mask & DeltaKind)   bits += 8;
    return bits;
}

inline void WriteDeltaRecord(Neuron::Serde::WriteBuffer& wb, const DeltaRecord& r)
{
    wb.WriteUint32(r.netId);
    wb.WriteUint8(r.mask);
    if (r.mask & DeltaTomb) return; // tombstone: netId + flags only (App. A)
    if (r.mask & DeltaSector) {
        wb.WriteUint32(ZigZag32(static_cast<int32_t>(r.sector.x)));
        wb.WriteUint32(ZigZag32(static_cast<int32_t>(r.sector.y)));
        wb.WriteUint32(ZigZag32(static_cast<int32_t>(r.sector.z)));
    }
    if (r.mask & DeltaPos) {
        wb.WriteBits(r.qpos[0], kPosQuantBitsPerAxis);
        wb.WriteBits(r.qpos[1], kPosQuantBitsPerAxis);
        wb.WriteBits(r.qpos[2], kPosQuantBitsPerAxis);
    }
    if (r.mask & DeltaHp)    wb.WriteUint32(static_cast<uint32_t>(r.hp));
    if (r.mask & DeltaShape) wb.WriteUint16(r.shapeId);
    if (r.mask & DeltaOwner) wb.WriteUint32(r.ownerPlayer);
    if (r.mask & DeltaKind)  wb.WriteUint8(r.kind);
}

[[nodiscard]] inline bool ReadDeltaRecord(Neuron::Serde::ReadBuffer& rb, DeltaRecord& r)
{
    r = {};
    r.netId = rb.ReadUint32();
    r.mask  = rb.ReadUint8();
    if (!rb.IsGood()) return false;
    if (r.mask & DeltaTomb) return rb.IsGood();
    if (r.mask & DeltaSector) {
        r.sector.x = UnZigZag32(rb.ReadUint32());
        r.sector.y = UnZigZag32(rb.ReadUint32());
        r.sector.z = UnZigZag32(rb.ReadUint32());
    }
    if (r.mask & DeltaPos) {
        r.qpos[0] = static_cast<uint32_t>(rb.ReadBits(kPosQuantBitsPerAxis));
        r.qpos[1] = static_cast<uint32_t>(rb.ReadBits(kPosQuantBitsPerAxis));
        r.qpos[2] = static_cast<uint32_t>(rb.ReadBits(kPosQuantBitsPerAxis));
    }
    if (r.mask & DeltaHp)    r.hp          = static_cast<int32_t>(rb.ReadUint32());
    if (r.mask & DeltaShape) r.shapeId     = rb.ReadUint16();
    if (r.mask & DeltaOwner) r.ownerPlayer = rb.ReadUint32();
    if (r.mask & DeltaKind)  r.kind        = rb.ReadUint8();
    return rb.IsGood();
}

inline std::vector<uint8_t> EncodeDeltaSnapshot(const DeltaSnapshot& snap)
{
    Neuron::Serde::WriteBuffer wb(16 + snap.records.size() * 16);
    wb.WriteUint32(snap.tick);
    wb.WriteUint16(static_cast<uint16_t>(snap.records.size()));
    for (const auto& r : snap.records) WriteDeltaRecord(wb, r);
    wb.Finalise();
    auto data = wb.Data();
    return { data.begin(), data.end() };
}

[[nodiscard]] inline bool DecodeDeltaSnapshot(std::span<const uint8_t> body, DeltaSnapshot& out)
{
    Neuron::Serde::ReadBuffer rb(body);
    if (!rb.IsGood()) return false;
    out.tick = rb.ReadUint32();
    const uint16_t count = rb.ReadUint16();
    out.records.clear();
    out.records.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        DeltaRecord r;
        if (!ReadDeltaRecord(rb, r)) return false;
        out.records.push_back(r);
    }
    return rb.IsGood();
}

// Fit records (already in priority order) into a 'byteBudget' snapshot: the prefix
// that fits is kept; the remainder spill to 'overflow' (re-scheduled next tick by
// area E — area B keeps their version > acked, so none are dropped). Holds the
// §8.2/§8.4 "never larger than MTU, never fragmented" invariant. mask-0 records
// (stationary) are skipped — they carry nothing.
[[nodiscard]] inline DeltaSnapshot BuildBudgetedSnapshot(uint32_t tick,
        const std::vector<DeltaRecord>& ordered, size_t byteBudget, std::vector<uint32_t>& overflow)
{
    DeltaSnapshot snap;
    snap.tick = tick;
    overflow.clear();
    long long bits = kDeltaHeaderBits;
    for (size_t i = 0; i < ordered.size(); ++i) {
        const DeltaRecord& r = ordered[i];
        if (r.mask == 0) continue; // nothing to send
        const long long withRecord = bits + DeltaRecordBits(r);
        if (static_cast<size_t>((withRecord + 7) / 8) > byteBudget) {
            for (size_t j = i; j < ordered.size(); ++j)
                if (ordered[j].mask != 0) overflow.push_back(ordered[j].netId);
            break;
        }
        bits = withRecord;
        snap.records.push_back(r);
    }
    return snap;
}

// Stateful client-side decode (§8.4): keeps each entity's last-known full state and
// applies masked deltas onto it, last-writer-wins by tick — so reordered/duplicate
// snapshots are idempotent (the same rule the M3 full path uses, now per record).
// Absolute position is reconstructed from the entity's sector + the decoded local
// delta; no absolute int64 ever crosses the wire.
class DeltaDecodeState
{
public:
    // Apply one encoded delta-snapshot body; false on malformed data.
    bool Apply(std::span<const uint8_t> body)
    {
        DeltaSnapshot snap;
        if (!DecodeDeltaSnapshot(body, snap)) return false;
        if (snap.tick > m_latestTick) m_latestTick = snap.tick;
        for (const DeltaRecord& r : snap.records) ApplyRecord(snap.tick, r);
        return true;
    }

    // Highest tick applied so far (the accumulated state's logical time).
    [[nodiscard]] uint32_t LatestTick() const noexcept { return m_latestTick; }

    [[nodiscard]] const SnapshotEntity* Find(uint32_t netId) const
    {
        auto it = m_entities.find(netId);
        return it == m_entities.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const std::unordered_map<uint32_t, SnapshotEntity>& Entities() const noexcept { return m_entities; }
    [[nodiscard]] size_t Size() const noexcept { return m_entities.size(); }

private:
    void ApplyRecord(uint32_t tick, const DeltaRecord& r)
    {
        auto tickIt = m_lastTick.find(r.netId);
        if (tickIt != m_lastTick.end() && tick <= tickIt->second) return; // LWW: stale/duplicate
        m_lastTick[r.netId] = tick;

        if (r.mask & DeltaTomb) { m_entities.erase(r.netId); return; } // despawn (area D)

        SnapshotEntity& e = m_entities[r.netId];
        e.netId = r.netId;
        const Neuron::Universe::SectorId sec =
            (r.mask & DeltaSector) ? r.sector : Neuron::Universe::UniverseToSector(e.pos);
        if (r.mask & DeltaPos) {
            const Neuron::Universe::UniversePos origin = Neuron::Universe::SectorToOrigin(sec);
            const double lx = DequantizeSectorLocal(r.qpos[0]);
            const double ly = DequantizeSectorLocal(r.qpos[1]);
            const double lz = DequantizeSectorLocal(r.qpos[2]);
            e.pos = { origin.x + static_cast<int64_t>(std::floor(lx)),
                      origin.y + static_cast<int64_t>(std::floor(ly)),
                      origin.z + static_cast<int64_t>(std::floor(lz)) };
            e.localOffset = { static_cast<float>(lx - std::floor(lx)),
                              static_cast<float>(ly - std::floor(ly)),
                              static_cast<float>(lz - std::floor(lz)) };
        }
        if (r.mask & DeltaHp)    e.hp          = r.hp;
        if (r.mask & DeltaShape) e.shapeId     = r.shapeId;
        if (r.mask & DeltaOwner) e.ownerPlayer = r.ownerPlayer;
        if (r.mask & DeltaKind)  e.kind        = static_cast<EntityKind>(r.kind);
    }

    std::unordered_map<uint32_t, SnapshotEntity> m_entities;
    std::unordered_map<uint32_t, uint32_t>       m_lastTick; // netId → last applied tick (LWW)
    uint32_t                                     m_latestTick{ 0 };
};

} // namespace Neuron::Sim
