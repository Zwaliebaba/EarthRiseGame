#pragma once

// ClientConfig.h — client/bot connection settings, loaded from a JSON file
// instead of the process environment (§20). This replaces the former ER_SERVER_HOST
// / ER_SERVER_PORT / ER_SERVER_PUB environment reads in ERHeadless.
//
// Layout of the document (see Config/erheadless.config.example.json):
//
//   {
//     "server":   { "host": "127.0.0.1", "port": 7777, "pinnedPublicKeyFile": "er_server_pub.bin" },
//     "headless": { "botCount": 3 }
//   }
//
// Every field has a default, so a missing file or partial document still yields a
// usable config (Load() returns false on a missing/invalid file so the caller can
// decide whether to fall back to defaults or fail).

#include <cstdint>
#include <string>

#include "Json.h"

namespace Neuron::Client
{

inline constexpr const char* DEFAULT_SERVER_HOST = "127.0.0.1";
inline constexpr uint16_t    DEFAULT_SERVER_PORT = 7777;
inline constexpr const char* DEFAULT_PINNED_PUBKEY_FILE = "er_server_pub.bin";
inline constexpr uint32_t    DEFAULT_BOT_COUNT = 3;

// The config filename the client/headless host looks for in its working directory
// when no --config <path> argument is given.
inline constexpr const char* DEFAULT_CONFIG_FILENAME = "erheadless.config.json";

struct ClientConfig
{
    std::string host{ DEFAULT_SERVER_HOST };                  // server.host
    uint16_t    port{ DEFAULT_SERVER_PORT };                  // server.port
    std::string pinnedPublicKeyFile{ DEFAULT_PINNED_PUBKEY_FILE }; // server.pinnedPublicKeyFile
    uint32_t    botCount{ DEFAULT_BOT_COUNT };                // headless.botCount

    // Build a config from an already-parsed JSON document (the whole root object).
    // Missing keys fall back to the defaults above.
    [[nodiscard]] static ClientConfig FromJson(const Neuron::Json::Value& root)
    {
        ClientConfig out;
        const Neuron::Json::Value& server = root["server"];
        out.host = server.getString("host", DEFAULT_SERVER_HOST);
        out.port = static_cast<uint16_t>(server.getUint32("port", DEFAULT_SERVER_PORT));
        out.pinnedPublicKeyFile =
            server.getString("pinnedPublicKeyFile", DEFAULT_PINNED_PUBKEY_FILE);

        const Neuron::Json::Value& headless = root["headless"];
        out.botCount = headless.getUint32("botCount", DEFAULT_BOT_COUNT);
        return out;
    }

    // Load the config from a JSON file on disk. Returns true on success; on a
    // parse/IO error returns false (with a diagnostic in 'error') and leaves 'out'
    // at defaults. Missing keys in a valid document fall back to the defaults above.
    [[nodiscard]] static bool Load(const std::string& path, ClientConfig& out,
                                   std::string* error = nullptr)
    {
        Neuron::Json::Value root;
        if (!Neuron::Json::ParseFile(path, root, error))
            return false;
        out = FromJson(root);
        return true;
    }
};

} // namespace Neuron::Client
