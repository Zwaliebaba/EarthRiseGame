#pragma once
// ScriptedController — a deterministic, platform-independent command log for the
// ERHeadless bots and the record/replay harness (§10.3, §16.1/§16.2). A bot
// "input log" is a list of (tick → FleetCommand) steps; replaying the same log on
// a fresh sim must reproduce the run bit-for-bit (ServerUniverse::SimHash). The
// EarthRise client/ERHeadless feed the per-tick commands into their Session; tests
// feed them straight into ServerUniverse::ApplyFleetCommand.

#include "Command.h" // FleetCommand (NeuronCore)

#include <cstdint>
#include <vector>

namespace Neuron::Client
{

struct ScriptedStep
{
    uint32_t                player{ 0 }; // which player issues it (server checks ownership)
    uint32_t                tick{ 0 };   // sim tick at which to issue the command
    Neuron::Sim::FleetCommand cmd{};
};

// An ordered command log, replayed by sim tick. Deterministic: CommandsForTick
// returns steps in insertion order, so two replays issue identical commands in
// identical order.
class ScriptedController
{
public:
    void Add(uint32_t player, uint32_t tick, Neuron::Sim::FleetCommand cmd)
    {
        m_log.push_back({ player, tick, std::move(cmd) });
    }

    // Steps due exactly at 'tick' (in insertion order). Advances an internal
    // cursor, so call once per ascending tick during a run.
    [[nodiscard]] std::vector<const ScriptedStep*> StepsForTick(uint32_t tick)
    {
        std::vector<const ScriptedStep*> due;
        for (const auto& s : m_log)
            if (s.tick == tick) due.push_back(&s);
        return due;
    }

    [[nodiscard]] const std::vector<ScriptedStep>& Log() const noexcept { return m_log; }
    [[nodiscard]] uint32_t LastTick() const noexcept
    {
        uint32_t t = 0;
        for (const auto& s : m_log) t = s.tick > t ? s.tick : t;
        return t;
    }

private:
    std::vector<ScriptedStep> m_log;
};

} // namespace Neuron::Client
