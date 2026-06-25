#pragma once

// ServerStatus.h — the out-of-band server status/diagnostics record (§21) served
// over the optional SEPARATE diagnostic UDP port (distinct from the game port). A
// debug client (EarthRise built with _DEBUG) sends a fixed query token to the
// server's status port and the server replies with a single-datagram JSON document
// carrying the live counters the §21 telemetry already aggregates — connection
// counts, spawned-object count, sim/encode percentiles, dilation, cumulative network
// bytes — plus a few static config values (listen port, auth mode, persistence).
//
// This header is the platform-independent core: the wire encode/decode and the
// query-token validation, all pure and testrunner-verified (ServerStatusTests). The
// sockets live in the build-specific edges, ALL gated by _DEBUG so retail compiles
// none of them:
//   * ERServer/StatusEndpoint.h (Winsock)   — serves the status reply.
//   * EarthRise App.cpp (WinRT DatagramSocket) + NeuronClient/ServerStatusClient.h
//     — polls the port and renders the overlay.
//
// The header itself has no platform dependency and is harmless to include anywhere,
// but the feature is dark unless a debug server opens the port (off by default,
// statusPort 0) and a debug client queries it. The port is read-only — it answers
// only the exact query token and accepts no commands — and is meant for a trusted
// operator network, never an untrusted interface.

#include "Json.h"

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace Neuron::Net
{

// The status sub-protocol version (bumped if the JSON shape changes). Carried both
// in the query token and in the reply so a client/server mismatch is detectable.
inline constexpr uint8_t kStatusProtocolVersion = 1;

// The fixed request token a status client sends. The server validates it exactly
// before replying — a DoS guard so the port never answers arbitrary traffic. 8
// bytes: 'E','R','S','T','A','T','?' + the 1-byte status-protocol version.
inline constexpr char   kStatusQueryToken[8] = {
    'E', 'R', 'S', 'T', 'A', 'T', '?', static_cast<char>(kStatusProtocolVersion) };
inline constexpr size_t kStatusQueryTokenSize = sizeof(kStatusQueryToken);

// A status reply fits comfortably inside one safe-MTU datagram.
inline constexpr size_t kStatusMaxDatagramBytes = 1024;

// True if 'dg' is a well-formed status query (exact token match). The server
// endpoint uses this to decide whether to answer a received datagram.
[[nodiscard]] inline bool IsStatusQuery(std::span<const uint8_t> dg) noexcept
{
    if (dg.size() != kStatusQueryTokenSize) return false;
    for (size_t i = 0; i < kStatusQueryTokenSize; ++i)
        if (dg[i] != static_cast<uint8_t>(kStatusQueryToken[i])) return false;
    return true;
}

// The live + static fields the status UI shows. The live counters mirror
// ServerTelemetry / ServerHost; the config fields mirror ServerConfig. All scalar,
// so the JSON below is flat.
struct ServerStatus
{
    uint8_t protocolVersion{ kStatusProtocolVersion };

    // -- liveness ---------------------------------------------------------------
    uint64_t uptimeSeconds{ 0 };
    uint64_t simTick{ 0 };

    // -- connections (ServerHost) -----------------------------------------------
    uint32_t connectionsPending{ 0 }; // ConnectionCount() — handshaking + active
    uint32_t connectionsActive{ 0 };  // ConnectedCount()  — fully connected

    // -- objects (the authoritative ECS) ----------------------------------------
    uint64_t objectsSpawned{ 0 };     // World().EntityCount()
    uint64_t projectiles{ 0 };        // in-flight projectiles

    // -- sim / encode timing (ServerTelemetry, §21) -----------------------------
    double simP99Ms{ 0.0 };
    double encodeP99Ms{ 0.0 };
    double dilation{ 1.0 };

    // -- cumulative network (NetCounters, §21) ----------------------------------
    uint64_t downstreamBytes{ 0 };
    uint64_t upstreamBytes{ 0 };
    uint64_t datagramsIn{ 0 };
    uint64_t datagramsOut{ 0 };
    uint64_t baselineBytes{ 0 };      // per-client baseline RAM gauge (App. B)

    // -- static config (ServerConfig) -------------------------------------------
    uint16_t listenPort{ 0 };
    bool     devAuthStub{ false };
    bool     persistenceEnabled{ false };
};

// Serialize a status record to a flat JSON object. Numbers only (no string fields),
// so no escaping is needed; the doubles use a compact %.4g form the Json reader
// round-trips. The output fits in kStatusMaxDatagramBytes.
[[nodiscard]] inline std::string EncodeStatusJson(const ServerStatus& s)
{
    auto dbl = [](double d) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4g", d);
        return std::string(buf);
    };
    const char* trueFalse[2] = { "false", "true" };

    std::string j;
    j.reserve(512);
    j += '{';
    j += "\"protocolVersion\":" + std::to_string(static_cast<unsigned>(s.protocolVersion)) + ',';
    j += "\"uptimeSeconds\":"      + std::to_string(s.uptimeSeconds) + ',';
    j += "\"simTick\":"            + std::to_string(s.simTick) + ',';
    j += "\"connectionsPending\":" + std::to_string(s.connectionsPending) + ',';
    j += "\"connectionsActive\":"  + std::to_string(s.connectionsActive) + ',';
    j += "\"objectsSpawned\":"     + std::to_string(s.objectsSpawned) + ',';
    j += "\"projectiles\":"        + std::to_string(s.projectiles) + ',';
    j += "\"simP99Ms\":"           + dbl(s.simP99Ms) + ',';
    j += "\"encodeP99Ms\":"        + dbl(s.encodeP99Ms) + ',';
    j += "\"dilation\":"           + dbl(s.dilation) + ',';
    j += "\"downstreamBytes\":"    + std::to_string(s.downstreamBytes) + ',';
    j += "\"upstreamBytes\":"      + std::to_string(s.upstreamBytes) + ',';
    j += "\"datagramsIn\":"        + std::to_string(s.datagramsIn) + ',';
    j += "\"datagramsOut\":"       + std::to_string(s.datagramsOut) + ',';
    j += "\"baselineBytes\":"      + std::to_string(s.baselineBytes) + ',';
    j += "\"listenPort\":"         + std::to_string(static_cast<unsigned>(s.listenPort)) + ',';
    j += "\"devAuthStub\":"        + std::string(trueFalse[s.devAuthStub ? 1 : 0]) + ',';
    j += "\"persistenceEnabled\":" + std::string(trueFalse[s.persistenceEnabled ? 1 : 0]);
    j += '}';
    return j;
}

// Parse a status reply (the JSON produced by EncodeStatusJson). Returns false if the
// text is not a JSON object; missing keys fall back to the struct defaults, so a
// partial/older document still yields a usable record.
[[nodiscard]] inline bool ParseStatusJson(std::string_view text, ServerStatus& out)
{
    Neuron::Json::Value root;
    if (!Neuron::Json::Parse(text, root) || !root.isObject())
        return false;

    out.protocolVersion    = static_cast<uint8_t>(root.getUint32("protocolVersion", kStatusProtocolVersion));
    out.uptimeSeconds      = root.getUint64("uptimeSeconds", 0);
    out.simTick            = root.getUint64("simTick", 0);
    out.connectionsPending = root.getUint32("connectionsPending", 0);
    out.connectionsActive  = root.getUint32("connectionsActive", 0);
    out.objectsSpawned     = root.getUint64("objectsSpawned", 0);
    out.projectiles        = root.getUint64("projectiles", 0);
    out.simP99Ms           = root["simP99Ms"].asNumber(0.0);
    out.encodeP99Ms        = root["encodeP99Ms"].asNumber(0.0);
    out.dilation           = root["dilation"].asNumber(1.0);
    out.downstreamBytes    = root.getUint64("downstreamBytes", 0);
    out.upstreamBytes      = root.getUint64("upstreamBytes", 0);
    out.datagramsIn        = root.getUint64("datagramsIn", 0);
    out.datagramsOut       = root.getUint64("datagramsOut", 0);
    out.baselineBytes      = root.getUint64("baselineBytes", 0);
    out.listenPort         = static_cast<uint16_t>(root.getUint32("listenPort", 0));
    out.devAuthStub        = root.getBool("devAuthStub", false);
    out.persistenceEnabled = root.getBool("persistenceEnabled", false);
    return true;
}

} // namespace Neuron::Net
