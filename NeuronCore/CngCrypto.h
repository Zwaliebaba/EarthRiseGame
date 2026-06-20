#pragma once
// Windows CNG (bcrypt.h) implementation of ICrypto — §8.3 / §14.
//
// This is the production crypto backend used by ERServer, NeuronClient and
// ERHeadless on Windows. It is intentionally Windows-only: the .cpp pulls in
// <windows.h>/<bcrypt.h> and links against bcrypt.lib (already referenced by
// ERServer.vcxproj; the client/headless projects must add it too).
//
// The header keeps Windows includes out of the public surface so that code
// that only needs the ICrypto contract (and the rest of NeuronCore, which is
// platform-independent) does not transitively pull in <windows.h>. CNG
// algorithm-provider handles are stored as opaque void* and reinterpreted in
// the .cpp.
//
// Primitives implemented (all per Microsoft CNG docs):
//   ECDH P-256       : BCryptGenerateKeyPair / BCryptSecretAgreement / BCryptDeriveKey
//   ECDSA P-256      : BCryptSignHash / BCryptVerifySignature
//   AES-256-GCM      : BCryptEncrypt/BCryptDecrypt + BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO
//   HKDF             : BCryptKeyDerivation with BCRYPT_HKDF_ALGORITHM (per-direction keys)
//   PBKDF2-SHA512    : BCryptDeriveKeyPBKDF2
//   HMAC-SHA256      : BCryptCreateHash (stateless cookie)
//   RNG              : BCryptGenRandom (BCRYPT_USE_SYSTEM_PREFERRED_RNG)

#include "ICrypto.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Neuron::Net
{

class CngCrypto final : public ICrypto
{
public:
    CngCrypto() = default;
    ~CngCrypto() override;

    CngCrypto(const CngCrypto&) = delete;
    CngCrypto& operator=(const CngCrypto&) = delete;

    // Opens the CNG algorithm providers (ECDH, ECDSA, AES-GCM, HKDF, HMAC, RNG).
    // Must be called once before any other method. Returns false if any
    // provider fails to open (in which case the object is left un-initialized
    // and the destructor cleans up whatever opened).
    [[nodiscard]] bool Initialize();

    // -- Server static signing key (ECDSA P-256, §8.3 pinning) -----------------
    // Loads the server's static ECDSA keypair from a previously exported private
    // BCRYPT_ECCPRIVATE_BLOB, or generates a fresh one if 'existingPrivBlob' is
    // empty. The resulting private blob (for persistence) and the public key are
    // available via GetStaticPrivateBlob()/GetStaticPublicKey().
    [[nodiscard]] bool LoadOrGenerateStaticKey(std::span<const uint8_t> existingPrivBlob);

    // Exportable private blob (BCRYPT_ECCPRIVATE_BLOB) for the static key, so the
    // caller can persist it across restarts. Empty until LoadOrGenerateStaticKey.
    [[nodiscard]] const std::vector<uint8_t>& GetStaticPrivateBlob() const { return m_staticPrivBlob; }

    // The pinned public key (X||Y, 64 bytes) clients ship with the build.
    [[nodiscard]] EcPubKey GetStaticPublicKey() const { return m_staticPub; }

    // -- ICrypto ---------------------------------------------------------------
    EcdhKeyPair GenerateEcdhKeyPair() override;
    bool DeriveSharedSecret(const EcdhKeyPair& ours,
                            const EcPubKey& peerPub,
                            SharedSecret& out) override;

    bool Sign(std::span<const uint8_t> staticPrivBlob,
              std::span<const uint8_t> data,
              EcSignature& out) override;
    bool Verify(const EcPubKey& staticPub,
                std::span<const uint8_t> data,
                const EcSignature& sig) override;

    bool DeriveAeadKeys(const SharedSecret& secret,
                        uint32_t epoch,
                        AeadKey& clientToServer,
                        AeadKey& serverToClient) override;

    bool Encrypt(const AeadKey& key, Direction dir, uint64_t packetNumber,
                 std::span<const uint8_t> aad,
                 std::span<const uint8_t> plaintext,
                 std::vector<uint8_t>& ciphertextWithTag) override;
    bool Decrypt(const AeadKey& key, Direction dir, uint64_t packetNumber,
                 std::span<const uint8_t> aad,
                 std::span<const uint8_t> ciphertextWithTag,
                 std::vector<uint8_t>& plaintext) override;

    Cookie MakeCookie(std::span<const uint8_t> serverSecret,
                      std::span<const uint8_t> clientAddr) override;
    bool   VerifyCookie(std::span<const uint8_t> serverSecret,
                        std::span<const uint8_t> clientAddr,
                        const Cookie& cookie) override;

    bool Pbkdf2(std::span<const uint8_t> password,
                std::span<const uint8_t> salt,
                std::span<const uint8_t> pepper,
                uint32_t iterations,
                std::span<uint8_t> out) override;

    void RandomBytes(std::span<uint8_t> out) override;

    // Tag/nonce layout constants (documented; used by the connection layer too).
    static constexpr size_t kNonceBytes = 12; // GCM standard nonce
    static constexpr size_t kTagBytes   = 16; // GCM tag appended to ciphertext

private:
    // Build the 12-byte GCM nonce: [dir:1][packetNumber BE:8][zero:3].
    static void BuildNonce(Direction dir, uint64_t packetNumber,
                           uint8_t (&nonce)[kNonceBytes]);

    // CNG algorithm-provider handles, stored opaquely (BCRYPT_ALG_HANDLE).
    void* m_ecdhAlg{ nullptr };   // BCRYPT_ECDH_P256_ALGORITHM
    void* m_ecdsaAlg{ nullptr };  // BCRYPT_ECDSA_P256_ALGORITHM
    void* m_aesAlg{ nullptr };    // BCRYPT_AES_ALGORITHM (chain mode GCM set on alg)
    void* m_hkdfAlg{ nullptr };   // BCRYPT_HKDF_ALGORITHM
    void* m_hmacAlg{ nullptr };   // BCRYPT_SHA256_ALGORITHM, HMAC flag
    void* m_rngAlg{ nullptr };    // system-preferred RNG (no provider handle needed)

    bool m_initialized{ false };

    // Server static ECDSA key material.
    std::vector<uint8_t> m_staticPrivBlob; // BCRYPT_ECCPRIVATE_BLOB
    EcPubKey             m_staticPub{};
    bool                 m_hasStaticKey{ false };
};

} // namespace Neuron::Net
