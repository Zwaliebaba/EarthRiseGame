#pragma once
// UDP socket abstraction — §8.1.
//
// A thin interface so NeuronClient can run over Winsock on both the UWP client
// and ERHeadless, and so tests can substitute an in-memory loopback that injects
// loss/reorder/duplication. ERServer uses a separate IOCP-based listener
// (ERServer/IocpUdpListener) rather than this per-client interface.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Neuron::Net
{

// A network endpoint (IPv4/IPv6 address + port), kept opaque-ish for portability.
struct Endpoint
{
    std::string ip;        // textual address ("127.0.0.1", "::1", …)
    uint16_t    port{ 0 };

    bool operator==(const Endpoint& o) const noexcept
    {
        return port == o.port && ip == o.ip;
    }

    // Serialized address bytes for cookie HMAC input (§8.5 step 1).
    [[nodiscard]] std::vector<uint8_t> AddrBytes() const
    {
        std::vector<uint8_t> b(ip.begin(), ip.end());
        b.push_back(static_cast<uint8_t>(port & 0xFF));
        b.push_back(static_cast<uint8_t>((port >> 8) & 0xFF));
        return b;
    }
};

class ISocket
{
public:
    virtual ~ISocket() = default;

    // Bind to a local port (0 = ephemeral). Returns false on failure.
    virtual bool Open(uint16_t localPort) = 0;
    virtual void Close() = 0;

    // Non-blocking send to a remote endpoint. Returns bytes sent, -1 on error.
    virtual int SendTo(const Endpoint& to, std::span<const uint8_t> data) = 0;

    // Non-blocking receive. Returns bytes received (0 if none available, -1 error)
    // and fills 'from' with the sender's endpoint.
    virtual int RecvFrom(Endpoint& from, std::span<uint8_t> buffer) = 0;

    [[nodiscard]] virtual uint16_t LocalPort() const = 0;
};

} // namespace Neuron::Net
