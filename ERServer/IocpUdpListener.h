#pragma once
// Server-side IOCP UDP listener — §9 (skeleton for M1a).
//
// Windows-only. ERServer scales to many connections on a single UDP socket using
// an I/O completion port: a small pool of worker threads block on
// GetQueuedCompletionStatus, each consuming a completed WSARecvFrom, dispatching
// the datagram to the registered callback, and re-posting a fresh WSARecvFrom.
//
// IMPORTANT (§9) — per-connection affinity:
//   Reliability state (per-channel sequence/ack) and the AEAD decrypt state (packet
//   number / nonce, replay window) are all PER-CONNECTION and are NOT safe to touch
//   from multiple IOCP threads at once.
//   The IOCP threads here only do the raw recv + dispatch; the callback MUST route
//   each datagram to the owning connection's single-threaded context. The intended
//   scheme is hash(remote endpoint) % numWorkers -> a fixed worker/lane so that all
//   datagrams for one connection are always processed by the same lane, removing
//   the race without locks. This skeleton exposes the raw callback and documents
//   the hash; the connection-affinity router is built on top in a later milestone.
//
// Links ws2_32.lib (see .cpp).

#include "ISocket.h" // Neuron::Net::Endpoint

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

// Forward-declare so winsock2.h stays out of this header.
struct IocpUdpListenerImpl;

namespace Neuron::Server
{

class IocpUdpListener
{
public:
    using RecvCallback =
        std::function<void(const Neuron::Net::Endpoint&, std::span<const uint8_t>)>;

    IocpUdpListener();
    ~IocpUdpListener();

    IocpUdpListener(const IocpUdpListener&) = delete;
    IocpUdpListener& operator=(const IocpUdpListener&) = delete;

    // Bind a UDP socket to 'port' (0 = ephemeral), create the completion port,
    // spawn 'numThreads' worker threads (0 => one per hardware thread) and post
    // the initial batch of overlapped receives. Returns false on failure.
    bool Start(uint16_t port, unsigned numThreads);

    // Signal workers to exit, cancel outstanding I/O, join, and clean up.
    void Stop();

    // Register the datagram callback. Set before Start(). The callback may run on
    // ANY worker thread — see the affinity note above.
    void SetRecvCallback(RecvCallback cb);

    // Send a datagram to a remote endpoint. Returns bytes sent, or -1 on error.
    // Thread-safe for concurrent senders on a single UDP socket.
    int SendTo(const Neuron::Net::Endpoint& to, std::span<const uint8_t> data);

    [[nodiscard]] uint16_t LocalPort() const;

private:
    IocpUdpListenerImpl* impl_ = nullptr;
};

} // namespace Neuron::Server
