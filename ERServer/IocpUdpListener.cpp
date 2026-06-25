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
//   * Stop() posts a sentinel completion (key = STOP_KEY) per worker so each
//     thread wakes and exits, then closes the socket and IOCP.
//
// The OVERLAPPED* returned by GetQueuedCompletionStatus is the first member of
// RecvOp, so we recover the owning RecvOp by CONTAINING_RECORD.
//
// Per-connection affinity (M4 area G): the IOCP receive threads do not invoke the
// callback directly. They peek each datagram's 64-bit connection token (clear AEAD
// header, App. A) and enqueue the datagram onto that token's affinity *lane* — a
// dedicated worker thread chosen by a stable token→lane hash, so all datagrams for
// one connection are serviced single-threaded. A cookie-phase datagram (no token)
// is laned by its ip:port instead, so a peer's handshake also stays on one lane.

#include "pch.h"

#include "IocpUdpListener.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "ConnectionTable.h" // Neuron::Net::PeekConnectionToken (token-peek router)
#include "Protocol.h"        // MAX_DATAGRAM_BYTES

#pragma comment(lib, "ws2_32.lib")

namespace
{

using Neuron::Net::Endpoint;

constexpr ULONG_PTR RECV_KEY = 1; // completion key for socket I/O
constexpr ULONG_PTR STOP_KEY = 0; // sentinel to wake/exit a worker

// How many overlapped receives we keep posted at once. Higher = more in-flight
// receive capacity under burst load at the cost of memory.
constexpr int RECV_POOL_SIZE = 64;

// One outstanding overlapped receive. OVERLAPPED MUST be the first member so the
// completion-port OVERLAPPED* maps back to this struct.
struct RecvOp
{
    OVERLAPPED       ov;            // must be first
    WSABUF           wsabuf;
    sockaddr_storage from;
    INT              fromLen;
    DWORD            flags;
    uint8_t          buffer[Neuron::Net::MAX_DATAGRAM_BYTES];
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

// Lane for a post-handshake datagram: a stable hash of the 64-bit connection token
// (M4 area G). All datagrams for one connection hash to the same lane → the callback
// is single-threaded per connection. This need not equal ConnectionTable::Lane()'s
// slot-index lane (the listener doesn't know the slot index); affinity only requires
// the assignment be *consistent per connection*, which a token hash guarantees.
unsigned LaneForToken(uint64_t token, unsigned numLanes)
{
    if (numLanes == 0) return 0;
    uint64_t h = token * 0x9E3779B97F4A7C15ull; // splitmix-style spread
    h ^= h >> 32;
    return static_cast<unsigned>(h % numLanes);
}

// Lane for a cookie-phase datagram (no token yet): hash the peer's ip:port so a
// peer's whole handshake stays on one lane until its token-bearing traffic begins.
unsigned LaneForEndpoint(const Endpoint& ep, unsigned numLanes)
{
    if (numLanes == 0) return 0;
    uint64_t h = 1469598103934665603ull; // FNV-1a
    for (char c : ep.ip) { h ^= static_cast<uint8_t>(c); h *= 1099511628211ull; }
    h ^= ep.port; h *= 1099511628211ull;
    return static_cast<unsigned>(h % numLanes);
}

// A datagram handed from an IOCP receive thread to an affinity lane: the source
// endpoint plus the owned bytes (copied out of the RecvOp so the RecvOp can be
// re-posted immediately).
struct LaneItem
{
    Endpoint             from;
    std::vector<uint8_t> bytes;
};

// One affinity lane: a single worker thread draining a FIFO queue. One connection's
// datagrams always land on one lane, so the callback runs single-threaded per conn.
struct Lane
{
    std::mutex               mtx;
    std::condition_variable  cv;
    std::deque<LaneItem>     queue;
    std::thread              thread;
    std::atomic<bool>        running{ false };
};

} // namespace

// Pimpl: keeps all Winsock types out of the header.
struct IocpUdpListenerImpl
{
    SOCKET                                       sock   = INVALID_SOCKET;
    HANDLE                                       iocp   = nullptr;
    uint16_t                                     port   = 0;
    unsigned                                     laneCount = 0; // 0 until Start resolves it
    std::vector<std::thread>                     workers;       // IOCP receive threads
    std::vector<RecvOp*>                         recvOps;
    std::vector<std::unique_ptr<Lane>>           lanes;         // per-connection affinity lanes
    std::atomic<bool>                            running{ false };
    Neuron::Server::IocpUdpListener::RecvCallback callback;

    // Route + enqueue a received datagram to its connection's affinity lane (area G).
    void DispatchToLane(const Endpoint& from, const uint8_t* data, size_t len)
    {
        if (lanes.empty()) {
            // Lane dispatch disabled — fall back to the raw inline callback (M1a).
            if (callback) callback(from, std::span<const uint8_t>(data, len));
            return;
        }
        const std::span<const uint8_t> dg(data, len);
        const auto token = Neuron::Net::PeekConnectionToken(dg);
        const unsigned lane = token ? LaneForToken(*token, laneCount)
                                    : LaneForEndpoint(from, laneCount);
        Lane& L = *lanes[lane];
        {
            std::lock_guard<std::mutex> lk(L.mtx);
            L.queue.push_back({ from, std::vector<uint8_t>(data, data + len) });
        }
        L.cv.notify_one();
    }

    // One lane worker: drain its queue in order, invoking the callback single-
    // threaded so per-connection reliability/decrypt state is never raced (area G).
    void LaneLoop(Lane* L)
    {
        for (;;) {
            LaneItem item;
            {
                std::unique_lock<std::mutex> lk(L->mtx);
                L->cv.wait(lk, [&] { return !L->queue.empty() || !L->running.load(); });
                if (!L->running.load() && L->queue.empty()) return;
                item = std::move(L->queue.front());
                L->queue.pop_front();
            }
            if (callback)
                callback(item.from, std::span<const uint8_t>(item.bytes.data(), item.bytes.size()));
        }
    }

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

            if (key == STOP_KEY)
                break; // shutdown sentinel

            if (ov == nullptr)
                continue; // spurious / port closing

            RecvOp* op = CONTAINING_RECORD(ov, RecvOp, ov);

            if (ok && bytes > 0) {
                Endpoint from;
                if (SockaddrToEndpoint(reinterpret_cast<const sockaddr*>(&op->from), from)) {
                    // NOTE (§9): this runs on an arbitrary worker thread. The
                    // callback must hand off to the connection's affinitised lane
                    // (see LaneForEndpoint) before touching reliability/decrypt
                    // state. Do NOT mutate per-connection state here.
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

void IocpUdpListener::SetLaneCount(unsigned lanes)
{
    if (!impl_ || impl_->running.load())
        return;

    impl_->laneCount = lanes;
}

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
                                 impl_->iocp, RECV_KEY, 0) == nullptr) {
        ::CloseHandle(impl_->iocp); impl_->iocp = nullptr;
        ::closesocket(impl_->sock); impl_->sock = INVALID_SOCKET;
        ::WSACleanup();
        return false;
    }

    impl_->running.store(true, std::memory_order_release);

    const unsigned resolvedLaneCount = (impl_->laneCount != 0)
        ? impl_->laneCount
        : (std::thread::hardware_concurrency() != 0 ? std::thread::hardware_concurrency() : 1u);

    impl_->lanes.clear();
    impl_->lanes.reserve(resolvedLaneCount);
    for (unsigned i = 0; i < resolvedLaneCount; ++i) {
        auto lane = std::make_unique<Lane>();
        lane->running.store(true, std::memory_order_release);
        lane->thread = std::thread([this, rawLane = lane.get()] { impl_->LaneLoop(rawLane); });
        impl_->lanes.push_back(std::move(lane));
    }
    impl_->laneCount = resolvedLaneCount;

    // Pre-post the receive pool.
    impl_->recvOps.reserve(RECV_POOL_SIZE);
    for (int i = 0; i < RECV_POOL_SIZE; ++i) {
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
            ::PostQueuedCompletionStatus(impl_->iocp, 0, STOP_KEY, nullptr);
    }

    for (auto& t : impl_->workers)
        if (t.joinable())
            t.join();
    impl_->workers.clear();

    for (auto& lane : impl_->lanes) {
        lane->running.store(false, std::memory_order_release);
        lane->cv.notify_one();
    }
    for (auto& lane : impl_->lanes)
        if (lane->thread.joinable())
            lane->thread.join();
    impl_->lanes.clear();

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

unsigned IocpUdpListener::LaneCount() const
{
    return impl_ ? impl_->laneCount : 0;
}

} // namespace Neuron::Server
