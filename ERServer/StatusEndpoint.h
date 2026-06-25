#pragma once

// StatusEndpoint.h — ERServer's optional out-of-band diagnostic status port (debug
// builds only; the whole feature is compiled out of retail via _DEBUG at the call
// sites in ERServer.cpp).
//
// It binds a SEPARATE UDP socket — distinct from the game IOCP listener — on a
// configured port and, when it receives the fixed status query token
// (ServerStatus.h), replies with a one-datagram JSON snapshot of the live server
// status. A debug EarthRise client polls it to drive the server-status overlay.
//
// The socket is the plain WinsockSocket (low-traffic, non-blocking), NOT the IOCP
// path: this is a diagnostics side-channel and never touches the 30 Hz hot loop
// (§9). The status is built lazily — only when a query actually arrives — so an
// unwatched port costs nothing per loop.
//
// Security: the port answers ONLY the exact query token (IsStatusQuery) and emits
// read-only counters; it accepts no commands. It is intended for a trusted operator
// network, is dark in retail (not compiled), and off by default (statusPort 0). Do
// not expose it on an untrusted interface.

#include "ServerStatus.h"
#include "WinsockSocket.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace Neuron::Server
{

class StatusEndpoint
{
public:
    // Bind the diagnostic UDP socket on 'port'. port == 0 disables the endpoint
    // (Start returns false and Poll is a no-op). Returns true if the port is open.
    bool Start(uint16_t port)
    {
        if (port == 0) return false;
        if (!m_socket.Open(port)) return false;
        m_open = true;
        return true;
    }

    [[nodiscard]] bool IsOpen() const noexcept { return m_open; }
    [[nodiscard]] uint16_t LocalPort() const { return m_open ? m_socket.LocalPort() : uint16_t{ 0 }; }

    // Drain pending datagrams; answer each valid status query with the current
    // status (built lazily via 'makeStatus' only when a query actually arrives, so
    // there is no per-loop cost when nobody is watching). Call once per server loop.
    void Poll(const std::function<Neuron::Net::ServerStatus()>& makeStatus)
    {
        if (!m_open) return;
        std::array<uint8_t, Neuron::Net::STATUS_MAX_DATAGRAM_BYTES> buf{};
        for (;;)
        {
            Neuron::Net::Endpoint from;
            const int n = m_socket.RecvFrom(from, std::span<uint8_t>(buf.data(), buf.size()));
            if (n <= 0) break;
            if (!Neuron::Net::IsStatusQuery(
                    std::span<const uint8_t>(buf.data(), static_cast<size_t>(n))))
                continue; // ignore anything that isn't the exact query token

            const std::string json = Neuron::Net::EncodeStatusJson(makeStatus());
            m_socket.SendTo(from, std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(json.data()), json.size()));
        }
    }

    void Stop()
    {
        if (m_open)
        {
            m_socket.Close();
            m_open = false;
        }
    }

private:
    Neuron::Net::WinsockSocket m_socket;
    bool                       m_open{ false };
};

} // namespace Neuron::Server
