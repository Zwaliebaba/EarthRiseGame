// IocpUdpListener.cpp — server-side IOCP UDP receive loop (§9 skeleton).
//
// Windows-only. Links ws2_32.lib (pragma below + ERServer.vcxproj dependencies).
//
// Receive model:
//   * One AF_INET6 dual-stack UDP socket bound to the listen port.
//   * The socket is associated with an IOCP via CreateIoCompletionPort.
//   * We pre-post a pool of overlapped WSARecvFrom operations (RecvOp objects),
//     each with its own OVERLAPPED, WSABUF, datagram buffer and source sockaddr.
//   * Worker threads loop on GetQueuedCompletionStatus. On each completion they
//     decode the source address, invoke the callback with the received bytes,
//     then re-issue WSARecvFrom on the same RecvOp to keep the pool saturated.
//   * Stop() posts a sentinel completion (key = kStopKey) per worker so each
//     thread wakes and exits, then closes the socket and IOCP.
//
// The OVERLAPPED* returned by GetQueuedCompletionStatus is the first member of
// RecvOp, so we recover the owning RecvOp by CONTAINING_RECORD.

#include "pch.h"

#include "IocpUdpListener.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "Protocol.h" // kMaxDatagramBytes

#pragma comment(lib, "ws2_32.lib")

namespace
{

using Neuron::Net::Endpoint;

constexpr ULONG_PTR kRecvKey = 1; // completion key for socket I/O
constexpr ULONG_PTR kStopKey = 0; // sentinel to wake/exit a worker

// How many overlapped receives we keep posted at once. Higher = more in-flight
// receive capacity under burst load at the cost of memory.
constexpr int kRecvPoolSize = 64;

// One outstanding overlapped receive. OVERLAPPED MUST be the first member so the
// completion-port OVERLAPPED* maps back to this struct.
struct RecvOp
{
    OVERLAPPED       ov;            // must be first
    WSABUF           wsabuf;
    sockaddr_storage from;
    INT              fromLen;
    DWORD            flags;
    uint8_t          buffer[Neuron::Net::kMaxDatagramBytes];
};

bool SockaddrToEndpoint(const sockaddr* sa, Endpoint& out)
{
    char buf[INET6_ADDRSTRLEN] = {};
    if (sa->sa_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(sa);
        if (!inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf))) return false;
        out.ip = buf; out.port = ntohs(a->sin_port); return true;
    }
    if (sa->sa_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(sa);
        if (!inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf))) return false;
        out.ip = buf; out.port = ntohs(a->sin6_port); return true;
    }
    return false;
}

bool EndpointToSockaddr(const Endpoint& ep, sockaddr_storage& out, int& outLen)
{
    std::memset(&out, 0, sizeof(out));
    in6_addr a6{};
    if (inet_pton(AF_INET6, ep.ip.c_str(), &a6) == 1) {
        auto* sa = reinterpret_cast<sockaddr_in6*>(&out);
        sa->sin6_family = AF_INET6; sa->sin6_addr = a6; sa->sin6_port = htons(ep.port);
        outLen = sizeof(sockaddr_in6); return true;
    }
    in_addr a4{};
    if (inet_pton(AF_INET, ep.ip.c_str(), &a4) == 1) {
        // Wrap as v4-mapped IPv6 so the dual-stack socket can send it.
        auto* sa = reinterpret_cast<sockaddr_in6*>(&out);
        sa->sin6_family = AF_INET6; sa->sin6_port = htons(ep.port);
        sa->sin6_addr.u.Byte[10] = 0xff; sa->sin6_addr.u.Byte[11] = 0xff;
        std::memcpy(&sa->sin6_addr.u.Byte[12], &a4, 4);
        outLen = sizeof(sockaddr_in6); return true;
    }
    return false;
}

// Hash a remote endpoint to a worker "lane" for per-connection affinity (§9).
// Not used to dispatch in this skeleton (the raw callback fans out to whoever
// completes), but provided so the connection router built on top has a single,
// canonical lane assignment to consult.
[[maybe_unused]] unsigned LaneForEndpoint(const Endpoint& ep, unsigned numLanes)
{
    if (numLanes == 0) return 0;
    uint64_t h = 1469598103934665603ull; // FNV-1a
    for (char c : ep.ip) { h ^= static_cast<uint8_t>(c); h *= 1099511628211ull; }
    h ^= ep.port; h *= 1099511628211ull;
    return static_cast<unsigned>(h % numLanes);
}

} // namespace

// Pimpl: keeps all Winsock types out of the header.
struct IocpUdpListenerImpl
{
    SOCKET                                       sock   = INVALID_SOCKET;
    HANDLE                                       iocp   = nullptr;
    uint16_t                                     port   = 0;
    std::vector<std::thread>                     workers;
    std::vector<RecvOp*>                         recvOps;
    std::atomic<bool>                            running{ false };
    Neuron::Server::IocpUdpListener::RecvCallback callback;

    bool PostRecv(RecvOp* op)
    {
        std::memset(&op->ov, 0, sizeof(op->ov));
        op->wsabuf.buf = reinterpret_cast<CHAR*>(op->buffer);
        op->wsabuf.len = static_cast<ULONG>(sizeof(op->buffer));
        op->fromLen    = static_cast<INT>(sizeof(op->from));
        op->flags      = 0;

        const int rc = ::WSARecvFrom(
            sock, &op->wsabuf, 1, nullptr, &op->flags,
            reinterpret_cast<sockaddr*>(&op->from), &op->fromLen,
            &op->ov, nullptr);

        if (rc == SOCKET_ERROR) {
            const int err = ::WSAGetLastError();
            if (err != WSA_IO_PENDING)
                return false; // a true failure to queue the receive
        }
        return true;
    }

    void WorkerLoop()
    {
        while (running.load(std::memory_order_acquire)) {
            DWORD       bytes = 0;
            ULONG_PTR   key   = 0;
            OVERLAPPED* ov    = nullptr;

            const BOOL ok = ::GetQueuedCompletionStatus(
                iocp, &bytes, &key, &ov, INFINITE);

            if (key == kStopKey)
                break; // shutdown sentinel

            if (ov == nullptr)
                continue; // spurious / port closing

            RecvOp* op = CONTAINING_RECORD(ov, RecvOp, ov);

            if (ok && bytes > 0) {
                Endpoint from;
                if (SockaddrToEndpoint(reinterpret_cast<const sockaddr*>(&op->from), from)) {
                    // NOTE (§9): this runs on an arbitrary worker thread. The
                    // callback must hand off to the connection's affinitised lane
                    // (see LaneForEndpoint) before touching reliability/decrypt/
                    // reassembly state. Do NOT mutate per-connection state here.
                    if (callback)
                        callback(from, std::span<const uint8_t>(op->buffer, bytes));
                }
            }
            // ok==FALSE here typically means a recv-side ICMP error
            // (WSAECONNRESET) for a prior datagram; we simply re-post.

            if (running.load(std::memory_order_acquire))
                PostRecv(op); // keep the pool saturated
        }
    }
};

namespace Neuron::Server
{

IocpUdpListener::IocpUdpListener()  : impl_(new IocpUdpListenerImpl) {}
IocpUdpListener::~IocpUdpListener() { Stop(); delete impl_; impl_ = nullptr; }

void IocpUdpListener::SetRecvCallback(RecvCallback cb)
{
    impl_->callback = std::move(cb);
}

bool IocpUdpListener::Start(uint16_t port, unsigned numThreads)
{
    if (impl_->running.load())
        return false; // already started

    // WSAStartup — refcounted by Winsock; ERServer may also start it elsewhere.
    WSADATA wsaData{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return false;

    // Overlapped UDP socket (dual-stack v6).
    impl_->sock = ::WSASocketW(AF_INET6, SOCK_DGRAM, IPPROTO_UDP,
                               nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (impl_->sock == INVALID_SOCKET) {
        ::WSACleanup();
        return false;
    }

    DWORD v6only = 0;
    ::setsockopt(impl_->sock, IPPROTO_IPV6, IPV6_V6ONLY,
                 reinterpret_cast<const char*>(&v6only), sizeof(v6only));

    sockaddr_in6 local{};
    local.sin6_family = AF_INET6;
    local.sin6_addr   = in6addr_any;
    local.sin6_port   = htons(port);
    if (::bind(impl_->sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        ::closesocket(impl_->sock); impl_->sock = INVALID_SOCKET;
        ::WSACleanup();
        return false;
    }

    // Resolve the actual bound port (handles ephemeral port == 0).
    sockaddr_in6 bound{}; int boundLen = sizeof(bound);
    impl_->port = port;
    if (::getsockname(impl_->sock, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0)
        impl_->port = ntohs(bound.sin6_port);

    // Create the completion port and associate the socket with it.
    impl_->iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (impl_->iocp == nullptr) {
        ::closesocket(impl_->sock); impl_->sock = INVALID_SOCKET;
        ::WSACleanup();
        return false;
    }
    if (::CreateIoCompletionPort(reinterpret_cast<HANDLE>(impl_->sock),
                                 impl_->iocp, kRecvKey, 0) == nullptr) {
        ::CloseHandle(impl_->iocp); impl_->iocp = nullptr;
        ::closesocket(impl_->sock); impl_->sock = INVALID_SOCKET;
        ::WSACleanup();
        return false;
    }

    impl_->running.store(true, std::memory_order_release);

    // Pre-post the receive pool.
    impl_->recvOps.reserve(kRecvPoolSize);
    for (int i = 0; i < kRecvPoolSize; ++i) {
        auto* op = new RecvOp{};
        impl_->recvOps.push_back(op);
        if (!impl_->PostRecv(op)) {
            // Couldn't prime the pool — tear down.
            impl_->running.store(false);
            Stop();
            return false;
        }
    }

    // Spawn workers.
    const unsigned n = (numThreads != 0) ? numThreads
                                         : (std::thread::hardware_concurrency()
                                                ? std::thread::hardware_concurrency() : 2u);
    impl_->workers.reserve(n);
    for (unsigned i = 0; i < n; ++i)
        impl_->workers.emplace_back([this] { impl_->WorkerLoop(); });

    return true;
}

void IocpUdpListener::Stop()
{
    if (!impl_ || !impl_->running.exchange(false))
        return;

    // Closing the socket cancels outstanding WSARecvFrom; workers then see the
    // stop sentinel and exit.
    if (impl_->sock != INVALID_SOCKET) {
        ::closesocket(impl_->sock);
        impl_->sock = INVALID_SOCKET;
    }

    if (impl_->iocp) {
        for (size_t i = 0; i < impl_->workers.size(); ++i)
            ::PostQueuedCompletionStatus(impl_->iocp, 0, kStopKey, nullptr);
    }

    for (auto& t : impl_->workers)
        if (t.joinable())
            t.join();
    impl_->workers.clear();

    for (auto* op : impl_->recvOps)
        delete op;
    impl_->recvOps.clear();

    if (impl_->iocp) {
        ::CloseHandle(impl_->iocp);
        impl_->iocp = nullptr;
    }

    ::WSACleanup();
}

int IocpUdpListener::SendTo(const Neuron::Net::Endpoint& to, std::span<const uint8_t> data)
{
    if (!impl_ || impl_->sock == INVALID_SOCKET)
        return -1;

    sockaddr_storage dst{}; int dstLen = 0;
    if (!EndpointToSockaddr(to, dst, dstLen))
        return -1;

    // A blocking-style sendto on an overlapped socket completes inline for UDP;
    // concurrent sends from multiple threads on one UDP socket are safe.
    const int n = ::sendto(
        impl_->sock,
        reinterpret_cast<const char*>(data.data()),
        static_cast<int>(data.size()),
        0,
        reinterpret_cast<const sockaddr*>(&dst),
        dstLen);

    if (n == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
            return 0;
        return -1;
    }
    return n;
}

uint16_t IocpUdpListener::LocalPort() const
{
    return impl_ ? impl_->port : 0;
}

} // namespace Neuron::Server
