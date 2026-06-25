// ServerStatusTests — the platform-independent core of the out-of-band server
// status/diagnostics feature (§21): the JSON encode/decode (NeuronCore/ServerStatus.h),
// the query-token validation, and the client-side poller + line formatter
// (NeuronClient/ServerStatusClient.h) driven over a LoopbackSocket. The sockets in
// ERServer/StatusEndpoint.h and the EarthRise overlay are the build-specific edges
// (Winsock / DatagramSocket, _DEBUG-gated) and are not exercised here.

#include "TestRunner.h"

#include "LoopbackNetwork.h"
#include "ServerStatus.h"
#include "ServerStatusClient.h"

#include <string>

using Neuron::Net::ServerStatus;

namespace
{
// A status record with a distinct, %.4g-safe value in every field, so a round-trip
// that drops or swaps a field is caught.
ServerStatus SampleStatus()
{
    ServerStatus s;
    s.protocolVersion    = Neuron::Net::kStatusProtocolVersion;
    s.uptimeSeconds       = 12345;
    s.simTick             = 67890;
    s.connectionsPending  = 7;
    s.connectionsActive   = 4;
    s.objectsSpawned      = 1024;
    s.projectiles         = 33;
    s.simP99Ms            = 1.25;   // exactly representable; %.4g -> "1.25"
    s.encodeP99Ms         = 0.5;    // -> "0.5"
    s.dilation            = 0.75;   // -> "0.75"
    s.downstreamBytes     = 500000;
    s.upstreamBytes       = 250000;
    s.datagramsIn         = 900;
    s.datagramsOut        = 1100;
    s.baselineBytes       = 4096;
    s.listenPort          = 7777;
    s.devAuthStub         = true;
    s.persistenceEnabled  = true;
    return s;
}
} // namespace

ER_TEST(ServerStatus, EncodeParseRoundTrip)
{
    const ServerStatus in = SampleStatus();
    const std::string json = Neuron::Net::EncodeStatusJson(in);
    ER_CHECK(json.size() < Neuron::Net::kStatusMaxDatagramBytes);

    ServerStatus out;
    ER_CHECK(Neuron::Net::ParseStatusJson(json, out));

    ER_CHECK_EQ(out.protocolVersion, in.protocolVersion);
    ER_CHECK_EQ(out.uptimeSeconds, in.uptimeSeconds);
    ER_CHECK_EQ(out.simTick, in.simTick);
    ER_CHECK_EQ(out.connectionsPending, in.connectionsPending);
    ER_CHECK_EQ(out.connectionsActive, in.connectionsActive);
    ER_CHECK_EQ(out.objectsSpawned, in.objectsSpawned);
    ER_CHECK_EQ(out.projectiles, in.projectiles);
    ER_CHECK(out.simP99Ms == in.simP99Ms);
    ER_CHECK(out.encodeP99Ms == in.encodeP99Ms);
    ER_CHECK(out.dilation == in.dilation);
    ER_CHECK_EQ(out.downstreamBytes, in.downstreamBytes);
    ER_CHECK_EQ(out.upstreamBytes, in.upstreamBytes);
    ER_CHECK_EQ(out.datagramsIn, in.datagramsIn);
    ER_CHECK_EQ(out.datagramsOut, in.datagramsOut);
    ER_CHECK_EQ(out.baselineBytes, in.baselineBytes);
    ER_CHECK_EQ(out.listenPort, in.listenPort);
    ER_CHECK_EQ(out.devAuthStub, in.devAuthStub);
    ER_CHECK_EQ(out.persistenceEnabled, in.persistenceEnabled);
}

ER_TEST(ServerStatus, ParseRejectsGarbageAndDefaultsMissing)
{
    ServerStatus out;
    ER_CHECK(!Neuron::Net::ParseStatusJson("not json", out));
    ER_CHECK(!Neuron::Net::ParseStatusJson("[1,2,3]", out)); // valid JSON but not an object

    // A partial object parses; absent keys fall back to the struct defaults.
    ServerStatus partial;
    ER_CHECK(Neuron::Net::ParseStatusJson("{\"connectionsActive\":3}", partial));
    ER_CHECK_EQ(partial.connectionsActive, 3u);
    ER_CHECK_EQ(partial.objectsSpawned, 0ull);
    ER_CHECK(partial.dilation == 1.0); // default
}

ER_TEST(ServerStatus, QueryTokenValidation)
{
    // The exact token is accepted.
    ER_CHECK(Neuron::Net::IsStatusQuery(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(Neuron::Net::kStatusQueryToken),
        Neuron::Net::kStatusQueryTokenSize)));

    // Wrong length is rejected.
    const uint8_t shortBuf[3] = { 'E', 'R', 'S' };
    ER_CHECK(!Neuron::Net::IsStatusQuery(std::span<const uint8_t>(shortBuf, sizeof(shortBuf))));

    // Right length, one wrong byte is rejected (e.g. a version mismatch).
    uint8_t bad[Neuron::Net::kStatusQueryTokenSize];
    for (size_t i = 0; i < sizeof(bad); ++i) bad[i] = static_cast<uint8_t>(Neuron::Net::kStatusQueryToken[i]);
    bad[sizeof(bad) - 1] ^= 0xFF;
    ER_CHECK(!Neuron::Net::IsStatusQuery(std::span<const uint8_t>(bad, sizeof(bad))));
}

ER_TEST(ServerStatus, FormatStatusLines)
{
    const auto lines = Neuron::Client::ServerStatusClient::FormatStatusLines(SampleStatus());
    ER_CHECK_EQ(lines.size(), 12u); // title + 11 value rows
    ER_CHECK(lines[0].find("SERVER STATUS") != std::string::npos);

    // The connection row reflects the active/total counts.
    bool foundConns = false;
    for (const auto& ln : lines)
        if (ln.find("4 active / 7 total") != std::string::npos) foundConns = true;
    ER_CHECK(foundConns);
}

// Full client path over the in-memory loopback: the client queries the (simulated)
// diagnostic port, a "server" socket validates the token and replies with the JSON,
// and the client parses it back into a status + display lines.
ER_TEST(ServerStatus, ClientPollOverLoopback)
{
    Neuron::Net::LoopbackNetwork net;

    constexpr uint16_t kStatusPort = 7778;
    Neuron::Net::LoopbackSocket serverSock(&net);
    Neuron::Net::LoopbackSocket clientSock(&net);
    ER_CHECK(serverSock.Open(kStatusPort));
    ER_CHECK(clientSock.Open(0)); // ephemeral

    Neuron::Client::ServerStatusClient client(&clientSock, "127.0.0.1", kStatusPort);
    ER_CHECK(client.Enabled());
    ER_CHECK(!client.Valid()); // nothing received yet

    // 1) Client sends the query token to the status port.
    client.RequestStatus();

    // 2) The "server" socket receives it, validates, and replies with the status JSON.
    {
        std::array<uint8_t, Neuron::Net::kStatusMaxDatagramBytes> buf{};
        Neuron::Net::Endpoint from;
        const int n = serverSock.RecvFrom(from, std::span<uint8_t>(buf.data(), buf.size()));
        ER_CHECK(n > 0);
        ER_CHECK(Neuron::Net::IsStatusQuery(std::span<const uint8_t>(buf.data(), static_cast<size_t>(n))));
        const std::string json = Neuron::Net::EncodeStatusJson(SampleStatus());
        serverSock.SendTo(from, std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(json.data()), json.size()));
    }

    // 3) The client polls and parses the reply.
    ER_CHECK(client.Poll());
    ER_CHECK(client.Valid());
    ER_CHECK_EQ(client.Last().objectsSpawned, 1024ull);
    ER_CHECK_EQ(client.Last().connectionsActive, 4u);
    ER_CHECK_EQ(client.Lines().size(), 12u);

    // A disabled client (port 0) is inert: RequestStatus is a no-op, nothing to poll.
    Neuron::Client::ServerStatusClient off(&clientSock, "127.0.0.1", 0);
    ER_CHECK(!off.Enabled());
    off.RequestStatus();
    ER_CHECK(!off.Poll());
}
