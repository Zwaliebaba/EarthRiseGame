#pragma once
// IClientController — abstract intent source for a client session (§10.1).
// Human players and bots both implement this interface.
// The controller produces Commands; the Session sends them to the server.

#include "Session.h"

namespace Neuron::Client
{

// Move intent (one per tick maximum).
struct MoveIntent
{
    float dx{ 0 }, dy{ 0 }, dz{ 0 }; // desired displacement this tick (normalised direction)
    float speed{ 0 };                  // desired speed (m/s); server validates cap
};

class IClientController
{
public:
    virtual ~IClientController() = default;

    // Called each sim tick. Fill 'cmd' with the controller's intent.
    // Return false if no command this tick (idle).
    virtual bool ProduceCommand(uint32_t tick, Command& cmd) = 0;

    // Optional: called when the server sends an ack/correction for tick N.
    // M0: snap-on-ack only; predict/reconcile added post-M1.
    virtual void OnAck(uint32_t tick) {}

    [[nodiscard]] virtual bool IsBot() const noexcept = 0;
};

// Null controller — always idle. Used for testing and stub sessions.
class NullController final : public IClientController
{
public:
    bool ProduceCommand(uint32_t, Command&) override { return false; }
    bool IsBot() const noexcept override { return true; }
};

} // namespace Neuron::Client
