#pragma once
// Crypto abstraction — §8.3 / §14. Contract shared by the connection layer and
// the Windows CNG implementation (CngCrypto in net/CngCrypto.{h,cpp}).
//
// Defining this as an interface keeps NeuronCore's connection logic
// platform-independent and lets tests substitute a deterministic fake.
//
// Primitives (all via Windows CNG in production):
//   - ECDH (P-256) ephemeral key agreement
//   - ECDSA (P-256) server static-key signature over the ephemeral pubkey (pinning)
//   - AES-256-GCM AEAD (nonce = direction-bit ‖ 64-bit packet counter)
//   - PBKDF2-HMAC-SHA512 password hashing (+ server pepper)
//   - HMAC-SHA256 for the stateless connection cookie

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Neuron::Net
{

// Key / buffer sizes.
inline constexpr size_t kEcPubKeyBytes  = 64; // P-256 X||Y
inline constexpr size_t kEcSigBytes     = 64; // P-256 r||s
inline constexpr size_t kAeadKeyBytes   = 32; // AES-256
inline constexpr size_t kSharedSecretBytes = 32;
inline constexpr size_t kCookieBytes    = 32; // HMAC-SHA256
inline constexpr size_t kPwHashBytes    = 64; // PBKDF2-HMAC-SHA512
inline constexpr size_t kPwSaltBytes    = 32;

using EcPubKey      = std::array<uint8_t, kEcPubKeyBytes>;
using EcSignature   = std::array<uint8_t, kEcSigBytes>;
using AeadKey       = std::array<uint8_t, kAeadKeyBytes>;
using SharedSecret  = std::array<uint8_t, kSharedSecretBytes>;
using Cookie        = std::array<uint8_t, kCookieBytes>;

// An ephemeral ECDH keypair handle (opaque; implementation-defined storage).
struct EcdhKeyPair
{
    EcPubKey             publicKey{};
    std::vector<uint8_t> privateBlob; // opaque CNG key blob (impl detail)
};

// AEAD direction: keys/nonces are split so the two directions never collide.
enum class Direction : uint8_t { ClientToServer = 0, ServerToClient = 1 };

class ICrypto
{
public:
    virtual ~ICrypto() = default;

    // -- ECDH (§8.5 step 3) --
    virtual EcdhKeyPair GenerateEcdhKeyPair() = 0;
    // Derive the shared secret from our private key and the peer's public key.
    virtual bool DeriveSharedSecret(const EcdhKeyPair& ours,
                                    const EcPubKey& peerPub,
                                    SharedSecret& out) = 0;

    // -- ECDSA server-key pinning (§8.3) --
    // Server signs its ephemeral pubkey with its static private key; client
    // verifies against the pinned static public key shipped with the build.
    virtual bool Sign(std::span<const uint8_t> staticPrivBlob,
                      std::span<const uint8_t> data,
                      EcSignature& out) = 0;
    virtual bool Verify(const EcPubKey& staticPub,
                        std::span<const uint8_t> data,
                        const EcSignature& sig) = 0;

    // -- Key derivation from shared secret --
    // Derive distinct AEAD keys per direction (HKDF-style) plus a rekey epoch.
    virtual bool DeriveAeadKeys(const SharedSecret& secret,
                                uint32_t epoch,
                                AeadKey& clientToServer,
                                AeadKey& serverToClient) = 0;

    // -- AES-256-GCM AEAD (§8.3) --
    // Nonce is built from (direction, packetNumber); 'aad' is the clear header.
    virtual bool Encrypt(const AeadKey& key, Direction dir, uint64_t packetNumber,
                         std::span<const uint8_t> aad,
                         std::span<const uint8_t> plaintext,
                         std::vector<uint8_t>& ciphertextWithTag) = 0;
    virtual bool Decrypt(const AeadKey& key, Direction dir, uint64_t packetNumber,
                         std::span<const uint8_t> aad,
                         std::span<const uint8_t> ciphertextWithTag,
                         std::vector<uint8_t>& plaintext) = 0;

    // -- Stateless cookie (§8.5 step 1) --
    // cookie = HMAC-SHA256(secret, clientAddr ‖ salt); no per-connection state.
    virtual Cookie MakeCookie(std::span<const uint8_t> serverSecret,
                              std::span<const uint8_t> clientAddr) = 0;
    virtual bool   VerifyCookie(std::span<const uint8_t> serverSecret,
                                std::span<const uint8_t> clientAddr,
                                const Cookie& cookie) = 0;

    // -- Password hashing (§14) --
    virtual bool Pbkdf2(std::span<const uint8_t> password,
                        std::span<const uint8_t> salt,
                        std::span<const uint8_t> pepper,
                        uint32_t iterations,
                        std::span<uint8_t> out) = 0;

    // -- Random --
    virtual void RandomBytes(std::span<uint8_t> out) = 0;
};

// Default rekey threshold: rekey before the 64-bit packet counter approaches
// any risk of wrap (we rekey far earlier, by epoch, well within GCM limits).
inline constexpr uint64_t kRekeyPacketThreshold = (1ull << 48);

} // namespace Neuron::Net
