// ClientConfigTests.cpp — coverage for NeuronClient/ClientConfig.h, the JSON
// loader that replaced ERHeadless's ER_SERVER_HOST/PORT/PUB env reads. Exercises
// the JSON document → ClientConfig mapping (header-only, so it runs in the Linux
// testrunner).

#include "TestRunner.h"

#include "ClientConfig.h"
#include "Json.h"

using Neuron::Client::ClientConfig;

namespace
{
ClientConfig fromText(std::string_view text)
{
    Neuron::Json::Value root;
    std::string err;
    if (!Neuron::Json::Parse(text, root, &err))
        ::ertest::fail("config JSON failed to parse: " + err);
    return ClientConfig::FromJson(root);
}
} // namespace

ER_TEST(ClientConfig, FullDocument)
{
    const ClientConfig c = fromText(R"({
        "server":   { "host": "10.0.0.5", "port": 9100, "pinnedPublicKeyFile": "keys/pub.bin" },
        "headless": { "botCount": 12 }
    })");
    ER_CHECK_EQ(c.host, std::string("10.0.0.5"));
    ER_CHECK_EQ(c.port, uint16_t(9100));
    ER_CHECK_EQ(c.pinnedPublicKeyFile, std::string("keys/pub.bin"));
    ER_CHECK_EQ(c.botCount, uint32_t(12));
}

ER_TEST(ClientConfig, DefaultsWhenKeysMissing)
{
    const ClientConfig c = fromText("{}");
    ER_CHECK_EQ(c.host, std::string(Neuron::Client::DEFAULT_SERVER_HOST));
    ER_CHECK_EQ(c.port, Neuron::Client::DEFAULT_SERVER_PORT);
    ER_CHECK_EQ(c.pinnedPublicKeyFile, std::string(Neuron::Client::DEFAULT_PINNED_PUBKEY_FILE));
    ER_CHECK_EQ(c.botCount, Neuron::Client::DEFAULT_BOT_COUNT);
}

ER_TEST(ClientConfig, PartialDocumentKeepsOtherDefaults)
{
    const ClientConfig c = fromText(R"({ "server": { "port": 5000 } })");
    ER_CHECK_EQ(c.port, uint16_t(5000));
    ER_CHECK_EQ(c.host, std::string(Neuron::Client::DEFAULT_SERVER_HOST)); // untouched
    ER_CHECK_EQ(c.botCount, Neuron::Client::DEFAULT_BOT_COUNT);            // section absent
}
