// DatagramSocketAdapter.cpp — WinRT DatagramSocket ISocket for the UWP client (§8.1).

#include "pch.h"
#include "DatagramSocketAdapter.h"

#include <algorithm>
#include <cstring>

using namespace winrt::Windows::Networking;
using namespace winrt::Windows::Networking::Sockets;
using namespace winrt::Windows::Storage::Streams;

namespace Neuron::Net
{

DatagramSocketAdapter::~DatagramSocketAdapter()
{
    Close();
}

bool DatagramSocketAdapter::Open(uint16_t localPort)
{
    Close();
    try
    {
        DatagramSocket socket;
        // Receives arrive on a threadpool thread; OnMessage enqueues under m_rxMutex.
        m_msgToken = socket.MessageReceived(
            [this](DatagramSocket const& s, DatagramSocketMessageReceivedEventArgs const& a) {
                OnMessage(s, a);
            });
        {
            std::lock_guard lk(m_txMutex);
            m_socket = socket;
        }
    }
    catch (...)
    {
        return false;
    }

    m_open.store(true);
    // Bind runs in the background; Open returns immediately (non-blocking — blocking a
    // WinRT async op on the ASTA thread is illegal). Sends queue until the bind lands.
    BindAsync(localPort);
    return true;
}

winrt::fire_and_forget DatagramSocketAdapter::BindAsync(uint16_t localPort)
{
    co_await winrt::resume_background();

    DatagramSocket socket{ nullptr };
    {
        std::lock_guard lk(m_txMutex);
        socket = m_socket;
    }
    if (!socket || !m_open.load())
        co_return;

    try
    {
        // Empty service name = ephemeral local port.
        co_await socket.BindServiceNameAsync(
            localPort == 0 ? winrt::hstring{} : winrt::to_hstring(static_cast<uint32_t>(localPort)));
        uint16_t lp = 0;
        try
        {
            lp = static_cast<uint16_t>(std::stoul(winrt::to_string(socket.Information().LocalPort())));
        }
        catch (...)
        {
        }
        m_localPort.store(lp);
    }
    catch (...)
    {
        m_open.store(false); // bind failed; sends will be rejected
    }
}

void DatagramSocketAdapter::Close()
{
    m_open.store(false);

    DatagramSocket socket{ nullptr };
    {
        std::lock_guard lk(m_txMutex);
        socket = m_socket;
        m_socket = nullptr;
        m_tx.clear();
    }
    if (socket)
    {
        if (m_msgToken.value)
        {
            try { socket.MessageReceived(m_msgToken); } catch (...) {}
            m_msgToken = {};
        }
        try { socket.Close(); } catch (...) {}
    }
    {
        std::lock_guard lk(m_rxMutex);
        m_rx.clear();
    }
    m_localPort.store(0);
}

int DatagramSocketAdapter::SendTo(const Endpoint& to, std::span<const uint8_t> data)
{
    if (!m_open.load())
        return -1;

    const std::string key = KeyOf(to);
    bool kick = false;
    {
        std::lock_guard lk(m_txMutex);
        auto& ch    = m_tx[key];
        ch.endpoint = to;
        ch.pending.emplace_back(data.begin(), data.end());
        if (!ch.draining)
        {
            ch.draining = true;
            kick        = true;
        }
    }
    if (kick)
        DrainTx(key);

    // Optimistic: queued for an async store (UDP is lossy; a drop here is acceptable).
    return static_cast<int>(data.size());
}

winrt::fire_and_forget DatagramSocketAdapter::DrainTx(std::string key)
{
    co_await winrt::resume_background();

    for (;;)
    {
        std::vector<uint8_t> datagram;
        Endpoint             ep;
        bool                 needStream = false;
        {
            std::lock_guard lk(m_txMutex);
            auto            it = m_tx.find(key);
            if (it == m_tx.end() || it->second.pending.empty() || !m_open.load())
            {
                if (it != m_tx.end())
                    it->second.draining = false;
                co_return;
            }
            datagram   = std::move(it->second.pending.front());
            it->second.pending.pop_front();
            ep         = it->second.endpoint;
            needStream = (it->second.stream == nullptr);
        }

        try
        {
            if (needStream)
            {
                DatagramSocket socket{ nullptr };
                {
                    std::lock_guard lk(m_txMutex);
                    socket = m_socket;
                }
                if (!socket)
                    co_return;

                IOutputStream stream = co_await socket.GetOutputStreamAsync(
                    HostName{ winrt::to_hstring(ep.ip) }, winrt::to_hstring(static_cast<uint32_t>(ep.port)));
                std::lock_guard lk(m_txMutex);
                auto            it = m_tx.find(key);
                if (it == m_tx.end())
                    co_return;
                it->second.stream = stream;
            }

            IOutputStream stream{ nullptr };
            {
                std::lock_guard lk(m_txMutex);
                auto            it = m_tx.find(key);
                if (it == m_tx.end())
                    co_return;
                stream = it->second.stream;
            }

            DataWriter writer{ stream };
            writer.WriteBytes(
                winrt::array_view<uint8_t const>(datagram.data(), datagram.data() + datagram.size()));
            co_await writer.StoreAsync();
            writer.DetachStream(); // keep the cached output stream open for reuse
        }
        catch (...)
        {
            // Drop this datagram and keep draining the rest (UDP semantics).
        }
    }
}

void DatagramSocketAdapter::OnMessage(DatagramSocket const&,
                                      DatagramSocketMessageReceivedEventArgs const& args)
{
    try
    {
        DataReader     reader = args.GetDataReader();
        const uint32_t n      = reader.UnconsumedBufferLength();

        RxDatagram dg;
        dg.bytes.resize(n);
        if (n)
            reader.ReadBytes(winrt::array_view<uint8_t>(dg.bytes.data(), dg.bytes.data() + n));

        if (const HostName host = args.RemoteAddress())
            dg.from.ip = winrt::to_string(host.CanonicalName());
        try
        {
            dg.from.port = static_cast<uint16_t>(std::stoul(winrt::to_string(args.RemotePort())));
        }
        catch (...)
        {
        }

        std::lock_guard lk(m_rxMutex);
        m_rx.push_back(std::move(dg));
    }
    catch (...)
    {
        // Datagram dropped (e.g. a transient reader error after an ICMP). UDP is lossy.
    }
}

int DatagramSocketAdapter::RecvFrom(Endpoint& from, std::span<uint8_t> buffer)
{
    std::lock_guard lk(m_rxMutex);
    if (m_rx.empty())
        return 0;

    RxDatagram   dg = std::move(m_rx.front());
    m_rx.pop_front();

    const size_t n = (std::min)(buffer.size(), dg.bytes.size());
    if (n)
        std::memcpy(buffer.data(), dg.bytes.data(), n);
    from = dg.from;
    return static_cast<int>(n);
}

uint16_t DatagramSocketAdapter::LocalPort() const
{
    return m_localPort.load();
}

} // namespace Neuron::Net
