#pragma once

// ServerStatusClient.h — client-side poller for the optional server diagnostic port
// (debug builds only). Given an ISocket bound locally and the server's status
// endpoint (host + the SEPARATE diagnostic port, NOT the game port), it sends the
// fixed query token and parses the JSON reply into a ServerStatus plus a list of
// pre-formatted display lines for the debug overlay.
//
// Platform-independent — it drives the ISocket abstraction, so it unit-tests on the
// Linux runner against a LoopbackSocket (ServerStatusTests). The EarthRise client
// supplies a real WinRT DatagramSocket and draws Lines() via the CanvasRenderer.
//
// The whole feature is gated by _DEBUG at its socket edges: the app opens the socket
// and constructs this only under _DEBUG. The class itself is plain and builds
// anywhere, but is never instantiated in a retail client.

#include "ISocket.h"
#include "ServerStatus.h"

#include <array>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Neuron::Client
{

class ServerStatusClient
{
public:
    // 'socket' must outlive this object and be Open()ed by the caller. 'host'/'port'
    // address the server's diagnostic status port. A port of 0 disables the poller
    // (RequestStatus becomes a no-op), matching the server-side "off by default".
    ServerStatusClient(Neuron::Net::ISocket* socket, std::string host, uint16_t port)
        : m_socket(socket), m_server{ std::move(host), port }
    {
    }

    [[nodiscard]] bool Enabled() const noexcept { return m_socket != nullptr && m_server.port != 0; }

    // Send one status query to the server's diagnostic port (fire-and-forget UDP).
    // The reply arrives asynchronously and is picked up by a later Poll().
    void RequestStatus()
    {
        if (!Enabled()) return;
        m_socket->SendTo(m_server,
            std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(Neuron::Net::STATUS_QUERY_TOKEN),
                Neuron::Net::STATUS_QUERY_TOKEN_SIZE));
    }

    // Drain any pending reply datagrams; on a valid status reply, refresh Last() /
    // Lines() and return true. Non-blocking (returns false when nothing arrived).
    bool Poll()
    {
        if (!m_socket) return false;
        bool updated = false;
        std::array<uint8_t, Neuron::Net::STATUS_MAX_DATAGRAM_BYTES> buf{};
        for (;;)
        {
            Neuron::Net::Endpoint from;
            const int n = m_socket->RecvFrom(from, std::span<uint8_t>(buf.data(), buf.size()));
            if (n <= 0) break;
            Neuron::Net::ServerStatus s;
            const std::string_view text(reinterpret_cast<const char*>(buf.data()),
                                        static_cast<size_t>(n));
            if (Neuron::Net::ParseStatusJson(text, s))
            {
                m_last  = s;
                m_valid = true;
                m_lines = FormatStatusLines(s);
                updated = true;
            }
        }
        return updated;
    }

    [[nodiscard]] bool Valid() const noexcept { return m_valid; }
    [[nodiscard]] const Neuron::Net::ServerStatus& Last() const noexcept { return m_last; }
    [[nodiscard]] const std::vector<std::string>& Lines() const noexcept { return m_lines; }

    // Build the human-readable overlay lines from a status record. Pure + static, so
    // it unit-tests without a socket. The first line is the panel title; the rest are
    // one label/value row each.
    [[nodiscard]] static std::vector<std::string> FormatStatusLines(const Neuron::Net::ServerStatus& s)
    {
        auto line = [](const char* fmt, auto... args) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), fmt, args...);
            return std::string(buf);
        };

        std::vector<std::string> out;
        out.push_back(line("SERVER STATUS  (proto %u)", static_cast<unsigned>(s.protocolVersion)));
        out.push_back(line("uptime       %llu s", static_cast<unsigned long long>(s.uptimeSeconds)));
        out.push_back(line("sim tick     %llu", static_cast<unsigned long long>(s.simTick)));
        out.push_back(line("connections  %u active / %u total",
                           s.connectionsActive, s.connectionsPending));
        out.push_back(line("objects      %llu  (proj %llu)",
                           static_cast<unsigned long long>(s.objectsSpawned),
                           static_cast<unsigned long long>(s.projectiles)));
        out.push_back(line("sim p99      %.2f ms", s.simP99Ms));
        out.push_back(line("encode p99   %.2f ms", s.encodeP99Ms));
        out.push_back(line("dilation     %.2f", s.dilation));
        out.push_back(line("net down     %llu B / %llu dg",
                           static_cast<unsigned long long>(s.downstreamBytes),
                           static_cast<unsigned long long>(s.datagramsOut)));
        out.push_back(line("net up       %llu B / %llu dg",
                           static_cast<unsigned long long>(s.upstreamBytes),
                           static_cast<unsigned long long>(s.datagramsIn)));
        out.push_back(line("baseline RAM %llu B", static_cast<unsigned long long>(s.baselineBytes)));
        out.push_back(line("port %u  auth %s  persist %s",
                           static_cast<unsigned>(s.listenPort),
                           s.devAuthStub ? "DEV-STUB" : "real",
                           s.persistenceEnabled ? "on" : "off"));
        return out;
    }

private:
    Neuron::Net::ISocket*     m_socket{ nullptr };
    Neuron::Net::Endpoint     m_server;
    Neuron::Net::ServerStatus m_last;
    std::vector<std::string>  m_lines;
    bool                      m_valid{ false };
};

} // namespace Neuron::Client
