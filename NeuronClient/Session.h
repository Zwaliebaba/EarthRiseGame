#pragma once
// Session — encrypted reliable-UDP session to ERServer (§8, §10.1).
// M0 skeleton: interface only; implementation lands in M1a.

#include <cstdint>
#include <span>
#include <string_view>

namespace Neuron::Client
{

enum class SessionState : uint8_t
{
    Disconnected,
    Handshaking,     // stateless cookie → version gate → ECDH → clock sync
    Authenticating,  // login exchange over encrypted channel
    Connected,       // in the tick/snapshot loop
    Reconnecting,
};

class Session
{
public:
    virtual ~Session() = default;

    // -- Lifecycle --
    virtual bool Connect(std::string_view host, uint16_t port)     = 0;
    virtual void Disconnect()                                       = 0;
    virtual void Tick()                                             = 0; // pump network I/O

    // -- State --
    [[nodiscard]] virtual SessionState GetState() const noexcept   = 0;
    [[nodiscard]] virtual bool         IsConnected() const noexcept = 0;
    [[nodiscard]] virtual uint64_t     GetSessionToken() const noexcept = 0;

    // -- Send --
    // Variable-length RTS fleet intent (§23.4; M3 area B). 'body' is an encoded
    // Neuron::Sim::FleetCommand (EncodeFleetCommand); the server validates it. All
    // gameplay (incl. base relocation) goes through this — there is no separate command.
    virtual void SendFleetCommand(std::span<const uint8_t> body)    = 0;

    // -- Receive (polled by client loop) --
    // Returns false when no more snapshots are available this tick.
    virtual bool PollSnapshot(std::span<uint8_t> outBuf, size_t& outSize) = 0;
};

// Factory — returns a null stub in M0; wired to real transport in M1a.
Session* CreateSession();
void     DestroySession(Session* s);

} // namespace Neuron::Client
