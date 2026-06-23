#pragma once
// Server-side IOCP UDP listener — §9 (skeleton for M1a).
//
// Windows-only. ERServer scales to many connections on a single UDP socket using
// an I/O completion port: a small pool of worker threads block on
// GetQueuedCompletionStatus, each consuming a completed WSARecvFrom, dispatching
// the datagram to the registered callback, and re-posting a fresh WSARecvFrom.
//
// IMPORTANT (§9) — per-connection affinity (M4 area G):
//   Reliability state (per-channel sequence/ack) and the AEAD decrypt state (packet
//   number / nonce, replay window) are all PER-CONNECTION and are NOT safe to touch
//   from multiple IOCP threads at once.
//   The IOCP receive threads here only do the raw recv; they then hand each datagram
//   to its connection's affinity *lane* — a fixed worker thread chosen by the 64-bit
//   connection token (post-handshake) or by the peer's ip:port (cookie phase) — so
//   ALL datagrams for one connection are always processed by the same lane. That
//   removes the race on per-connection state without locks. The callback runs on the
//   lane thread, single-threaded per connection; concurrency is across *connections*,
//   not within one. (M1a used the raw fan-out; M4 enables lane dispatch by default.)
//
// M4: Windows integration — unverified on Linux (Winsock/IOCP); validate on the
// Windows build agent. The lane assignment matches ConnectionTable::Lane semantics.
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

    // Number of per-connection affinity lanes (M4 area G). Each connection is pinned
    // to exactly one lane by its token (or ip:port pre-handshake), so the callback is
    // single-threaded per connection. Set before Start(); 0 => one per hardware
    // thread. The default (0 = unset) enables lane dispatch at Start().
    void SetLaneCount(unsigned lanes);

    // Bind a UDP socket to 'port' (0 = ephemeral), create the completion port,
    // spawn 'numThreads' IOCP receive worker threads (0 => one per hardware thread)
    // plus the affinity-lane threads, and post the initial batch of overlapped
    // receives. Returns false on failure.
    bool Start(uint16_t port, unsigned numThreads);

    // Signal workers to exit, cancel outstanding I/O, join, and clean up.
    void Stop();

    // Register the datagram callback. Set before Start(). With lane dispatch on, the
    // callback runs on the datagram's affinity lane — single-threaded per connection
    // (see the affinity note above), so it may safely touch per-connection state.
    void SetRecvCallback(RecvCallback cb);

    // Send a datagram to a remote endpoint. Returns bytes sent, or -1 on error.
    // Thread-safe for concurrent senders on a single UDP socket.
    int SendTo(const Neuron::Net::Endpoint& to, std::span<const uint8_t> data);

    [[nodiscard]] uint16_t LocalPort() const;
    [[nodiscard]] unsigned LaneCount() const;

private:
    IocpUdpListenerImpl* impl_ = nullptr;
};

} // namespace Neuron::Server
