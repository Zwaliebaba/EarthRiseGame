#pragma once
// Handshake state machines — §8.5 connection sequence.
//
//   1. Stateless cookie   (handshake-DoS guard; no server state until returned)
//   2. Protocol-version gate
//   3. CNG ECDH + server static-key ECDSA signature (MITM resistance)
//   4. Clock sync (RTT/offset)
//   → hands the derived per-direction AEAD keys + connection token to the
//     SecureChannel; login (step 5) and universe sync (step 6) run on the higher
//     Connection layer over the now-encrypted channel.
//
// Platform-independent: depends only on ICrypto, so it is unit-tested with
// FakeCrypto and an in-memory transport (R10/R6 handshake + MITM tests).

#include "HandshakeMessages.h"
#include "ICrypto.h"
#include "Protocol.h"

#include <cstdint>
#include <vector>

namespace Neuron::Net
{

// A single produced message (or a failure verdict).
struct HsOutput
{
    bool                 send{ false };
    MsgType              type{};
    std::vector<uint8_t> body;

    bool                 failed{ false };
    DisconnectReason     reason{ DisconnectReason::Normal };

    static HsOutput Message(MsgType t, std::vector<uint8_t> b)
    {
        return { true, t, std::move(b), false, DisconnectReason::Normal };
    }
    static HsOutput Fail(DisconnectReason r)
    {
        return { false, {}, {}, true, r };
    }
    static HsOutput None() { return {}; }
};

// ---------------------------------------------------------------------------
// Stateless server cookie phase (steps 1–2). No per-connection allocation.
// ---------------------------------------------------------------------------
struct HandshakeServerStateless
{
    ICrypto*             crypto{ nullptr };
    std::vector<uint8_t> serverSecret; // process-wide secret for cookie HMAC

    // Respond to a ClientHello: version gate, then issue a stateless cookie.
    [[nodiscard]] HsOutput OnClientHello(std::span<const uint8_t> body,
                                         std::span<const uint8_t> clientAddr) const
    {
        ClientHelloBody hello;
        if (!ClientHelloBody::Decode(body, hello))
            return HsOutput::Fail(DisconnectReason::ProtocolMismatch);
        if (!IsProtocolCompatible(hello.protocolId))
            return HsOutput::Message(MsgType::VersionReject,
                                     { static_cast<uint8_t>(DisconnectReason::ProtocolMismatch) });

        CookieBody cb;
        cb.cookie = crypto->MakeCookie(serverSecret, clientAddr);
        std::vector<uint8_t> out;
        cb.Encode(out);
        return HsOutput::Message(MsgType::Cookie, std::move(out));
    }

    // Verify a returned cookie before allocating any connection state.
    [[nodiscard]] bool VerifyCookie(const Cookie& cookie,
                                    std::span<const uint8_t> clientAddr) const
    {
        return crypto->VerifyCookie(serverSecret, clientAddr, cookie);
    }
};

// ---------------------------------------------------------------------------
// Per-connection server side (allocated only after a valid cookie returns).
// ---------------------------------------------------------------------------
class HandshakeServerConnection
{
public:
    HandshakeServerConnection(ICrypto* crypto,
                              std::span<const uint8_t> staticPrivBlob,
                              uint64_t connectionToken,
                              uint64_t serverTimeMicros) noexcept
        : m_crypto(crypto)
        , m_staticPriv(staticPrivBlob.begin(), staticPrivBlob.end())
        , m_token(connectionToken)
        , m_serverTime(serverTimeMicros)
    {
    }

    // Step 3: consume CookieResponse (cookie already verified by the stateless
    // layer), run ECDH, sign our ephemeral pubkey, derive AEAD keys.
    [[nodiscard]] HsOutput OnCookieResponse(std::span<const uint8_t> body)
    {
        CookieResponseBody cr;
        if (!CookieResponseBody::Decode(body, cr))
            return HsOutput::Fail(DisconnectReason::HandshakeFailure);

        m_serverEphemeral = m_crypto->GenerateEcdhKeyPair();

        SharedSecret secret;
        if (!m_crypto->DeriveSharedSecret(m_serverEphemeral, cr.clientEphemeralPub, secret))
            return HsOutput::Fail(DisconnectReason::HandshakeFailure);

        // Server signs its ephemeral public key with the pinned static key.
        EcSignature sig;
        if (!m_crypto->Sign(m_staticPriv, m_serverEphemeral.publicKey, sig))
            return HsOutput::Fail(DisconnectReason::HandshakeFailure);

        if (!m_crypto->DeriveAeadKeys(secret, m_epoch, m_keyC2S, m_keyS2C))
            return HsOutput::Fail(DisconnectReason::HandshakeFailure);
        m_haveKeys = true;

        HandshakeResponseBody resp;
        resp.serverEphemeralPub = m_serverEphemeral.publicKey;
        resp.signature          = sig;
        resp.connectionToken    = m_token;
        resp.epoch              = m_epoch;
        std::vector<uint8_t> out;
        resp.Encode(out);
        return HsOutput::Message(MsgType::HandshakeResponse, std::move(out));
    }

    // Step 4: echo the client's timestamp and stamp our server time + current
    // time-dilation factor (M4 area H, §8.5) so the client interpolates against the
    // dilated authoritative clock. The host sets the factor each loop via the
    // connection (SetDilationFactor); clock-sync is a one-shot at connect, so a
    // mid-session dilation change is also republished on the periodic clock re-sync.
    [[nodiscard]] HsOutput OnClockSyncRequest(std::span<const uint8_t> body)
    {
        ClockSyncBody cs;
        if (!ClockSyncBody::Decode(body, cs))
            return HsOutput::Fail(DisconnectReason::HandshakeFailure);
        cs.serverTimeMicros = m_serverTime;
        cs.dilationFactor   = m_dilationFactor;
        std::vector<uint8_t> out;
        cs.Encode(out);
        m_complete = true; // crypto established; login proceeds on the secure channel
        return HsOutput::Message(MsgType::ClockSyncResponse, std::move(out));
    }

    // Set the dilation factor echoed in the clock-sync response (§7.2). Fed from the
    // server loop's FixedStepAccumulator::DilationFactor() each tick.
    void SetDilationFactor(double f) noexcept { m_dilationFactor = f; }

    [[nodiscard]] bool IsComplete()  const noexcept { return m_complete; }
    [[nodiscard]] bool HasKeys()     const noexcept { return m_haveKeys; }
    [[nodiscard]] const AeadKey& KeyC2S() const noexcept { return m_keyC2S; }
    [[nodiscard]] const AeadKey& KeyS2C() const noexcept { return m_keyS2C; }
    [[nodiscard]] uint64_t Token()   const noexcept { return m_token; }
    [[nodiscard]] uint32_t Epoch()   const noexcept { return m_epoch; }

private:
    ICrypto*             m_crypto{ nullptr };
    std::vector<uint8_t> m_staticPriv;
    uint64_t             m_token{ 0 };
    uint64_t             m_serverTime{ 0 };
    double               m_dilationFactor{ 1.0 }; // M4 area H — published in clock echo
    uint32_t             m_epoch{ 0 };

    EcdhKeyPair          m_serverEphemeral;
    AeadKey              m_keyC2S{};
    AeadKey              m_keyS2C{};
    bool                 m_haveKeys{ false };
    bool                 m_complete{ false };
};

// ---------------------------------------------------------------------------
// Client side (stateful from the first message).
// ---------------------------------------------------------------------------
class HandshakeClient
{
public:
    HandshakeClient(ICrypto* crypto, const EcPubKey& pinnedServerStaticPub,
                    uint64_t clientTimeMicros) noexcept
        : m_crypto(crypto)
        , m_pinnedStaticPub(pinnedServerStaticPub)
        , m_clientTime(clientTimeMicros)
    {
    }

    // Step 1: produce the ClientHello.
    [[nodiscard]] HsOutput Begin()
    {
        ClientHelloBody hello;
        hello.protocolId = kProtocolId;
        std::vector<uint8_t> out;
        hello.Encode(out);
        return HsOutput::Message(MsgType::ClientHello, std::move(out));
    }

    // Dispatch an incoming handshake message.
    [[nodiscard]] HsOutput OnMessage(MsgType type, std::span<const uint8_t> body)
    {
        switch (type) {
            case MsgType::VersionReject:
                return HsOutput::Fail(DisconnectReason::ProtocolMismatch);
            case MsgType::Cookie:            return OnCookie(body);
            case MsgType::HandshakeResponse: return OnHandshakeResponse(body);
            case MsgType::ClockSyncResponse: return OnClockSyncResponse(body);
            default:                         return HsOutput::None();
        }
    }

    [[nodiscard]] bool IsComplete()  const noexcept { return m_complete; }
    [[nodiscard]] bool HasKeys()     const noexcept { return m_haveKeys; }
    [[nodiscard]] const AeadKey& KeyC2S() const noexcept { return m_keyC2S; }
    [[nodiscard]] const AeadKey& KeyS2C() const noexcept { return m_keyS2C; }
    [[nodiscard]] uint64_t Token()   const noexcept { return m_token; }
    [[nodiscard]] uint32_t Epoch()   const noexcept { return m_epoch; }
    // Round-trip estimate from clock sync (micros); valid once complete.
    [[nodiscard]] uint64_t RttMicros() const noexcept { return m_rttMicros; }
    [[nodiscard]] int64_t  ClockOffsetMicros() const noexcept { return m_clockOffsetMicros; }
    // Server time-dilation factor from the last clock-sync response (M4 area H,
    // §7.2/§8.5); 1.0 = full speed. The interpolator scales server-time advance by
    // this so it tracks the dilated authoritative clock, not wall-clock.
    [[nodiscard]] double   DilationFactor() const noexcept { return m_dilationFactor; }

private:
    // Step 1→3: got the cookie; make an ephemeral keypair and return it + cookie.
    // Idempotent: a duplicated/reordered Cookie must NOT regenerate the ephemeral
    // key, or the derived keys would diverge from the server's (which committed to
    // the first CookieResponse). Re-echo the original CookieResponse instead.
    [[nodiscard]] HsOutput OnCookie(std::span<const uint8_t> body)
    {
        CookieBody cb;
        if (!CookieBody::Decode(body, cb))
            return HsOutput::Fail(DisconnectReason::BadCookie);

        if (m_cookieHandled) {
            CookieResponseBody cr;
            cr.cookie             = cb.cookie;
            cr.clientEphemeralPub = m_clientEphemeral.publicKey;
            std::vector<uint8_t> out;
            cr.Encode(out);
            return HsOutput::Message(MsgType::CookieResponse, std::move(out));
        }

        m_cookieHandled  = true;
        m_clientEphemeral = m_crypto->GenerateEcdhKeyPair();

        CookieResponseBody cr;
        cr.cookie             = cb.cookie;
        cr.clientEphemeralPub = m_clientEphemeral.publicKey;
        std::vector<uint8_t> out;
        cr.Encode(out);
        return HsOutput::Message(MsgType::CookieResponse, std::move(out));
    }

    // Step 3: verify the server signature (MITM check) and derive keys.
    // Idempotent against a duplicated HandshakeResponse: once keys are derived,
    // re-emit the ClockSyncRequest rather than re-deriving.
    [[nodiscard]] HsOutput OnHandshakeResponse(std::span<const uint8_t> body)
    {
        if (m_haveKeys) {
            ClockSyncBody cs;
            cs.clientTimeMicros = m_clientTime;
            std::vector<uint8_t> out;
            cs.Encode(out);
            return HsOutput::Message(MsgType::ClockSyncRequest, std::move(out));
        }

        HandshakeResponseBody resp;
        if (!HandshakeResponseBody::Decode(body, resp))
            return HsOutput::Fail(DisconnectReason::HandshakeFailure);

        // Pinned static-key signature verification — rejects an active MITM that
        // tries to substitute its own ephemeral key in the login exchange.
        if (!m_crypto->Verify(m_pinnedStaticPub, resp.serverEphemeralPub, resp.signature))
            return HsOutput::Fail(DisconnectReason::HandshakeFailure);

        SharedSecret secret;
        if (!m_crypto->DeriveSharedSecret(m_clientEphemeral, resp.serverEphemeralPub, secret))
            return HsOutput::Fail(DisconnectReason::HandshakeFailure);

        m_token = resp.connectionToken;
        m_epoch = resp.epoch;
        if (!m_crypto->DeriveAeadKeys(secret, m_epoch, m_keyC2S, m_keyS2C))
            return HsOutput::Fail(DisconnectReason::HandshakeFailure);
        m_haveKeys = true;

        ClockSyncBody cs;
        cs.clientTimeMicros = m_clientTime;
        std::vector<uint8_t> out;
        cs.Encode(out);
        return HsOutput::Message(MsgType::ClockSyncRequest, std::move(out));
    }

    // Step 4: finish — estimate RTT/offset.
    [[nodiscard]] HsOutput OnClockSyncResponse(std::span<const uint8_t> body)
    {
        ClockSyncBody cs;
        if (!ClockSyncBody::Decode(body, cs))
            return HsOutput::Fail(DisconnectReason::HandshakeFailure);
        // RTT = now - echoed client time; offset ≈ serverTime - (clientTime + RTT/2).
        const uint64_t nowApprox = cs.clientTimeMicros + 1; // owner overwrites with real now
        m_rttMicros = nowApprox - cs.clientTimeMicros;
        m_clockOffsetMicros =
            static_cast<int64_t>(cs.serverTimeMicros) -
            static_cast<int64_t>(cs.clientTimeMicros + m_rttMicros / 2);
        m_dilationFactor = cs.dilationFactor; // M4 area H — track the dilated server clock
        m_complete = true;
        return HsOutput::None(); // ready to send LoginRequest on the secure channel
    }

    ICrypto*    m_crypto{ nullptr };
    EcPubKey    m_pinnedStaticPub{};
    uint64_t    m_clientTime{ 0 };

    EcdhKeyPair m_clientEphemeral;
    AeadKey     m_keyC2S{};
    AeadKey     m_keyS2C{};
    uint64_t    m_token{ 0 };
    uint32_t    m_epoch{ 0 };
    bool        m_haveKeys{ false };
    bool        m_complete{ false };
    bool        m_cookieHandled{ false };
    uint64_t    m_rttMicros{ 0 };
    int64_t     m_clockOffsetMicros{ 0 };
    double      m_dilationFactor{ 1.0 }; // M4 area H — last server dilation factor
};

} // namespace Neuron::Net
