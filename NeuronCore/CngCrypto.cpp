// Windows CNG (bcrypt.h) implementation of ICrypto — §8.3 / §14.
//
// LINK NOTE: requires bcrypt.lib. ERServer.vcxproj already links it; the client
// and ERHeadless projects must add <AdditionalDependencies>bcrypt.lib</...> too.
//
// This file is Windows-only and will NOT compile on Linux/macOS — that is by
// design (the cross-platform connection layer is tested against FakeCrypto).
//
// API references (Microsoft CNG / bcrypt):
//   - BCryptOpenAlgorithmProvider / BCryptCloseAlgorithmProvider
//   - BCryptGenerateKeyPair / BCryptFinalizeKeyPair / BCryptExportKey /
//     BCryptImportKeyPair  (ECDH + ECDSA, BCRYPT_ECCPUBLIC_BLOB / PRIVATE_BLOB)
//   - BCryptSecretAgreement / BCryptDeriveKey (BCRYPT_KDF_RAW_SECRET)
//   - BCryptSignHash / BCryptVerifySignature
//   - BCryptSetProperty(BCRYPT_CHAINING_MODE = BCRYPT_CHAIN_MODE_GCM)
//   - BCryptEncrypt / BCryptDecrypt + BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO
//   - BCryptKeyDerivation with BCRYPT_HKDF_ALGORITHM (HKDF, per-direction keys)
//   - BCryptDeriveKeyPBKDF2
//   - BCryptCreateHash (HMAC-SHA256 for the cookie)
//   - BCryptGenRandom (BCRYPT_USE_SYSTEM_PREFERRED_RNG)
#include "pch.h"
#include "CngCrypto.h"
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace Neuron::Net
{
namespace
{

// Convenience reinterpretations of the opaque void* handles.
inline BCRYPT_ALG_HANDLE AsAlg(void* p) { return reinterpret_cast<BCRYPT_ALG_HANDLE>(p); }

// HKDF info labels per direction (§8.3 key derivation). Distinct labels ensure
// the two directional keys never coincide even from the same shared secret.
constexpr unsigned char kInfoC2S[] = "er-c2s";
constexpr unsigned char kInfoS2C[] = "er-s2c";

// ECC blob magics for P-256 (defined in bcrypt.h as BCRYPT_ECDH_PUBLIC_P256_MAGIC
// etc.). We re-use them via the header constants; declared here for clarity.

// Pack a raw X||Y (64 bytes) public key into a BCRYPT_ECCPUBLIC_BLOB for import.
// 'magic' selects ECDH vs ECDSA P-256.
std::vector<uint8_t> MakeEccPublicBlob(const EcPubKey& pub, ULONG magic)
{
    std::vector<uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB) + kEcPubKeyBytes);
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    hdr->dwMagic = magic;
    hdr->cbKey   = 32; // P-256 field element size
    std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), pub.data(), kEcPubKeyBytes);
    return blob;
}

// Extract X||Y (64 bytes) from an exported BCRYPT_ECCPUBLIC_BLOB.
bool ExtractPubFromBlob(const std::vector<uint8_t>& blob, EcPubKey& out)
{
    if (blob.size() < sizeof(BCRYPT_ECCKEY_BLOB) + kEcPubKeyBytes)
        return false;
    std::memcpy(out.data(), blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), kEcPubKeyBytes);
    return true;
}

// SHA-256 of 'data'. Used to hash the signed message down to a 32-byte digest
// before ECDSA (CNG's BCryptSignHash signs an arbitrary digest, so we provide
// the digest of the ephemeral pubkey bytes).
bool Sha256(std::span<const uint8_t> data, uint8_t out[32])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        return false;

    bool ok = false;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (NT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0))) {
        if (NT_SUCCESS(BCryptHashData(h, const_cast<PUCHAR>(data.data()),
                                      static_cast<ULONG>(data.size()), 0)) &&
            NT_SUCCESS(BCryptFinishHash(h, out, 32, 0))) {
            ok = true;
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

} // namespace

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

bool CngCrypto::Initialize()
{
    if (m_initialized)
        return true;

    BCRYPT_ALG_HANDLE alg = nullptr;

    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0)))
        return false;
    m_ecdhAlg = alg;

    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)))
        return false;
    m_ecdsaAlg = alg;

    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)))
        return false;
    m_aesAlg = alg;
    // Set GCM chaining mode once on the algorithm provider; key objects inherit it.
    if (!NT_SUCCESS(BCryptSetProperty(AsAlg(m_aesAlg), BCRYPT_CHAINING_MODE,
                                      reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                      sizeof(BCRYPT_CHAIN_MODE_GCM), 0)))
        return false;

    // HKDF as a generic-key-derivation algorithm (BCryptKeyDerivation path).
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_HKDF_ALGORITHM, nullptr, 0)))
        return false;
    m_hkdfAlg = alg;

    // HMAC-SHA256 for the stateless cookie.
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                                BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        return false;
    m_hmacAlg = alg;

    m_initialized = true;
    return true;
}

CngCrypto::~CngCrypto()
{
    if (m_ecdhAlg)  BCryptCloseAlgorithmProvider(AsAlg(m_ecdhAlg), 0);
    if (m_ecdsaAlg) BCryptCloseAlgorithmProvider(AsAlg(m_ecdsaAlg), 0);
    if (m_aesAlg)   BCryptCloseAlgorithmProvider(AsAlg(m_aesAlg), 0);
    if (m_hkdfAlg)  BCryptCloseAlgorithmProvider(AsAlg(m_hkdfAlg), 0);
    if (m_hmacAlg)  BCryptCloseAlgorithmProvider(AsAlg(m_hmacAlg), 0);
}

// -----------------------------------------------------------------------------
// Static signing key (ECDSA P-256)
// -----------------------------------------------------------------------------

bool CngCrypto::LoadOrGenerateStaticKey(std::span<const uint8_t> existingPrivBlob)
{
    if (!m_initialized)
        return false;

    BCRYPT_KEY_HANDLE key = nullptr;

    if (!existingPrivBlob.empty()) {
        // Import the persisted BCRYPT_ECCPRIVATE_BLOB.
        if (!NT_SUCCESS(BCryptImportKeyPair(AsAlg(m_ecdsaAlg), nullptr,
                                            BCRYPT_ECCPRIVATE_BLOB, &key,
                                            const_cast<PUCHAR>(existingPrivBlob.data()),
                                            static_cast<ULONG>(existingPrivBlob.size()), 0)))
            return false;
        m_staticPrivBlob.assign(existingPrivBlob.begin(), existingPrivBlob.end());
    } else {
        // Generate a fresh static keypair.
        if (!NT_SUCCESS(BCryptGenerateKeyPair(AsAlg(m_ecdsaAlg), &key, 256, 0)))
            return false;
        if (!NT_SUCCESS(BCryptFinalizeKeyPair(key, 0))) {
            BCryptDestroyKey(key);
            return false;
        }
        // Export the private blob for persistence.
        ULONG cb = 0;
        if (!NT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB,
                                        nullptr, 0, &cb, 0))) {
            BCryptDestroyKey(key);
            return false;
        }
        m_staticPrivBlob.resize(cb);
        if (!NT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB,
                                        m_staticPrivBlob.data(), cb, &cb, 0))) {
            BCryptDestroyKey(key);
            return false;
        }
    }

    // Export the public blob to capture the pinned public key (X||Y).
    ULONG cbPub = 0;
    bool ok = false;
    if (NT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                   nullptr, 0, &cbPub, 0))) {
        std::vector<uint8_t> pubBlob(cbPub);
        if (NT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                       pubBlob.data(), cbPub, &cbPub, 0))) {
            ok = ExtractPubFromBlob(pubBlob, m_staticPub);
        }
    }
    BCryptDestroyKey(key);

    m_hasStaticKey = ok;
    return ok;
}

// -----------------------------------------------------------------------------
// ECDH
// -----------------------------------------------------------------------------

EcdhKeyPair CngCrypto::GenerateEcdhKeyPair()
{
    EcdhKeyPair result;
    if (!m_initialized)
        return result;

    BCRYPT_KEY_HANDLE key = nullptr;
    if (!NT_SUCCESS(BCryptGenerateKeyPair(AsAlg(m_ecdhAlg), &key, 256, 0)))
        return result;
    if (!NT_SUCCESS(BCryptFinalizeKeyPair(key, 0))) {
        BCryptDestroyKey(key);
        return result;
    }

    // Export private blob (opaque, kept in EcdhKeyPair::privateBlob).
    ULONG cb = 0;
    if (NT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &cb, 0))) {
        result.privateBlob.resize(cb);
        if (!NT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB,
                                        result.privateBlob.data(), cb, &cb, 0))) {
            result.privateBlob.clear();
        }
    }

    // Export public blob → X||Y.
    ULONG cbPub = 0;
    if (NT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &cbPub, 0))) {
        std::vector<uint8_t> pubBlob(cbPub);
        if (NT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                       pubBlob.data(), cbPub, &cbPub, 0))) {
            ExtractPubFromBlob(pubBlob, result.publicKey);
        }
    }

    BCryptDestroyKey(key);
    return result;
}

bool CngCrypto::DeriveSharedSecret(const EcdhKeyPair& ours,
                                   const EcPubKey& peerPub,
                                   SharedSecret& out)
{
    if (!m_initialized || ours.privateBlob.empty())
        return false;

    BCRYPT_KEY_HANDLE priv = nullptr;
    BCRYPT_KEY_HANDLE peer = nullptr;
    BCRYPT_SECRET_HANDLE secret = nullptr;
    bool ok = false;

    // Import our private key.
    if (!NT_SUCCESS(BCryptImportKeyPair(AsAlg(m_ecdhAlg), nullptr, BCRYPT_ECCPRIVATE_BLOB,
                                        &priv, const_cast<PUCHAR>(ours.privateBlob.data()),
                                        static_cast<ULONG>(ours.privateBlob.size()), 0)))
        goto cleanup;

    // Import the peer's public key (build a BCRYPT_ECCPUBLIC_BLOB).
    {
        auto blob = MakeEccPublicBlob(peerPub, BCRYPT_ECDH_PUBLIC_P256_MAGIC);
        if (!NT_SUCCESS(BCryptImportKeyPair(AsAlg(m_ecdhAlg), nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                            &peer, blob.data(),
                                            static_cast<ULONG>(blob.size()), 0)))
            goto cleanup;
    }

    if (!NT_SUCCESS(BCryptSecretAgreement(priv, peer, &secret, 0)))
        goto cleanup;

    // Pull the raw secret. NOTE: BCRYPT_KDF_RAW_SECRET returns the X coordinate
    // in LITTLE-ENDIAN. That is fine here because we treat it as opaque key
    // material fed straight into HKDF; both peers see the identical 32 bytes.
    {
        ULONG cb = 0;
        if (!NT_SUCCESS(BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr,
                                        nullptr, 0, &cb, 0)))
            goto cleanup;
        if (cb != kSharedSecretBytes)
            goto cleanup; // P-256 raw secret must be 32 bytes
        if (!NT_SUCCESS(BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr,
                                        out.data(), static_cast<ULONG>(out.size()), &cb, 0)))
            goto cleanup;
        ok = true;
    }

cleanup:
    if (secret) BCryptDestroySecret(secret);
    if (peer)   BCryptDestroyKey(peer);
    if (priv)   BCryptDestroyKey(priv);
    return ok;
}

// -----------------------------------------------------------------------------
// ECDSA sign / verify (server static-key pinning)
// -----------------------------------------------------------------------------

bool CngCrypto::Sign(std::span<const uint8_t> staticPrivBlob,
                     std::span<const uint8_t> data,
                     EcSignature& out)
{
    if (!m_initialized || staticPrivBlob.empty())
        return false;

    uint8_t digest[32];
    if (!Sha256(data, digest))
        return false;

    BCRYPT_KEY_HANDLE key = nullptr;
    if (!NT_SUCCESS(BCryptImportKeyPair(AsAlg(m_ecdsaAlg), nullptr, BCRYPT_ECCPRIVATE_BLOB,
                                        &key, const_cast<PUCHAR>(staticPrivBlob.data()),
                                        static_cast<ULONG>(staticPrivBlob.size()), 0)))
        return false;

    bool ok = false;
    ULONG cb = 0;
    // ECDSA P-256 signature is r||s, 64 bytes — exactly kEcSigBytes.
    if (NT_SUCCESS(BCryptSignHash(key, nullptr, digest, sizeof(digest),
                                  out.data(), static_cast<ULONG>(out.size()), &cb, 0))) {
        ok = (cb == kEcSigBytes);
    }
    BCryptDestroyKey(key);
    return ok;
}

bool CngCrypto::Verify(const EcPubKey& staticPub,
                       std::span<const uint8_t> data,
                       const EcSignature& sig)
{
    if (!m_initialized)
        return false;

    // Dev/test convention (see EarthRise client `m_pinnedPub{}`): an all-zero
    // pinned key means "no key pinned" — accept without verifying rather than
    // fail importing an invalid (0,0) EC point. Production builds ship a real
    // pinned public key, so this path is never taken there.
    bool allZero = true;
    for (uint8_t b : staticPub)
        if (b != 0) { allZero = false; break; }
    if (allZero)
        return true;

    uint8_t digest[32];
    if (!Sha256(data, digest))
        return false;

    auto blob = MakeEccPublicBlob(staticPub, BCRYPT_ECDSA_PUBLIC_P256_MAGIC);
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!NT_SUCCESS(BCryptImportKeyPair(AsAlg(m_ecdsaAlg), nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                        &key, blob.data(),
                                        static_cast<ULONG>(blob.size()), 0)))
        return false;

    NTSTATUS st = BCryptVerifySignature(key, nullptr, digest, sizeof(digest),
                                        const_cast<PUCHAR>(sig.data()),
                                        static_cast<ULONG>(sig.size()), 0);
    BCryptDestroyKey(key);
    return NT_SUCCESS(st); // STATUS_INVALID_SIGNATURE on tamper → false
}

// -----------------------------------------------------------------------------
// Key derivation (HKDF) — distinct AEAD keys per direction, mixing the epoch
// -----------------------------------------------------------------------------

bool CngCrypto::DeriveAeadKeys(const SharedSecret& secret,
                               uint32_t epoch,
                               AeadKey& clientToServer,
                               AeadKey& serverToClient)
{
    if (!m_initialized)
        return false;

    // The HKDF salt mixes in the epoch (big-endian) so each rekey yields fresh
    // keys even from the same ECDH secret. The per-direction "info" label keeps
    // the two directional keys distinct.
    uint8_t saltBytes[4] = {
        static_cast<uint8_t>((epoch >> 24) & 0xFF),
        static_cast<uint8_t>((epoch >> 16) & 0xFF),
        static_cast<uint8_t>((epoch >> 8) & 0xFF),
        static_cast<uint8_t>(epoch & 0xFF),
    };

    // CNG HKDF requires, on the key object: (1) BCRYPT_HKDF_HASH_ALGORITHM set
    // before derivation, and (2) HKDF-Extract driven via the
    // BCRYPT_HKDF_SALT_AND_FINALIZE property (the salt is NOT a KDF_SALT buffer
    // passed to BCryptKeyDerivation — that buffer is for other KDFs). Expand
    // (HKDF-Expand) then runs via BCryptKeyDerivation with KDF_HKDF_INFO. We
    // rebuild the key per direction so each gets a clean extract+expand.
    auto deriveOne = [&](const unsigned char* info, ULONG infoLen, AeadKey& key) -> bool {
        BCRYPT_KEY_HANDLE hkdf = nullptr;
        if (!NT_SUCCESS(BCryptGenerateSymmetricKey(AsAlg(m_hkdfAlg), &hkdf, nullptr, 0,
                                                   const_cast<PUCHAR>(secret.data()),
                                                   static_cast<ULONG>(secret.size()), 0)))
            return false;

        bool ok = false;
        if (NT_SUCCESS(BCryptSetProperty(
                hkdf, BCRYPT_HKDF_HASH_ALGORITHM,
                reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_SHA256_ALGORITHM)),
                sizeof(BCRYPT_SHA256_ALGORITHM), 0)) &&
            NT_SUCCESS(BCryptSetProperty(hkdf, BCRYPT_HKDF_SALT_AND_FINALIZE,
                                         saltBytes, sizeof(saltBytes), 0)))
        {
            BCryptBuffer params[] = {
                { infoLen, KDF_HKDF_INFO, const_cast<PUCHAR>(info) },
            };
            BCryptBufferDesc desc{ BCRYPTBUFFER_VERSION, 1, params };

            ULONG cb = 0;
            NTSTATUS st = BCryptKeyDerivation(hkdf, &desc, key.data(),
                                              static_cast<ULONG>(key.size()), &cb, 0);
            ok = NT_SUCCESS(st) && cb == key.size();
        }

        BCryptDestroyKey(hkdf);
        return ok;
    };

    // -1 on the length to drop the terminating NUL from the string literal.
    return deriveOne(kInfoC2S, sizeof(kInfoC2S) - 1, clientToServer)
        && deriveOne(kInfoS2C, sizeof(kInfoS2C) - 1, serverToClient);
}

// -----------------------------------------------------------------------------
// AES-256-GCM AEAD
// -----------------------------------------------------------------------------

void CngCrypto::BuildNonce(Direction dir, uint64_t packetNumber,
                           uint8_t (&nonce)[kNonceBytes])
{
    // Layout: [dir:1][packetNumber big-endian:8][zero:3] = 12 bytes.
    std::memset(nonce, 0, kNonceBytes);
    nonce[0] = static_cast<uint8_t>(dir);
    for (int i = 0; i < 8; ++i)
        nonce[1 + i] = static_cast<uint8_t>((packetNumber >> (8 * (7 - i))) & 0xFF);
    // nonce[9..11] remain zero.
}

bool CngCrypto::Encrypt(const AeadKey& key, Direction dir, uint64_t packetNumber,
                        std::span<const uint8_t> aad,
                        std::span<const uint8_t> plaintext,
                        std::vector<uint8_t>& ciphertextWithTag)
{
    if (!m_initialized)
        return false;

    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (!NT_SUCCESS(BCryptGenerateSymmetricKey(AsAlg(m_aesAlg), &hKey, nullptr, 0,
                                               const_cast<PUCHAR>(key.data()),
                                               static_cast<ULONG>(key.size()), 0)))
        return false;

    uint8_t nonce[kNonceBytes];
    BuildNonce(dir, packetNumber, nonce);

    uint8_t tag[kTagBytes] = {};
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce      = nonce;
    info.cbNonce      = kNonceBytes;
    info.pbAuthData   = aad.empty() ? nullptr : const_cast<PUCHAR>(aad.data());
    info.cbAuthData   = static_cast<ULONG>(aad.size());
    info.pbTag        = tag;
    info.cbTag        = kTagBytes;
    // No chaining across calls (single-shot), so MacContext / chaining fields stay null.

    bool ok = false;
    ciphertextWithTag.assign(plaintext.size() + kTagBytes, 0);

    ULONG cbResult = 0;
    NTSTATUS st = BCryptEncrypt(hKey,
                                plaintext.empty() ? nullptr : const_cast<PUCHAR>(plaintext.data()),
                                static_cast<ULONG>(plaintext.size()),
                                &info, nullptr, 0,
                                ciphertextWithTag.data(),
                                static_cast<ULONG>(plaintext.size()),
                                &cbResult, 0);
    if (NT_SUCCESS(st)) {
        // Append the authentication tag after the ciphertext.
        std::memcpy(ciphertextWithTag.data() + plaintext.size(), tag, kTagBytes);
        ok = true;
    } else {
        ciphertextWithTag.clear();
    }

    BCryptDestroyKey(hKey);
    return ok;
}

bool CngCrypto::Decrypt(const AeadKey& key, Direction dir, uint64_t packetNumber,
                        std::span<const uint8_t> aad,
                        std::span<const uint8_t> ciphertextWithTag,
                        std::vector<uint8_t>& plaintext)
{
    if (!m_initialized || ciphertextWithTag.size() < kTagBytes)
        return false;

    const size_t ctLen = ciphertextWithTag.size() - kTagBytes;
    const uint8_t* ct  = ciphertextWithTag.data();
    const uint8_t* tag = ciphertextWithTag.data() + ctLen;

    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (!NT_SUCCESS(BCryptGenerateSymmetricKey(AsAlg(m_aesAlg), &hKey, nullptr, 0,
                                               const_cast<PUCHAR>(key.data()),
                                               static_cast<ULONG>(key.size()), 0)))
        return false;

    uint8_t nonce[kNonceBytes];
    BuildNonce(dir, packetNumber, nonce);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce      = nonce;
    info.cbNonce      = kNonceBytes;
    info.pbAuthData   = aad.empty() ? nullptr : const_cast<PUCHAR>(aad.data());
    info.cbAuthData   = static_cast<ULONG>(aad.size());
    info.pbTag        = const_cast<PUCHAR>(tag);
    info.cbTag        = kTagBytes;

    plaintext.assign(ctLen, 0);
    ULONG cbResult = 0;
    NTSTATUS st = BCryptDecrypt(hKey,
                               ctLen ? const_cast<PUCHAR>(ct) : nullptr,
                               static_cast<ULONG>(ctLen),
                               &info, nullptr, 0,
                               ctLen ? plaintext.data() : nullptr,
                               static_cast<ULONG>(ctLen),
                               &cbResult, 0);
    BCryptDestroyKey(hKey);

    // STATUS_AUTH_TAG_MISMATCH on tamper/wrong nonce/wrong AAD → false.
    if (!NT_SUCCESS(st)) {
        plaintext.clear();
        return false;
    }
    plaintext.resize(cbResult);
    return true;
}

// -----------------------------------------------------------------------------
// Stateless cookie — HMAC-SHA256(serverSecret, clientAddr ‖ salt)
// -----------------------------------------------------------------------------
//
// SALT ROTATION: for M1a a fixed in-memory server secret is fine. In production
// the serverSecret (or an appended rotating salt) rotates on a timer server-side,
// so cookies naturally expire: VerifyCookie checks the current and previous salt
// generation, anything older is rejected. Here we HMAC just (secret ‖ addr); the
// caller rotates the secret it passes in.

Cookie CngCrypto::MakeCookie(std::span<const uint8_t> serverSecret,
                             std::span<const uint8_t> clientAddr)
{
    Cookie out{};
    if (!m_initialized)
        return out;

    BCRYPT_HASH_HANDLE h = nullptr;
    if (!NT_SUCCESS(BCryptCreateHash(AsAlg(m_hmacAlg), &h, nullptr, 0,
                                     const_cast<PUCHAR>(serverSecret.data()),
                                     static_cast<ULONG>(serverSecret.size()), 0)))
        return out;

    if (NT_SUCCESS(BCryptHashData(h, const_cast<PUCHAR>(clientAddr.data()),
                                  static_cast<ULONG>(clientAddr.size()), 0))) {
        BCryptFinishHash(h, out.data(), static_cast<ULONG>(out.size()), 0);
    }
    BCryptDestroyHash(h);
    return out;
}

bool CngCrypto::VerifyCookie(std::span<const uint8_t> serverSecret,
                             std::span<const uint8_t> clientAddr,
                             const Cookie& cookie)
{
    Cookie expected = MakeCookie(serverSecret, clientAddr);
    // Constant-time comparison to avoid leaking via timing.
    unsigned diff = 0;
    for (size_t i = 0; i < kCookieBytes; ++i)
        diff |= static_cast<unsigned>(expected[i] ^ cookie[i]);
    return diff == 0;
}

// -----------------------------------------------------------------------------
// PBKDF2-HMAC-SHA512 (+ pepper)
// -----------------------------------------------------------------------------
//
// The pepper is a server-side secret mixed into the password input. We prepend
// the pepper to the password (pepper ‖ password) before stretching. (An HMAC
// pre-mix is equally valid; documented choice.)

bool CngCrypto::Pbkdf2(std::span<const uint8_t> password,
                       std::span<const uint8_t> salt,
                       std::span<const uint8_t> pepper,
                       uint32_t iterations,
                       std::span<uint8_t> out)
{
    if (iterations == 0)
        iterations = 200000; // §14 default

    std::vector<uint8_t> pwInput;
    pwInput.reserve(pepper.size() + password.size());
    pwInput.insert(pwInput.end(), pepper.begin(), pepper.end());
    pwInput.insert(pwInput.end(), password.begin(), password.end());

    // BCRYPT_SHA512_ALG_HANDLE is a pseudo-handle (Win10+) — no open/close needed.
    NTSTATUS st = BCryptDeriveKeyPBKDF2(BCRYPT_HMAC_SHA512_ALG_HANDLE,
                                        pwInput.data(), static_cast<ULONG>(pwInput.size()),
                                        const_cast<PUCHAR>(salt.data()),
                                        static_cast<ULONG>(salt.size()),
                                        iterations,
                                        out.data(), static_cast<ULONG>(out.size()), 0);
    return NT_SUCCESS(st);
}

// -----------------------------------------------------------------------------
// Random
// -----------------------------------------------------------------------------

void CngCrypto::RandomBytes(std::span<uint8_t> out)
{
    if (out.empty())
        return;
    BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

} // namespace Neuron::Net
