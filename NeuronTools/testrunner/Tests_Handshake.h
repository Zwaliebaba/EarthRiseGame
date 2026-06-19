#pragma once
// M1a integration tests — full §8.5 connection sequence + SecureChannel.
//
// Drives the real HandshakeClient / HandshakeServer state machines and the
// SecureChannel over the deterministic FakeCrypto. Covers the security-critical
// acceptance criteria: matching key derivation, MITM rejection (R6), and AEAD
// replay rejection (R13).

#include "TestRunner.h"
#include "net/FakeCrypto.h"
#include "net/Handshake.h"
#include "net/SecureChannel.h"
#include "net/HandshakeMessages.h"
#include "net/Protocol.h"

#include <vector>

namespace
{

// Drive one full successful handshake; returns the established client & server
// SecureChannels via out-params. Returns true on success.
inline bool RunHandshake(Neuron::Net::FakeCrypto& crypto,
                         Neuron::Net::AeadKey& cKeyC2S, Neuron::Net::AeadKey& cKeyS2C,
                         Neuron::Net::AeadKey& sKeyC2S, Neuron::Net::AeadKey& sKeyS2C,
                         uint64_t& token)
{
    using namespace Neuron::Net;

    // Server static (pinned) key. FakeCrypto needs its matching static-key
    // helper (the real CngCrypto would use a genuine ECDSA keypair).
    std::vector<uint8_t> staticPriv;
    EcPubKey pinnedPub;
    FakeCrypto::MakeFakeStaticKey(staticPriv, pinnedPub);

    const std::vector<uint8_t> serverSecret = { 's','e','c','r','e','t' };
    const std::vector<uint8_t> clientAddr   = { 127, 0, 0, 1, 0x30, 0x39 }; // ip+port

    HandshakeServerStateless stateless{ &crypto, serverSecret };
    HandshakeClient client(&crypto, pinnedPub, /*clientTime*/ 1000);

    // Step 1: ClientHello → Cookie
    HsOutput hello = client.Begin();
    if (!hello.send) return false;
    HsOutput cookie = stateless.OnClientHello(hello.body, clientAddr);
    if (!cookie.send || cookie.type != MsgType::Cookie) return false;

    // Step 1→3: Cookie → CookieResponse
    HsOutput cookieResp = client.OnMessage(cookie.type, cookie.body);
    if (!cookieResp.send || cookieResp.type != MsgType::CookieResponse) return false;

    // Server verifies cookie statelessly, then allocates the connection.
    CookieResponseBody cr;
    if (!CookieResponseBody::Decode(cookieResp.body, cr)) return false;
    if (!stateless.VerifyCookie(cr.cookie, clientAddr)) return false;

    token = 0xABCDEF0123456789ull;
    HandshakeServerConnection server(&crypto, staticPriv, token, /*serverTime*/ 5000);

    // Step 3: CookieResponse → HandshakeResponse
    HsOutput hsResp = server.OnCookieResponse(cookieResp.body);
    if (!hsResp.send || hsResp.type != MsgType::HandshakeResponse) return false;

    // Client verifies signature + derives keys → ClockSyncRequest
    HsOutput clockReq = client.OnMessage(hsResp.type, hsResp.body);
    if (!clockReq.send || clockReq.type != MsgType::ClockSyncRequest) return false;

    // Step 4: ClockSyncRequest → ClockSyncResponse
    HsOutput clockResp = server.OnClockSyncRequest(clockReq.body);
    if (!clockResp.send || clockResp.type != MsgType::ClockSyncResponse) return false;
    HsOutput done = client.OnMessage(clockResp.type, clockResp.body);
    (void)done;

    if (!client.IsComplete() || !server.IsComplete()) return false;
    if (!client.HasKeys() || !server.HasKeys()) return false;

    cKeyC2S = client.KeyC2S(); cKeyS2C = client.KeyS2C();
    sKeyC2S = server.KeyC2S(); sKeyS2C = server.KeyS2C();
    return true;
}

} // anonymous

TEST_SUITE(Handshake)
{
    TEST_CASE(FullSequenceDerivesMatchingKeys) {
        Neuron::Net::FakeCrypto crypto;
        Neuron::Net::AeadKey cC2S, cS2C, sC2S, sS2C;
        uint64_t token = 0;
        REQUIRE(RunHandshake(crypto, cC2S, cS2C, sC2S, sS2C, token));
        // Both ends must derive identical per-direction keys.
        CHECK(cC2S == sC2S);
        CHECK(cS2C == sS2C);
        // The two directions must use different keys.
        CHECK(!(cC2S == cS2C));
    });

    TEST_CASE(SecureChannelRoundTrip) {
        Neuron::Net::FakeCrypto crypto;
        Neuron::Net::AeadKey cC2S, cS2C, sC2S, sS2C;
        uint64_t token = 0;
        REQUIRE(RunHandshake(crypto, cC2S, cS2C, sC2S, sS2C, token));

        Neuron::Net::SecureChannel clientCh(&crypto, Neuron::Net::Direction::ClientToServer, token);
        Neuron::Net::SecureChannel serverCh(&crypto, Neuron::Net::Direction::ServerToClient, token);
        clientCh.SetKeys(cC2S, cS2C, 0);
        serverCh.SetKeys(sS2C, sC2S, 0); // server sends S2C, receives C2S

        // Client → server
        const std::vector<uint8_t> plain = { 1, 2, 3, 4, 5 };
        std::vector<uint8_t> datagram;
        REQUIRE(clientCh.Seal(plain, datagram));

        std::vector<uint8_t> recovered;
        auto r = serverCh.Open(datagram, recovered);
        CHECK(r == Neuron::Net::SecureChannel::OpenResult::Ok);
        CHECK(recovered == plain);
    });

    TEST_CASE(ReplayRejected) {
        Neuron::Net::FakeCrypto crypto;
        Neuron::Net::AeadKey cC2S, cS2C, sC2S, sS2C;
        uint64_t token = 0;
        REQUIRE(RunHandshake(crypto, cC2S, cS2C, sC2S, sS2C, token));

        Neuron::Net::SecureChannel clientCh(&crypto, Neuron::Net::Direction::ClientToServer, token);
        Neuron::Net::SecureChannel serverCh(&crypto, Neuron::Net::Direction::ServerToClient, token);
        clientCh.SetKeys(cC2S, cS2C, 0);
        serverCh.SetKeys(sS2C, sC2S, 0);

        const std::vector<uint8_t> plain = { 9, 9, 9 };
        std::vector<uint8_t> datagram;
        REQUIRE(clientCh.Seal(plain, datagram));

        std::vector<uint8_t> out1, out2;
        CHECK(serverCh.Open(datagram, out1) == Neuron::Net::SecureChannel::OpenResult::Ok);
        // Re-deliver the identical datagram → replay window rejects it (R13).
        CHECK(serverCh.Open(datagram, out2) == Neuron::Net::SecureChannel::OpenResult::Replay);
    });

    TEST_CASE(TamperedCiphertextRejected) {
        Neuron::Net::FakeCrypto crypto;
        Neuron::Net::AeadKey cC2S, cS2C, sC2S, sS2C;
        uint64_t token = 0;
        REQUIRE(RunHandshake(crypto, cC2S, cS2C, sC2S, sS2C, token));

        Neuron::Net::SecureChannel clientCh(&crypto, Neuron::Net::Direction::ClientToServer, token);
        Neuron::Net::SecureChannel serverCh(&crypto, Neuron::Net::Direction::ServerToClient, token);
        clientCh.SetKeys(cC2S, cS2C, 0);
        serverCh.SetKeys(sS2C, sC2S, 0);

        const std::vector<uint8_t> plain = { 4, 5, 6 };
        std::vector<uint8_t> datagram;
        REQUIRE(clientCh.Seal(plain, datagram));
        datagram.back() ^= 0xFF; // flip a tag/ciphertext byte

        std::vector<uint8_t> out;
        CHECK(serverCh.Open(datagram, out) == Neuron::Net::SecureChannel::OpenResult::AuthFailure);
    });

    TEST_CASE(MitmSignatureRejected) {
        // An active MITM swaps the server's ephemeral key. Without the pinned
        // static signature it cannot forge a valid signature, so the client
        // rejects the HandshakeResponse (R6).
        using namespace Neuron::Net;
        FakeCrypto crypto;

        std::vector<uint8_t> staticPriv;
        EcPubKey pinnedPub;
        FakeCrypto::MakeFakeStaticKey(staticPriv, pinnedPub);
        const std::vector<uint8_t> serverSecret = { 'x' };
        const std::vector<uint8_t> clientAddr   = { 10, 0, 0, 1, 0, 1 };

        HandshakeServerStateless stateless{ &crypto, serverSecret };
        HandshakeClient client(&crypto, pinnedPub, 1000);

        HsOutput hello = client.Begin();
        HsOutput cookie = stateless.OnClientHello(hello.body, clientAddr);
        HsOutput cookieResp = client.OnMessage(cookie.type, cookie.body);

        // Legit server builds a response...
        HandshakeServerConnection server(&crypto, staticPriv, 1234, 5000);
        HsOutput hsResp = server.OnCookieResponse(cookieResp.body);

        // ...MITM tampers the signed ephemeral public key in transit.
        HandshakeResponseBody body;
        REQUIRE(HandshakeResponseBody::Decode(hsResp.body, body));
        body.serverEphemeralPub[0] ^= 0xFF; // tamper
        std::vector<uint8_t> tampered;
        body.Encode(tampered);

        HsOutput verdict = client.OnMessage(MsgType::HandshakeResponse, tampered);
        CHECK(verdict.failed);
        CHECK(verdict.reason == DisconnectReason::HandshakeFailure);
    });

    TEST_CASE(VersionMismatchRejected) {
        using namespace Neuron::Net;
        FakeCrypto crypto;
        const std::vector<uint8_t> serverSecret = { 'y' };
        const std::vector<uint8_t> clientAddr   = { 10, 0, 0, 2, 0, 2 };
        HandshakeServerStateless stateless{ &crypto, serverSecret };

        // Forge a ClientHello with a wrong protocol id.
        ClientHelloBody bad;
        bad.protocolId = 0xDEADBEEF;
        std::vector<uint8_t> body;
        bad.Encode(body);

        HsOutput out = stateless.OnClientHello(body, clientAddr);
        CHECK(out.send);
        CHECK(out.type == MsgType::VersionReject);
    });

    TEST_CASE(BadCookieRejected) {
        using namespace Neuron::Net;
        FakeCrypto crypto;
        const std::vector<uint8_t> serverSecret = { 'z' };
        const std::vector<uint8_t> clientAddr   = { 10, 0, 0, 3, 0, 3 };
        HandshakeServerStateless stateless{ &crypto, serverSecret };

        // A cookie the server never issued must not verify.
        Cookie forged{};
        forged[0] = 0x42;
        CHECK(!stateless.VerifyCookie(forged, clientAddr));
    });
}
