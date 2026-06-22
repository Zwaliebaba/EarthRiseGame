#pragma once
// Client → server command encoding (§8.4 — intents, not state).
//
// M1a carries a single MoveCommand: a desired base velocity. The server
// validates and clamps it (ServerUniverse::SetBaseVelocity → ClampSpeed) — clients
// never set authoritative state directly. Built on the serde primitives.
//
// M3 (area B) adds FleetCommand: the full RTS intent set (§23.4) addressed to a
// set of owned units. Encoding is versioned (a leading version byte) so the wire
// contract can grow. The server validates ownership + target + clamps before
// applying anything (ServerUniverse::ApplyFleetCommand); invalid intents are
// rejected, never applied (§8.4 — never client-authoritative).

#include "Components.h" // IntentType ↔ OrderType share the RTS verb set
#include "Serde.h"
#include "UniversePos.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Neuron::Sim
{

struct MoveCommand
{
    uint32_t clientTick{ 0 };
    float    velX{ 0 }, velY{ 0 }, velZ{ 0 }; // desired m/s (server clamps)
};

inline std::vector<uint8_t> EncodeMoveCommand(const MoveCommand& c)
{
    Neuron::Serde::WriteBuffer wb(32);
    wb.WriteUint32(c.clientTick);
    wb.WriteFloat(c.velX);
    wb.WriteFloat(c.velY);
    wb.WriteFloat(c.velZ);
    wb.Finalise();
    auto d = wb.Data();
    return { d.begin(), d.end() };
}

[[nodiscard]] inline bool DecodeMoveCommand(std::span<const uint8_t> body, MoveCommand& out)
{
    Neuron::Serde::ReadBuffer rb(body);
    if (!rb.IsGood()) return false;
    out.clientTick = rb.ReadUint32();
    out.velX = rb.ReadFloat();
    out.velY = rb.ReadFloat();
    out.velZ = rb.ReadFloat();
    return rb.IsGood();
}

// --- fleet command — the full RTS intent set (§23.4) ------------------------

// The intent verbs. Stop/Move/Attack/Guard/Orbit/KeepRange/Retreat map 1:1 onto
// OrderType (one machine drives both player + NPC units); Harvest/Warp/Jump are
// one-shot intents routed to their own systems. Stable wire values.
enum class IntentType : uint8_t
{
    Stop = 0, Move = 1, Attack = 2, Guard = 3, Orbit = 4, KeepRange = 5,
    Harvest = 6, Warp = 7, Jump = 8, Retreat = 9, Build = 10
};

// One client → server fleet order. 'units' lists the owned entity net ids to
// command (server checks ownership of each). The target is interpreted by
// 'intent': entity (Attack/Guard/Orbit/KeepRange/Harvest), point (Move/Warp),
// or beacon name (Jump). 'queue' shift-chains onto the unit's order queue
// instead of replacing it (§23.4). Stop/Retreat carry no target.
struct FleetCommand
{
    uint32_t                      clientTick{ 0 };
    IntentType                    intent{ IntentType::Stop };
    bool                          queue{ false };
    std::vector<uint32_t>         units;            // owned entity net ids
    uint32_t                      targetNetId{ 0 }; // entity target
    Neuron::Universe::UniversePos targetPoint{};    // point target
    float                         range{ 0.0f };    // orbit / keep-range distance
    std::string                   beacon;           // jump destination beacon name
};

inline constexpr uint8_t kFleetCommandVersion = 1;

inline std::vector<uint8_t> EncodeFleetCommand(const FleetCommand& c)
{
    Neuron::Serde::WriteBuffer wb(64 + c.units.size() * 4 + c.beacon.size());
    wb.WriteUint8(kFleetCommandVersion);
    wb.WriteUint32(c.clientTick);
    wb.WriteUint8(static_cast<uint8_t>(c.intent));
    wb.WriteUint8(c.queue ? 1u : 0u);
    wb.WriteUint16(static_cast<uint16_t>(c.units.size()));
    for (uint32_t u : c.units) wb.WriteUint32(u);
    wb.WriteUint32(c.targetNetId);
    wb.WriteInt64(c.targetPoint.x);
    wb.WriteInt64(c.targetPoint.y);
    wb.WriteInt64(c.targetPoint.z);
    wb.WriteFloat(c.range);
    wb.WriteUint16(static_cast<uint16_t>(c.beacon.size()));
    if (!c.beacon.empty()) wb.WriteBytes(c.beacon.data(), c.beacon.size());
    wb.Finalise();
    auto d = wb.Data();
    return { d.begin(), d.end() };
}

[[nodiscard]] inline bool DecodeFleetCommand(std::span<const uint8_t> body, FleetCommand& out)
{
    Neuron::Serde::ReadBuffer rb(body);
    if (!rb.IsGood()) return false;
    if (rb.ReadUint8() != kFleetCommandVersion) return false; // protocol-version gate (§8.5)
    out.clientTick = rb.ReadUint32();
    out.intent     = static_cast<IntentType>(rb.ReadUint8());
    out.queue      = rb.ReadUint8() != 0;
    const uint16_t n = rb.ReadUint16();
    out.units.clear();
    out.units.reserve(n);
    for (uint16_t i = 0; i < n; ++i) out.units.push_back(rb.ReadUint32());
    out.targetNetId  = rb.ReadUint32();
    out.targetPoint.x = rb.ReadInt64();
    out.targetPoint.y = rb.ReadInt64();
    out.targetPoint.z = rb.ReadInt64();
    out.range = rb.ReadFloat();
    const uint16_t blen = rb.ReadUint16();
    out.beacon.assign(blen, '\0');
    if (blen) rb.ReadBytes(out.beacon.data(), blen);
    return rb.IsGood();
}

} // namespace Neuron::Sim
