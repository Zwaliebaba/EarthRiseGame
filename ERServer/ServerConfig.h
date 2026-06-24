#pragma once

// ServerConfig.h — the whole ERServer configuration, loaded from a JSON file
// (§20). This replaces the former per-setting process-environment reads
// (ER_LISTEN_PORT / ER_DEV_AUTH_STUB and PersistConfig::LoadFromEnv): the daemon
// now reads exactly one JSON document and nothing from the environment.
//
// Layout of the document (see Config/erserver.config.example.json):
//
//   {
//     "server":     { "listenPort": 7777, "devAuthStub": false },
//     "database":   { "connectionString": "...", "user": "...", "password": "...", "authMode": "sql" },
//     "auth":       { "serverPepper": "...", "pbkdf2Iterations": 210000, ... },
//     "durability": { "writeBehindRpoMs": 2000, "snapshotMs": 60000 },
//     "pool":       { "min": 1, "max": 4 }
//   }
//
// The "database"/"auth"/"durability"/"pool" sections are owned by
// PersistConfig::FromJson; this struct adds the listener/dev-auth settings that
// live outside the persist layer. Every field has a sane default, so a partial
// config still loads.

#include <cstdint>
#include <string>

#include "Json.h"
#include "persist/PersistConfig.h"

namespace Neuron::Server
{

// Default UDP port clients connect to (former ER_LISTEN_PORT default).
inline constexpr uint16_t DEFAULT_LISTEN_PORT = 7777;

// The config filename the daemon looks for in its working directory when no
// --config <path> argument is given.
inline constexpr const char* DEFAULT_CONFIG_FILENAME = "erserver.config.json";

struct ServerConfig
{
    // -- server / listener ------------------------------------------------------
    uint16_t listenPort{ DEFAULT_LISTEN_PORT };  // server.listenPort

    // Dev flag: keeps the M1-M4 "pick a name" identity (no credentials). Default
    // OFF = real account auth (former ER_DEV_AUTH_STUB). server.devAuthStub
    bool devAuthStub{ false };

    // -- persistence / auth / durability / pool ---------------------------------
    Neuron::Persist::PersistConfig persist;

    // Load the config from a JSON file on disk. Returns true on success; on a
    // parse/IO error returns false and writes a diagnostic to 'error' (if given)
    // and leaves 'out' at defaults. A successfully-parsed document that simply
    // omits keys is NOT an error — missing keys fall back to the defaults above
    // (so e.g. a config with no "database" section loads as a no-persist run).
    [[nodiscard]] static bool Load(const std::string& path, ServerConfig& out,
                                   std::string* error = nullptr)
    {
        Neuron::Json::Value root;
        if (!Neuron::Json::ParseFile(path, root, error))
            return false;

        const Neuron::Json::Value& server = root["server"];
        out.listenPort = static_cast<uint16_t>(
            server.getUint32("listenPort", DEFAULT_LISTEN_PORT));
        out.devAuthStub = server.getBool("devAuthStub", false);

        out.persist = Neuron::Persist::PersistConfig::FromJson(root);
        return true;
    }
};

} // namespace Neuron::Server
