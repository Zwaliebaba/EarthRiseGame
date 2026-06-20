#pragma once
// Snapshot encoding — §8.4 state replication.
//
// M1a: a full per-tick snapshot of the player's visible entities (interest =
// "everything" until sector subscriptions + delta compression land at M4). The
// format is forward-compatible: clients read entityCount then that many records,
// so adding fields per record is a versioned change. Built on the tested serde
// primitives (versioned WriteBuffer/ReadBuffer).
//
// This loop is also the cold-start path: there is no bulk world sync (§8.4). A
// freshly connected client starts from an empty baseline and converges as these
// interest-scoped snapshots arrive, so no transfer ever exceeds the safe MTU.
//
// Record: netId u32 · kind u8 · pos(x,y,z) i64 · localOffset(x,y,z) f32 · hp i32

#include "Components.h"
#include "Serde.h"
#include "WorldPos.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Neuron::Sim
{

// One replicated entity as seen on the wire / decoded on the client.
struct SnapshotEntity
{
    uint32_t                netId{ 0 };
    EntityKind              kind{ EntityKind::Unknown };
    Neuron::World::WorldPos pos{};
    DirectX::XMFLOAT3       localOffset{ 0, 0, 0 };
    int32_t                 hp{ 0 };
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
        if (!rb.IsGood()) return false;
        out.entities.push_back(e);
    }
    return rb.IsGood();
}

} // namespace Neuron::Sim
