#pragma once
// Winsock UDP implementation of ISocket — §8.1.
//
// Windows-only. This is the real per-client datagram socket used by NeuronClient
// (UWP client and ERHeadless). ERServer does NOT use this — it has its own
// IOCP-based listener (ERServer/IocpUdpListener) for many-connection scale.
//
// Design notes:
//  * winsock2.h is NOT pulled into this header. The SOCKET handle is stored as a
//    uintptr_t (which is exactly what the Win32 SOCKET typedef is — UINT_PTR), so
//    callers that only need ISocket don't transitively include <winsock2.h>.
//    The .cpp casts the member back to SOCKET. INVALID_SOCKET == (SOCKET)(~0),
//    which as uintptr_t is the all-ones sentinel kInvalidSocket below.
//  * Non-blocking mode is enabled via ioctlsocket(FIONBIO) in Open().
//  * Links ws2_32.lib (see .cpp).
//
// IPv4 and IPv6 are both supported: Open() creates an AF_INET6 dual-stack socket
// when possible (IPV6_V6ONLY=0) so it can talk to both v4-mapped and v6 peers;
// it falls back to AF_INET if v6 is unavailable. SendTo resolves the textual
// address in Endpoint::ip to the right family with inet_pton.

#include "ISocket.h"

#include <cstdint>

namespace Neuron::Net
{

class WinsockSocket final : public ISocket
{
public:
    WinsockSocket() = default;
    ~WinsockSocket() override;

    WinsockSocket(const WinsockSocket&) = delete;
    WinsockSocket& operator=(const WinsockSocket&) = delete;
    WinsockSocket(WinsockSocket&& other) noexcept;
    WinsockSocket& operator=(WinsockSocket&& other) noexcept;

    // Process-wide Winsock lifetime. Call GlobalStartup() once before creating any
    // WinsockSocket and GlobalCleanup() once at shutdown. They are refcounted so
    // nested calls are safe. Returns false if WSAStartup failed.
    static bool GlobalStartup();
    static void GlobalCleanup();

    // ISocket -----------------------------------------------------------------
    bool Open(uint16_t localPort) override;
    void Close() override;
    int  SendTo(const Endpoint& to, std::span<const uint8_t> data) override;
    int  RecvFrom(Endpoint& from, std::span<uint8_t> buffer) override;
    [[nodiscard]] uint16_t LocalPort() const override;

private:
    // Mirrors Win32 SOCKET (UINT_PTR). All-ones is INVALID_SOCKET.
    static constexpr uintptr_t kInvalidSocket = static_cast<uintptr_t>(~uintptr_t{ 0 });

    uintptr_t sock_{ kInvalidSocket };
    uint16_t  localPort_{ 0 };
    bool      isV6_{ false }; // true if the bound socket is AF_INET6 (dual-stack)
};

} // namespace Neuron::Net
