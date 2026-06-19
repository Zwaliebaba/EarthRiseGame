#include "CppUnitTest.h"
#include "net/FakeCrypto.h"
#include "net/Handshake.h"
#include "net/SecureChannel.h"
#include "net/HandshakeMessages.h"
#include "net/Protocol.h"

#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace HandshakeTestHelpers {

inline bool RunHandshake(Neuron::Net::FakeCrypto& crypto,
                         Neuron::Net::AeadKey& cKeyC2S, Neuron::Net::AeadKey& cKeyS2C,
                         Neuron::Net::AeadKey& sKeyC2S, Neuron::Net::AeadKey& sKeyS2C,
                         uint64_t& token)
{
    using namespace Neuron::Net;

    std::vector<uint8_t> staticPriv;
    EcPubKey pinnedPub;
    FakeCrypto::MakeFakeStaticKey(staticPriv, pinnedPub);

    const std::vector<uint8_t> serverSecret = { 's','e','c','r','e','t' };
    const std::vector<uint8_t> clientAddr   = { 127, 0, 0, 1, 0x30, 0x39 };

    HandshakeServerStateless stateless{ &crypto, serverSecret };
    HandshakeClient client(&crypto, pinnedPub, 1000);

    HsOutput hello = client.Begin();
    if (!hello.send) return false;
    HsOutput cookie = stateless.OnClientHello(hello.body, clientAddr);
    if (!cookie.send || cookie.type != MsgType::Cookie) return false;

    HsOutput cookieResp = client.OnMessage(cookie.type, cookie.body);
    if (!cookieResp.send || cookieResp.type != MsgType::CookieResponse) return false;

    CookieResponseBody cr;
    if (!CookieResponseBody::Decode(cookieResp.body, cr)) return false;
    if (!stateless.VerifyCookie(cr.cookie, clientAddr)) return false;

    token = 0xABCDEF0123456789ull;
    HandshakeServerConnection server(&crypto, staticPriv, token, 5000);

    HsOutput hsResp = server.OnCookieResponse(cookieResp.body);
    if (!hsResp.send || hsResp.type != MsgType::HandshakeResponse) return false;

    HsOutput clockReq = client.OnMessage(hsResp.type, hsResp.body);
    if (!clockReq.send || clockReq.type != MsgType::ClockSyncRequest) return false;

    HsOutput clockResp = server.OnClockSyncRequest(clockReq.body);
    if (!clockResp.send || clockResp.type != MsgType::ClockSyncResponse) return false;
    HsOutput done = client.OnMessage(clockResp.type, clockResp.body);
    (void)done;

    if (!client.IsComplete() || !server.IsComplete()) return false;
    if (!client.HasKeys()    || !server.HasKeys())    return false;

    cKeyC2S = client.KeyC2S(); cKeyS2C = client.KeyS2C();
    sKeyC2S = server.KeyC2S(); sKeyS2C = server.KeyS2C();
    return true;
}

} // namespace HandshakeTestHelpers

TEST_CLASS(HandshakeTests)
{
public:
    TEST_METHOD(FullSequenceDerivesMatchingKeys)
    {
        Neuron::Net::FakeCrypto crypto;
        Neuron::Net::AeadKey cC2S, cS2C, sC2S, sS2C;
        uint64_t token = 0;
        Assert::IsTrue(HandshakeTestHelpers::RunHandshake(crypto, cC2S, cS2C, sC2S, sS2C, token));
        Assert::IsTrue(cC2S == sC2S);
        Assert::IsTrue(cS2C == sS2C);
        Assert::IsFalse(cC2S == cS2C);
    }

    TEST_METHOD(SecureChannelRoundTrip)
    {
        Neuron::Net::FakeCrypto crypto;
        Neuron::Net::AeadKey cC2S, cS2C, sC2S, sS2C;
        uint64_t token = 0;
        Assert::IsTrue(HandshakeTestHelpers::RunHandshake(crypto, cC2S, cS2C, sC2S, sS2C, token));

        Neuron::Net::SecureChannel clientCh(&crypto, Neuron::Net::Direction::ClientToServer, token);
        Neuron::Net::SecureChannel serverCh(&crypto, Neuron::Net::Direction::ServerToClient, token);
        clientCh.SetKeys(cC2S, cS2C, 0);
        serverCh.SetKeys(sS2C, sC2S, 0);

        const std::vector<uint8_t> plain = {1, 2, 3, 4, 5};
        std::vector<uint8_t> datagram;
        Assert::IsTrue(clientCh.Seal(plain, datagram));

        std::vector<uint8_t> recovered;
        auto r = serverCh.Open(datagram, recovered);
        Assert::IsTrue(r == Neuron::Net::SecureChannel::OpenResult::Ok);
        Assert::IsTrue(recovered == plain);
    }

    TEST_METHOD(ReplayRejected)
    {
        Neuron::Net::FakeCrypto crypto;
        Neuron::Net::AeadKey cC2S, cS2C, sC2S, sS2C;
        uint64_t token = 0;
        Assert::IsTrue(HandshakeTestHelpers::RunHandshake(crypto, cC2S, cS2C, sC2S, sS2C, token));

        Neuron::Net::SecureChannel clientCh(&crypto, Neuron::Net::Direction::ClientToServer, token);
        Neuron::Net::SecureChannel serverCh(&crypto, Neuron::Net::Direction::ServerToClient, token);
        clientCh.SetKeys(cC2S, cS2C, 0);
        serverCh.SetKeys(sS2C, sC2S, 0);

        const std::vector<uint8_t> plain = {9, 9, 9};
        std::vector<uint8_t> datagram;
        Assert::IsTrue(clientCh.Seal(plain, datagram));

        std::vector<uint8_t> out1, out2;
        Assert::IsTrue(serverCh.Open(datagram, out1) == Neuron::Net::SecureChannel::OpenResult::Ok);
        Assert::IsTrue(serverCh.Open(datagram, out2) == Neuron::Net::SecureChannel::OpenResult::Replay);
    }

    TEST_METHOD(TamperedCiphertextRejected)
    {
        Neuron::Net::FakeCrypto crypto;
        Neuron::Net::AeadKey cC2S, cS2C, sC2S, sS2C;
        uint64_t token = 0;
        Assert::IsTrue(HandshakeTestHelpers::RunHandshake(crypto, cC2S, cS2C, sC2S, sS2C, token));

        Neuron::Net::SecureChannel clientCh(&crypto, Neuron::Net::Direction::ClientToServer, token);
        Neuron::Net::SecureChannel serverCh(&crypto, Neuron::Net::Direction::ServerToClient, token);
        clientCh.SetKeys(cC2S, cS2C, 0);
        serverCh.SetKeys(sS2C, sC2S, 0);

        const std::vector<uint8_t> plain = {4, 5, 6};
        std::vector<uint8_t> datagram;
        Assert::IsTrue(clientCh.Seal(plain, datagram));
        datagram.back() ^= 0xFF;

        std::vector<uint8_t> out;
        Assert::IsTrue(serverCh.Open(datagram, out) == Neuron::Net::SecureChannel::OpenResult::AuthFailure);
    }

    TEST_METHOD(MitmSignatureRejected)
    {
        using namespace Neuron::Net;
        FakeCrypto crypto;

        std::vector<uint8_t> staticPriv;
        EcPubKey pinnedPub;
        FakeCrypto::MakeFakeStaticKey(staticPriv, pinnedPub);
        const std::vector<uint8_t> serverSecret = {'x'};
        const std::vector<uint8_t> clientAddr   = {10, 0, 0, 1, 0, 1};

        HandshakeServerStateless stateless{&crypto, serverSecret};
        HandshakeClient client(&crypto, pinnedPub, 1000);

        HsOutput hello      = client.Begin();
        HsOutput cookie     = stateless.OnClientHello(hello.body, clientAddr);
        HsOutput cookieResp = client.OnMessage(cookie.type, cookie.body);

        HandshakeServerConnection server(&crypto, staticPriv, 1234, 5000);
        HsOutput hsResp = server.OnCookieResponse(cookieResp.body);

        HandshakeResponseBody body;
        Assert::IsTrue(HandshakeResponseBody::Decode(hsResp.body, body));
        body.serverEphemeralPub[0] ^= 0xFF;
        std::vector<uint8_t> tampered;
        body.Encode(tampered);

        HsOutput verdict = client.OnMessage(MsgType::HandshakeResponse, tampered);
        Assert::IsTrue(verdict.failed);
        Assert::IsTrue(verdict.reason == DisconnectReason::HandshakeFailure);
    }

    TEST_METHOD(VersionMismatchRejected)
    {
        using namespace Neuron::Net;
        FakeCrypto crypto;
        const std::vector<uint8_t> serverSecret = {'y'};
        const std::vector<uint8_t> clientAddr   = {10, 0, 0, 2, 0, 2};
        HandshakeServerStateless stateless{&crypto, serverSecret};

        ClientHelloBody bad;
        bad.protocolId = 0xDEADBEEF;
        std::vector<uint8_t> body;
        bad.Encode(body);

        HsOutput out = stateless.OnClientHello(body, clientAddr);
        Assert::IsTrue(out.send);
        Assert::IsTrue(out.type == MsgType::VersionReject);
    }

    TEST_METHOD(BadCookieRejected)
    {
        using namespace Neuron::Net;
        FakeCrypto crypto;
        const std::vector<uint8_t> serverSecret = {'z'};
        const std::vector<uint8_t> clientAddr   = {10, 0, 0, 3, 0, 3};
        HandshakeServerStateless stateless{&crypto, serverSecret};

        Cookie forged{};
        forged[0] = 0x42;
        Assert::IsFalse(stateless.VerifyCookie(forged, clientAddr));
    }
};
