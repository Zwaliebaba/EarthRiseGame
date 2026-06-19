#pragma once
// SecureChannel — post-handshake encrypted datagram framing (§8.3).
//
// Ties together the AEAD crypto (ICrypto), the monotonic 64-bit packet counter
// (AEAD nonce input), and the replay window. Platform-independent: it depends
// only on the ICrypto interface, so it is exercised in unit tests with FakeCrypto.
//
//   Send:  plaintext payload → build header (AAD) → AEAD encrypt → datagram
//   Recv:  parse header → replay-window check → AEAD decrypt/verify → payload
//
// Rekey: when the outgoing packet counter crosses kRekeyPacketThreshold the
// channel signals the owner to renegotiate keys (epoch++). For M1a the threshold
// is far below any GCM/nonce-wrap risk, so a single epoch suffices in practice.

#include "ICrypto.h"
#include "PacketCodec.h"
#include "Protocol.h"
#include "ReplayWindow.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Neuron::Net
{

class SecureChannel
{
public:
    // 'sendDir' is this endpoint's outgoing direction (C2S for clients,
    // S2C for servers); the receive direction is the opposite.
    SecureChannel(ICrypto* crypto, Direction sendDir,
                  uint64_t connectionToken) noexcept
        : m_crypto(crypto)
        , m_sendDir(sendDir)
        , m_recvDir(sendDir == Direction::ClientToServer ? Direction::ServerToClient
                                                         : Direction::ClientToServer)
        , m_token(connectionToken)
    {
    }

    // Install the per-direction AEAD keys for the current epoch.
    void SetKeys(const AeadKey& sendKey, const AeadKey& recvKey, uint32_t epoch) noexcept
    {
        m_sendKey = sendKey;
        m_recvKey = recvKey;
        m_epoch   = epoch;
        m_hasKeys = true;
    }

    [[nodiscard]] bool HasKeys()       const noexcept { return m_hasKeys; }
    [[nodiscard]] uint64_t SendCounter() const noexcept { return m_sendCounter; }
    [[nodiscard]] uint32_t Epoch()     const noexcept { return m_epoch; }

    // True when the outgoing counter has crossed the rekey threshold.
    [[nodiscard]] bool NeedsRekey() const noexcept
    {
        return m_sendCounter >= kRekeyPacketThreshold;
    }

    // Encrypt a payload into a full datagram (header ‖ AEAD ciphertext+tag).
    // Returns false if keys are missing or encryption fails.
    [[nodiscard]] bool Seal(std::span<const uint8_t> payload, std::vector<uint8_t>& outDatagram)
    {
        if (!m_hasKeys || !m_crypto) return false;

        PacketHeader hdr;
        hdr.protocolId      = kProtocolId;
        hdr.connectionToken = m_token;
        hdr.packetNumber    = m_sendCounter;

        // Header is written in clear and authenticated as AAD.
        outDatagram.clear();
        WritePacketHeader(outDatagram, hdr);
        std::span<const uint8_t> aad{ outDatagram.data(), outDatagram.size() };

        std::vector<uint8_t> ct;
        if (!m_crypto->Encrypt(m_sendKey, m_sendDir, m_sendCounter, aad, payload, ct))
            return false;

        outDatagram.insert(outDatagram.end(), ct.begin(), ct.end());
        ++m_sendCounter;
        return true;
    }

    enum class OpenResult : uint8_t
    {
        Ok,
        BadHeader,
        WrongToken,
        Replay,        // replay-window rejection (R13)
        AuthFailure,   // AEAD tag mismatch / decrypt failed
        NoKeys,
    };

    // Verify + decrypt a received datagram into its plaintext payload.
    OpenResult Open(std::span<const uint8_t> datagram, std::vector<uint8_t>& outPayload)
    {
        if (!m_hasKeys || !m_crypto) return OpenResult::NoKeys;

        PacketHeader hdr;
        size_t off = 0;
        if (!ReadPacketHeader(datagram, hdr, off))
            return OpenResult::BadHeader;
        if (hdr.connectionToken != m_token)
            return OpenResult::WrongToken;

        // Replay-window check BEFORE spending a decrypt (cheap rejection).
        if (!m_replay.CheckAndUpdate(hdr.packetNumber))
            return OpenResult::Replay;

        std::span<const uint8_t> aad{ datagram.data(), PacketHeader::kWireSize };
        std::span<const uint8_t> ct  = datagram.subspan(PacketHeader::kWireSize);

        if (!m_crypto->Decrypt(m_recvKey, m_recvDir, hdr.packetNumber, aad, ct, outPayload))
            return OpenResult::AuthFailure;

        return OpenResult::Ok;
    }

private:
    ICrypto*      m_crypto{ nullptr };
    Direction     m_sendDir;
    Direction     m_recvDir;
    uint64_t      m_token{ 0 };

    AeadKey       m_sendKey{};
    AeadKey       m_recvKey{};
    uint32_t      m_epoch{ 0 };
    bool          m_hasKeys{ false };

    uint64_t      m_sendCounter{ 0 };
    ReplayWindow  m_replay;
};

} // namespace Neuron::Net
