#pragma once
// DatagramSocketAdapter.h — WinRT DatagramSocket implementation of ISocket for the
// UWP client (§8.1). The client transports UDP over
// Windows.Networking.Sockets.DatagramSocket — **not** Winsock, which is the
// server/headless (Win32) path.
//
// Bridging async WinRT → the synchronous, non-blocking ISocket poll API:
// DatagramSocket is event-driven, and the client drives ISocket from the UI (ASTA)
// thread, where blocking on a WinRT async op (`.get()`) is illegal. So this adapter
// never blocks the caller:
//   * Receives arrive on a threadpool thread via MessageReceived and are pushed onto
//     a mutex-guarded queue; RecvFrom drains it (returns 0 when empty), matching the
//     non-blocking Winsock semantics.
//   * Bind and send run as background coroutines. SendTo enqueues bytes and returns
//     immediately; datagrams sent before the bind completes queue and flush once
//     bound — the reliable-UDP RTO layer tolerates that brief startup delay.
//
// Lifetime: keep the adapter alive while sends are in flight (it is owned for the
// app's lifetime). Close() revokes the receive handler; in-flight send/bind
// coroutines observe `m_open == false` and bail.

#include "ISocket.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.Streams.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Neuron::Net
{

class DatagramSocketAdapter final : public ISocket
{
public:
    DatagramSocketAdapter() = default;
    ~DatagramSocketAdapter() override;

    DatagramSocketAdapter(const DatagramSocketAdapter&)            = delete;
    DatagramSocketAdapter& operator=(const DatagramSocketAdapter&) = delete;

    // ISocket -----------------------------------------------------------------
    bool                   Open(uint16_t localPort) override;
    void                   Close() override;
    int                    SendTo(const Endpoint& to, std::span<const uint8_t> data) override;
    int                    RecvFrom(Endpoint& from, std::span<uint8_t> buffer) override;
    [[nodiscard]] uint16_t LocalPort() const override;

private:
    struct RxDatagram
    {
        Endpoint             from;
        std::vector<uint8_t> bytes;
    };

    // Per-remote send channel: a cached output stream plus the queue of datagrams
    // waiting to be written, drained one StoreAsync at a time by a single background
    // coroutine (the `draining` flag guarantees no overlapping StoreAsync per stream).
    struct TxChannel
    {
        Endpoint                                        endpoint;
        winrt::Windows::Storage::Streams::IOutputStream stream{ nullptr };
        std::deque<std::vector<uint8_t>>                pending;
        bool                                            draining{ false };
    };

    void OnMessage(
        winrt::Windows::Networking::Sockets::DatagramSocket const&,
        winrt::Windows::Networking::Sockets::DatagramSocketMessageReceivedEventArgs const& args);
    winrt::fire_and_forget BindAsync(uint16_t localPort);
    winrt::fire_and_forget DrainTx(std::string key);

    static std::string KeyOf(const Endpoint& e) { return e.ip + ":" + std::to_string(e.port); }

    winrt::Windows::Networking::Sockets::DatagramSocket m_socket{ nullptr };
    winrt::event_token                                  m_msgToken{};

    std::mutex             m_rxMutex;
    std::deque<RxDatagram> m_rx;

    std::mutex                                 m_txMutex; // guards m_socket + m_tx
    std::unordered_map<std::string, TxChannel> m_tx;

    std::atomic<uint16_t> m_localPort{ 0 };
    std::atomic<bool>     m_open{ false };
};

} // namespace Neuron::Net
