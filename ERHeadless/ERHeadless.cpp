// ERHeadless.cpp — Headless client host entry point (M0 skeleton).
//
// Hosts N NeuronClient sessions in one process (each on its own UDP port).
// Used for:
//   - Integration tests (>=3 bots see M1a acceptance criteria)
//   - Record/replay determinism harness
//   - Load tests (~100 bots at M4)
//   - In-world bots (post-M1)
//
// Bots != PvE NPCs. Bots are client sessions; PvE NPCs are server-side AI.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <memory>

#include "platform/Debug.h"
#include "control/IClientController.h"
#include "session/Session.h"

static constexpr uint32_t kDefaultBotCount = 3;

int main(int argc, char* argv[])
{
    const uint32_t botCount = (argc >= 2) ? static_cast<uint32_t>(std::atoi(argv[1]))
                                          : kDefaultBotCount;

    EARTHRISE_LOG_INFO("ERHeadless starting — %u bot sessions (M0 skeleton).\n", botCount);

    // TODO M1a: spin up 'botCount' NeuronClient sessions, connect to ERServer,
    // drive with NullController (or scripted controllers), pump session ticks.

    std::vector<std::unique_ptr<Neuron::Client::NullController>> controllers;
    controllers.reserve(botCount);
    for (uint32_t i = 0; i < botCount; ++i)
        controllers.emplace_back(std::make_unique<Neuron::Client::NullController>());

    EARTHRISE_LOG_INFO("ERHeadless created %u null-controllers (transport not connected in M0).\n", botCount);
    EARTHRISE_LOG_INFO("ERHeadless: M0 done — exiting.\n");
    return 0;
}
