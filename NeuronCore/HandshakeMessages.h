#pragma once
// Handshake message bodies — §8.5 connection sequence.
//
// These are the payloads of the pre-/peri-handshake MsgTypes. Steps 1–4 are sent
// in clear (the handshake bootstraps the encrypted channel); step 5 (login) and
// onward go through the SecureChannel. Encoding uses the little-endian helpers in
// PacketCodec.h. Platform-independent.

#include "ICrypto.h"
#include "PacketCodec.h"
#include "Protocol.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Neuron::Net
{

// --- Step 1/2: ClientHello (client → server). Carries the protocol id so the
// server can apply the version gate before allocating any state. ---
struct ClientHelloBody
{
    uint32_t protocolId{ PROTOCOL_ID };

    void Encode(std::vector<uint8_t>& out) const { PutU32(out, protocolId); }
    [[nodiscard]] static bool Decode(std::span<const uint8_t> in, ClientHelloBody& b)
    {
        if (in.size() < 4) return false;
        size_t off = 0; b.protocolId = GetU32(in, off); return true;
    }
};

// --- Step 1: Cookie (server → client). Stateless HMAC cookie. ---
struct CookieBody
{
    Cookie cookie{};
    void Encode(std::vector<uint8_t>& out) const { out.insert(out.end(), cookie.begin(), cookie.end()); }
    [[nodiscard]] static bool Decode(std::span<const uint8_t> in, CookieBody& b)
    {
        if (in.size() < COOKIE_BYTES) return false;
        std::copy_n(in.begin(), COOKIE_BYTES, b.cookie.begin());
        return true;
    }
};

// --- Step 1/3: CookieResponse (client → server). Echoes the cookie and presents
// the client's ephemeral ECDH public key so the server can respond in one round. ---
struct CookieResponseBody
{
    Cookie    cookie{};
    EcPubKey  clientEphemeralPub{};

    void Encode(std::vector<uint8_t>& out) const
    {
        out.insert(out.end(), cookie.begin(), cookie.end());
        out.insert(out.end(), clientEphemeralPub.begin(), clientEphemeralPub.end());
    }
    [[nodiscard]] static bool Decode(std::span<const uint8_t> in, CookieResponseBody& b)
    {
        if (in.size() < COOKIE_BYTES + EC_PUB_KEY_BYTES) return false;
        std::copy_n(in.begin(), COOKIE_BYTES, b.cookie.begin());
        std::copy_n(in.begin() + COOKIE_BYTES, EC_PUB_KEY_BYTES, b.clientEphemeralPub.begin());
        return true;
    }
};

// --- Step 3: HandshakeResponse (server → client). Server ephemeral ECDH pubkey,
// signed by the pinned static ECDSA key, plus the assigned connection token and
// AEAD epoch. The client verifies the signature (MITM resistance, §8.3). ---
struct HandshakeResponseBody
{
    EcPubKey    serverEphemeralPub{};
    EcSignature signature{};        // ECDSA over serverEphemeralPub
    uint64_t    connectionToken{ 0 };
    uint32_t    epoch{ 0 };

    void Encode(std::vector<uint8_t>& out) const
    {
        out.insert(out.end(), serverEphemeralPub.begin(), serverEphemeralPub.end());
        out.insert(out.end(), signature.begin(), signature.end());
        PutU64(out, connectionToken);
        PutU32(out, epoch);
    }
    [[nodiscard]] static bool Decode(std::span<const uint8_t> in, HandshakeResponseBody& b)
    {
        if (in.size() < EC_PUB_KEY_BYTES + EC_SIG_BYTES + 8 + 4) return false;
        size_t off = 0;
        std::copy_n(in.begin() + off, EC_PUB_KEY_BYTES, b.serverEphemeralPub.begin()); off += EC_PUB_KEY_BYTES;
        std::copy_n(in.begin() + off, EC_SIG_BYTES, b.signature.begin());             off += EC_SIG_BYTES;
        b.connectionToken = GetU64(in, off);
        b.epoch           = GetU32(in, off);
        return true;
    }
};

// --- Step 4: ClockSync (both directions). RTT/offset estimation. ---
// M4 area H (§7.2/§8.5): the response carries the server's current time-dilation
// factor so the client interpolates against the *dilated authoritative clock*, not
// wall-clock — under overload the server slows in-game time toward a floor, and the
// client must track that or it over-extrapolates. Sent as fixed-point parts-per-
// million (factor ∈ [floor, 1] → [100000, 1000000]) to stay integer on the wire.
// Backward-aware: an old 16-byte body (no field) decodes as factor 1.0 (full speed).
inline constexpr uint32_t DILATION_FIXED_ONE = 1'000'000; // 1.0 in ppm fixed-point

struct ClockSyncBody
{
    uint64_t clientTimeMicros{ 0 };   // echoed back
    uint64_t serverTimeMicros{ 0 };   // filled by server in the response
    double   dilationFactor{ 1.0 };   // server time-dilation (§7.2); 1.0 = full speed

    void Encode(std::vector<uint8_t>& out) const
    {
        PutU64(out, clientTimeMicros);
        PutU64(out, serverTimeMicros);
        // Clamp to [floor=0, 1]·1e6 and round to ppm; the controller never exceeds 1.
        double f = dilationFactor < 0.0 ? 0.0 : (dilationFactor > 1.0 ? 1.0 : dilationFactor);
        PutU32(out, static_cast<uint32_t>(f * static_cast<double>(DILATION_FIXED_ONE) + 0.5));
    }
    [[nodiscard]] static bool Decode(std::span<const uint8_t> in, ClockSyncBody& b)
    {
        if (in.size() < 16) return false;
        size_t off = 0;
        b.clientTimeMicros = GetU64(in, off);
        b.serverTimeMicros = GetU64(in, off);
        // Versioned/forward-compatible: the dilation field is optional. A peer that
        // predates it (16-byte body) is treated as full speed.
        b.dilationFactor = (in.size() >= off + 4)
            ? static_cast<double>(GetU32(in, off)) / static_cast<double>(DILATION_FIXED_ONE)
            : 1.0;
        return true;
    }
};

// --- Step 5: Login (over the encrypted channel). M1a dev mode: name only;
// real PBKDF2 credential check lands in M5 (§14). ---
struct LoginRequestBody
{
    std::string username; // dev stub; password added in M5

    void Encode(std::vector<uint8_t>& out) const
    {
        PutU16(out, static_cast<uint16_t>(username.size()));
        out.insert(out.end(), username.begin(), username.end());
    }
    [[nodiscard]] static bool Decode(std::span<const uint8_t> in, LoginRequestBody& b)
    {
        if (in.size() < 2) return false;
        size_t off = 0;
        const uint16_t len = GetU16(in, off);
        if (off + len > in.size()) return false;
        b.username.assign(reinterpret_cast<const char*>(in.data() + off), len);
        return true;
    }
};

struct LoginResponseBody
{
    uint8_t  success{ 0 };          // 1 = ok, 0 = failure (reason in 'reason')
    uint8_t  reason{ 0 };           // DisconnectReason on failure
    uint64_t sessionToken{ 0 };     // valid when success
    uint32_t playerNetworkId{ 0 };  // server entity id of the player's base

    void Encode(std::vector<uint8_t>& out) const
    {
        out.push_back(success);
        out.push_back(reason);
        PutU64(out, sessionToken);
        PutU32(out, playerNetworkId);
    }
    [[nodiscard]] static bool Decode(std::span<const uint8_t> in, LoginResponseBody& b)
    {
        if (in.size() < 1 + 1 + 8 + 4) return false;
        size_t off = 0;
        b.success         = in[off++];
        b.reason          = in[off++];
        b.sessionToken    = GetU64(in, off);
        b.playerNetworkId = GetU32(in, off);
        return true;
    }
};

} // namespace Neuron::Net
