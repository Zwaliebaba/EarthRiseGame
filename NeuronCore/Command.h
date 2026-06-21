#pragma once
// Client → server command encoding (§8.4 — intents, not state).
//
// M1a carries a single MoveCommand: a desired base velocity. The server
// validates and clamps it (ServerUniverse::SetBaseVelocity → ClampSpeed) — clients
// never set authoritative state directly. Built on the serde primitives.

#include "Serde.h"

#include <cstdint>
#include <span>
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

} // namespace Neuron::Sim
