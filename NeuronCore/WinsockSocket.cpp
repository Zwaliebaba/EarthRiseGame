// WinsockSocket.cpp — Winsock UDP implementation of ISocket (§8.1).
//
// Windows-only. Links ws2_32.lib (provided via #pragma comment below and also in
// the consuming .vcxproj's AdditionalDependencies).
//
// Semantics required by ISocket:
//   SendTo  : returns bytes sent, or -1 on error. WSAEWOULDBLOCK on a UDP send is
//             effectively a transient "send buffer full"; we report it as 0 bytes
//             sent (caller may retry) rather than a hard error.
//   RecvFrom: returns >0 bytes received, 0 when no datagram is ready
//             (WSAEWOULDBLOCK), or -1 on a real error.

#include "pch.h"

#include "WinsockSocket.h"

#include <cstring>
#include <utility>

// Confirm our portable handle type matches the platform SOCKET type so the
// reinterpret in this file is well-defined.
static_assert(sizeof(uintptr_t) == sizeof(SOCKET),
              "uintptr_t must alias SOCKET (UINT_PTR) for WinsockSocket");

namespace Neuron::Net
{
namespace
{

inline SOCKET ToSocket(uintptr_t s) { return static_cast<SOCKET>(s); }

// Convert a sockaddr (v4 or v6) to an Endpoint. Returns false on unknown family.
bool SockaddrToEndpoint(const sockaddr* sa, int /*saLen*/, Endpoint& out)
{
    char buf[INET6_ADDRSTRLEN] = {};
    if (sa->sa_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(sa);
        if (!inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf)))
            return false;
        out.ip   = buf;
        out.port = ntohs(a->sin_port);
        return true;
    }
    if (sa->sa_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(sa);
        if (!inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf)))
            return false;
        out.ip   = buf;
        out.port = ntohs(a->sin6_port);
        return true;
    }
    return false;
}

// Build a sockaddr_storage from an Endpoint. 'preferV6' picks the wrapping family
// when the textual address is an IPv4 literal but the socket is dual-stack v6:
// in that case we emit a v4-mapped IPv6 address (::ffff:a.b.c.d).
bool EndpointToSockaddr(const Endpoint& ep, bool preferV6,
                        sockaddr_storage& out, int& outLen)
{
    std::memset(&out, 0, sizeof(out));

    // Try IPv6 literal first.
    in6_addr a6{};
    if (inet_pton(AF_INET6, ep.ip.c_str(), &a6) == 1) {
        auto* sa = reinterpret_cast<sockaddr_in6*>(&out);
        sa->sin6_family = AF_INET6;
        sa->sin6_addr   = a6;
        sa->sin6_port   = htons(ep.port);
        outLen = sizeof(sockaddr_in6);
        return true;
    }

    // Try IPv4 literal.
    in_addr a4{};
    if (inet_pton(AF_INET, ep.ip.c_str(), &a4) == 1) {
        if (preferV6) {
            // Emit v4-mapped IPv6 so a dual-stack v6 socket can reach it.
            auto* sa = reinterpret_cast<sockaddr_in6*>(&out);
            sa->sin6_family = AF_INET6;
            sa->sin6_port   = htons(ep.port);
            // ::ffff:a.b.c.d
            sa->sin6_addr.u.Byte[10] = 0xff;
            sa->sin6_addr.u.Byte[11] = 0xff;
            std::memcpy(&sa->sin6_addr.u.Byte[12], &a4, 4);
            outLen = sizeof(sockaddr_in6);
            return true;
        }
        auto* sa = reinterpret_cast<sockaddr_in*>(&out);
        sa->sin_family = AF_INET;
        sa->sin_addr   = a4;
        sa->sin_port   = htons(ep.port);
        outLen = sizeof(sockaddr_in);
        return true;
    }

    return false; // not a valid textual address
}

} // namespace

// ----------------------------------------------------------------------------
// Global Winsock lifetime (refcounted)
// ----------------------------------------------------------------------------
bool WinsockSocket::GlobalStartup()
{
    static int s_refcount = 0;
    if (s_refcount > 0) {
        ++s_refcount;
        return true;
    }
    WSADATA wsaData{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (rc != 0)
        return false;
    ++s_refcount;
    return true;
}

void WinsockSocket::GlobalCleanup()
{
    // Matches the refcount kept by GlobalStartup. We keep it simple: each cleanup
    // issues one WSACleanup (Winsock itself refcounts WSAStartup/WSACleanup pairs).
    WSACleanup();
}

// ----------------------------------------------------------------------------
WinsockSocket::~WinsockSocket()
{
    Close();
}

WinsockSocket::WinsockSocket(WinsockSocket&& other) noexcept
    : sock_(other.sock_), localPort_(other.localPort_), isV6_(other.isV6_)
{
    other.sock_      = kInvalidSocket;
    other.localPort_ = 0;
    other.isV6_      = false;
}

WinsockSocket& WinsockSocket::operator=(WinsockSocket&& other) noexcept
{
    if (this != &other) {
        Close();
        sock_      = other.sock_;
        localPort_ = other.localPort_;
        isV6_      = other.isV6_;
        other.sock_      = kInvalidSocket;
        other.localPort_ = 0;
        other.isV6_      = false;
    }
    return *this;
}

// ----------------------------------------------------------------------------
bool WinsockSocket::Open(uint16_t localPort)
{
    Close();

    // Prefer a dual-stack AF_INET6 socket so we can talk v4 and v6. Fall back to
    // plain IPv4 if v6 sockets aren't available on the host.
    SOCKET s = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    bool   v6 = true;
    if (s == INVALID_SOCKET) {
        s  = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        v6 = false;
        if (s == INVALID_SOCKET)
            return false;
    }

    if (v6) {
        // Disable IPV6_V6ONLY for dual-stack. Non-fatal if it fails.
        DWORD off = 0;
        ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY,
                     reinterpret_cast<const char*>(&off), sizeof(off));
    }

    // Bind to the requested local port (0 = ephemeral) on the wildcard address.
    sockaddr_storage local{};
    int localLen = 0;
    if (v6) {
        auto* a = reinterpret_cast<sockaddr_in6*>(&local);
        a->sin6_family = AF_INET6;
        a->sin6_addr   = in6addr_any;
        a->sin6_port   = htons(localPort);
        localLen = sizeof(sockaddr_in6);
    } else {
        auto* a = reinterpret_cast<sockaddr_in*>(&local);
        a->sin_family      = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_ANY);
        a->sin_port        = htons(localPort);
        localLen = sizeof(sockaddr_in);
    }

    if (::bind(s, reinterpret_cast<const sockaddr*>(&local), localLen) == SOCKET_ERROR) {
        ::closesocket(s);
        return false;
    }

    // Enable non-blocking I/O.
    u_long nonBlocking = 1;
    if (::ioctlsocket(s, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
        ::closesocket(s);
        return false;
    }

    // Query the actual bound port (resolves the ephemeral case).
    sockaddr_storage bound{};
    int boundLen = sizeof(bound);
    uint16_t actualPort = localPort;
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
        if (bound.ss_family == AF_INET6)
            actualPort = ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
        else if (bound.ss_family == AF_INET)
            actualPort = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
    }

    sock_      = static_cast<uintptr_t>(s);
    localPort_ = actualPort;
    isV6_      = v6;
    return true;
}

void WinsockSocket::Close()
{
    if (sock_ != kInvalidSocket) {
        ::closesocket(ToSocket(sock_));
        sock_ = kInvalidSocket;
    }
    localPort_ = 0;
    isV6_      = false;
}

int WinsockSocket::SendTo(const Endpoint& to, std::span<const uint8_t> data)
{
    if (sock_ == kInvalidSocket)
        return -1;

    sockaddr_storage dst{};
    int dstLen = 0;
    if (!EndpointToSockaddr(to, /*preferV6=*/isV6_, dst, dstLen))
        return -1; // unparseable destination address

    const int n = ::sendto(
        ToSocket(sock_),
        reinterpret_cast<const char*>(data.data()),
        static_cast<int>(data.size()),
        0,
        reinterpret_cast<const sockaddr*>(&dst),
        dstLen);

    if (n == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
            return 0; // send buffer momentarily full; caller may retry
        return -1;
    }
    return n;
}

int WinsockSocket::RecvFrom(Endpoint& from, std::span<uint8_t> buffer)
{
    if (sock_ == kInvalidSocket)
        return -1;

    sockaddr_storage src{};
    int srcLen = sizeof(src);

    const int n = ::recvfrom(
        ToSocket(sock_),
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0,
        reinterpret_cast<sockaddr*>(&src),
        &srcLen);

    if (n == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
            return 0; // no datagram available
        // WSAECONNRESET on a UDP socket means a prior send got an ICMP port-
        // unreachable. It is not fatal to the socket; treat as "no data this call".
        if (err == WSAECONNRESET || err == WSAEMSGSIZE)
            return 0;
        return -1;
    }

    if (!SockaddrToEndpoint(reinterpret_cast<const sockaddr*>(&src), srcLen, from)) {
        // Received but couldn't decode sender; still report the bytes.
        from.ip.clear();
        from.port = 0;
    }
    return n;
}

uint16_t WinsockSocket::LocalPort() const
{
    return localPort_;
}

} // namespace Neuron::Net
