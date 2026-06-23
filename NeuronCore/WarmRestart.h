#pragma once
// WarmRestart.h — warm-restart state snapshot blob (M5 area F, §15, §9, §26).
//
// ERServer is stateless (§9): on a restart it recovers from a periodic **binary
// state snapshot (blob) + an event log since the snapshot** (§15), replaying the log
// onto the last snapshot for a clean, verifiable state — not a reconstruction from
// normalized rows. Warm-restart correctness is an uptime SLA (§26, R22).
//
// This header owns the **portable** half (mirrored on the Linux testrunner, §16.2):
//   * `PersistState` — a POD mirror of the persistable / transient sim state (bases,
//     ships, build queue, NPC), with versioned `Encode`/`Decode` over the §7.2 serde
//     primitives (the same `SimSnapshots` blob the ODBC layer stores), and
//   * `StateHash` — a structural hash over the fields (the `SimHash` analog) used to
//     verify pre-crash state == post-restart state.
// The since-snapshot economy log replay is `Neuron::Persist::Outbox` (Outbox.h). The
// ServerUniverse <-> PersistState capture/restore glue and the ODBC blob read/write
// are the Win32/SQL integration step (added once area A lands).

#include "Serde.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace Neuron::Persist
{

// --- persistable entities (columns the M3/M4 sim loop produces, §15) -------------
// Bases carry the §13.1 layered HP + disable-not-destroy state; M3's single Health
// maps onto hull (shield/armor seed to max until the M6 combat model fills them).
struct PersistBase
{
    uint32_t netId{ 0 };
    uint64_t ownerAccount{ 0 };
    int64_t  x{ 0 }, y{ 0 }, z{ 0 };
    int32_t  shieldHp{ 0 }, armorHp{ 0 }, hullHp{ 0 };
    float    storage[3]{ 0, 0, 0 }; // Ore / Ice / Gas
    float    fuel{ 0 };
    uint8_t  navPhase{ 0 };
    uint8_t  baseState{ 0 };        // 0 active · 1 retreating · 2 disabled (§13.1)
};

struct PersistShip
{
    uint32_t netId{ 0 };
    uint64_t ownerAccount{ 0 };
    int64_t  x{ 0 }, y{ 0 }, z{ 0 };
    int32_t  hp{ 0 };
    float    cargo[3]{ 0, 0, 0 };
    uint8_t  shipType{ 0 };
};

struct PersistBuild
{
    uint64_t ownerAccount{ 0 };
    uint32_t itemDefId{ 0 };
    float    progress{ 0 };
};

// Transient sim state lives ONLY in the blob (never normalized, §15).
struct PersistNpc
{
    uint32_t netId{ 0 };
    int64_t  x{ 0 }, y{ 0 }, z{ 0 };
    int32_t  hp{ 0 };
    uint16_t siteId{ 0 };
    uint8_t  aiState{ 0 };
};

// The full warm-restart snapshot of the authoritative sim.
struct PersistState
{
    uint64_t                  tick{ 0 };       // sim tick the snapshot was taken at
    uint64_t                  outboxSeq{ 0 };  // economy log watermark at snapshot time
    std::vector<PersistBase>  bases;
    std::vector<PersistShip>  ships;
    std::vector<PersistBuild> builds;
    std::vector<PersistNpc>   npcs;
};

// --- structural hash (the SimHash analog, §17 M5) --------------------------------
namespace detail
{
    inline void HashU64(uint64_t& h, uint64_t v) noexcept
    {
        // FNV-1a-style mix over 8 bytes.
        for (int i = 0; i < 8; ++i) {
            h ^= static_cast<uint8_t>(v >> (i * 8));
            h *= 0x100000001B3ull;
        }
    }
    inline void HashF32(uint64_t& h, float f) noexcept
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        HashU64(h, bits);
    }
}

[[nodiscard]] inline uint64_t StateHash(const PersistState& s) noexcept
{
    uint64_t h = 0xCBF29CE484222325ull;
    detail::HashU64(h, s.tick);
    detail::HashU64(h, s.outboxSeq);
    detail::HashU64(h, s.bases.size());
    for (const auto& b : s.bases) {
        detail::HashU64(h, b.netId);
        detail::HashU64(h, b.ownerAccount);
        detail::HashU64(h, static_cast<uint64_t>(b.x));
        detail::HashU64(h, static_cast<uint64_t>(b.y));
        detail::HashU64(h, static_cast<uint64_t>(b.z));
        detail::HashU64(h, static_cast<uint64_t>(static_cast<uint32_t>(b.shieldHp)));
        detail::HashU64(h, static_cast<uint64_t>(static_cast<uint32_t>(b.armorHp)));
        detail::HashU64(h, static_cast<uint64_t>(static_cast<uint32_t>(b.hullHp)));
        for (float v : b.storage) detail::HashF32(h, v);
        detail::HashF32(h, b.fuel);
        detail::HashU64(h, b.navPhase);
        detail::HashU64(h, b.baseState);
    }
    detail::HashU64(h, s.ships.size());
    for (const auto& sh : s.ships) {
        detail::HashU64(h, sh.netId);
        detail::HashU64(h, sh.ownerAccount);
        detail::HashU64(h, static_cast<uint64_t>(sh.x));
        detail::HashU64(h, static_cast<uint64_t>(sh.y));
        detail::HashU64(h, static_cast<uint64_t>(sh.z));
        detail::HashU64(h, static_cast<uint64_t>(static_cast<uint32_t>(sh.hp)));
        for (float v : sh.cargo) detail::HashF32(h, v);
        detail::HashU64(h, sh.shipType);
    }
    detail::HashU64(h, s.builds.size());
    for (const auto& bd : s.builds) {
        detail::HashU64(h, bd.ownerAccount);
        detail::HashU64(h, bd.itemDefId);
        detail::HashF32(h, bd.progress);
    }
    detail::HashU64(h, s.npcs.size());
    for (const auto& n : s.npcs) {
        detail::HashU64(h, n.netId);
        detail::HashU64(h, static_cast<uint64_t>(n.x));
        detail::HashU64(h, static_cast<uint64_t>(n.y));
        detail::HashU64(h, static_cast<uint64_t>(n.z));
        detail::HashU64(h, static_cast<uint64_t>(static_cast<uint32_t>(n.hp)));
        detail::HashU64(h, n.siteId);
        detail::HashU64(h, n.aiState);
    }
    return h;
}

// --- versioned blob codec (the SimSnapshots payload, §7.2) -----------------------
[[nodiscard]] inline std::vector<uint8_t> EncodeState(const PersistState& s)
{
    Neuron::Serde::WriteBuffer w(1024);
    w.WriteUint64(s.tick);
    w.WriteUint64(s.outboxSeq);

    w.WriteUint32(static_cast<uint32_t>(s.bases.size()));
    for (const auto& b : s.bases) {
        w.WriteUint32(b.netId);
        w.WriteUint64(b.ownerAccount);
        w.WriteInt64(b.x); w.WriteInt64(b.y); w.WriteInt64(b.z);
        w.WriteUint32(static_cast<uint32_t>(b.shieldHp));
        w.WriteUint32(static_cast<uint32_t>(b.armorHp));
        w.WriteUint32(static_cast<uint32_t>(b.hullHp));
        for (float v : b.storage) w.WriteFloat(v);
        w.WriteFloat(b.fuel);
        w.WriteUint8(b.navPhase);
        w.WriteUint8(b.baseState);
    }

    w.WriteUint32(static_cast<uint32_t>(s.ships.size()));
    for (const auto& sh : s.ships) {
        w.WriteUint32(sh.netId);
        w.WriteUint64(sh.ownerAccount);
        w.WriteInt64(sh.x); w.WriteInt64(sh.y); w.WriteInt64(sh.z);
        w.WriteUint32(static_cast<uint32_t>(sh.hp));
        for (float v : sh.cargo) w.WriteFloat(v);
        w.WriteUint8(sh.shipType);
    }

    w.WriteUint32(static_cast<uint32_t>(s.builds.size()));
    for (const auto& bd : s.builds) {
        w.WriteUint64(bd.ownerAccount);
        w.WriteUint32(bd.itemDefId);
        w.WriteFloat(bd.progress);
    }

    w.WriteUint32(static_cast<uint32_t>(s.npcs.size()));
    for (const auto& n : s.npcs) {
        w.WriteUint32(n.netId);
        w.WriteInt64(n.x); w.WriteInt64(n.y); w.WriteInt64(n.z);
        w.WriteUint32(static_cast<uint32_t>(n.hp));
        w.WriteUint16(n.siteId);
        w.WriteUint8(n.aiState);
    }

    w.Finalise();
    const auto sp = w.Data();
    return { sp.begin(), sp.end() };
}

// Decode a blob into 'out'. Returns false on a truncated/version-mismatched blob.
[[nodiscard]] inline bool DecodeState(std::span<const uint8_t> blob, PersistState& out)
{
    Neuron::Serde::ReadBuffer r(blob);
    if (!r.IsGood()) return false;
    out = PersistState{};
    out.tick      = r.ReadUint64();
    out.outboxSeq = r.ReadUint64();

    const uint32_t nBases = r.ReadUint32();
    out.bases.reserve(nBases);
    for (uint32_t i = 0; i < nBases; ++i) {
        PersistBase b;
        b.netId        = r.ReadUint32();
        b.ownerAccount = r.ReadUint64();
        b.x = r.ReadInt64(); b.y = r.ReadInt64(); b.z = r.ReadInt64();
        b.shieldHp = static_cast<int32_t>(r.ReadUint32());
        b.armorHp  = static_cast<int32_t>(r.ReadUint32());
        b.hullHp   = static_cast<int32_t>(r.ReadUint32());
        for (float& v : b.storage) v = r.ReadFloat();
        b.fuel      = r.ReadFloat();
        b.navPhase  = r.ReadUint8();
        b.baseState = r.ReadUint8();
        out.bases.push_back(b);
    }

    const uint32_t nShips = r.ReadUint32();
    out.ships.reserve(nShips);
    for (uint32_t i = 0; i < nShips; ++i) {
        PersistShip sh;
        sh.netId        = r.ReadUint32();
        sh.ownerAccount = r.ReadUint64();
        sh.x = r.ReadInt64(); sh.y = r.ReadInt64(); sh.z = r.ReadInt64();
        sh.hp = static_cast<int32_t>(r.ReadUint32());
        for (float& v : sh.cargo) v = r.ReadFloat();
        sh.shipType = r.ReadUint8();
        out.ships.push_back(sh);
    }

    const uint32_t nBuilds = r.ReadUint32();
    out.builds.reserve(nBuilds);
    for (uint32_t i = 0; i < nBuilds; ++i) {
        PersistBuild bd;
        bd.ownerAccount = r.ReadUint64();
        bd.itemDefId    = r.ReadUint32();
        bd.progress     = r.ReadFloat();
        out.builds.push_back(bd);
    }

    const uint32_t nNpcs = r.ReadUint32();
    out.npcs.reserve(nNpcs);
    for (uint32_t i = 0; i < nNpcs; ++i) {
        PersistNpc n;
        n.netId = r.ReadUint32();
        n.x = r.ReadInt64(); n.y = r.ReadInt64(); n.z = r.ReadInt64();
        n.hp     = static_cast<int32_t>(r.ReadUint32());
        n.siteId = r.ReadUint16();
        n.aiState = r.ReadUint8();
        out.npcs.push_back(n);
    }

    return r.IsGood();
}

} // namespace Neuron::Persist
