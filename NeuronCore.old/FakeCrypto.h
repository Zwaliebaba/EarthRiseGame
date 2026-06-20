#pragma once
// Deterministic, platform-independent fake ICrypto for unit tests — §16.
//
// PURPOSE: lets the connection / handshake state machine be exercised on any
// platform (including this Linux CI container) WITHOUT Windows CNG. It is pure
// C++ STL: no bcrypt, no OpenSSL, no platform headers.
//
// SECURITY: this fake is NOT cryptographically secure. It exists only to be
// *correct and round-trippable* so that wiring, framing, AEAD semantics
// (tamper/replay/AAD detection), key-derivation distinctness, signature
// accept/reject, cookie accept/reject, and PBKDF2 determinism can all be tested.
// Do not ship it. RandomBytes is a SEEDED PRNG — reproducible, not random.
//
// The construction deliberately mirrors the real AEAD contract:
//   * Encrypt/Decrypt provide genuine authentication: any change to ciphertext,
//     packet number (nonce), or AAD makes Decrypt fail, exactly like AES-GCM.
//   * Sign/Verify accept a valid (data,sig) pair and reject any tampering, so
//     MITM-on-handshake tests are meaningful — but the "signature" only proves
//     wiring, not unforgeability.
//   * DeriveSharedSecret is symmetric so both endpoints agree.

#include "ICrypto.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace Neuron::Net
{

class FakeCrypto final : public ICrypto
{
public:
    FakeCrypto() = default;
    ~FakeCrypto() override = default;

    // ---- ECDH ---------------------------------------------------------------
    // A "keypair": a 32-byte deterministic private scalar; the public key is the
    // private scalar copied into the low 32 bytes of the 64-byte pubkey (a
    // reversible transform). DeriveSharedSecret is made order-independent by
    // hashing the sorted pair of private scalars — so both endpoints, each
    // holding (ourPriv, peerPub==peerPriv), compute the identical secret.
    EcdhKeyPair GenerateEcdhKeyPair() override
    {
        EcdhKeyPair kp;
        kp.privateBlob.resize(32);
        // Distinct per call via an incrementing counter seed.
        uint64_t state = Splitmix64(0xA11CE5EEDull ^ (++s_keypairCounter * 0x9E3779B97F4A7C15ull));
        for (auto& b : kp.privateBlob)
            b = NextByte(state);

        kp.publicKey.fill(0);
        std::memcpy(kp.publicKey.data(), kp.privateBlob.data(), 32);
        // High 32 bytes: a deterministic fingerprint of the private scalar, so a
        // tampered public key is detectable as inconsistent if a test cares.
        Hash32(std::span<const uint8_t>(kp.privateBlob.data(), 32),
               kp.publicKey.data() + 32);
        return kp;
    }

    bool DeriveSharedSecret(const EcdhKeyPair& ours,
                            const EcPubKey& peerPub,
                            SharedSecret& out) override
    {
        if (ours.privateBlob.size() < 32)
            return false;

        const uint8_t* a = ours.privateBlob.data();      // our private scalar
        const uint8_t* b = peerPub.data();               // peer private scalar (== low 32 of pub)

        // Order-independent: hash min(a,b) ‖ max(a,b) so both sides match.
        const bool aFirst = std::memcmp(a, b, 32) <= 0;
        std::array<uint8_t, 64> ordered{};
        std::memcpy(ordered.data(),      aFirst ? a : b, 32);
        std::memcpy(ordered.data() + 32, aFirst ? b : a, 32);

        Hash32(std::span<const uint8_t>(ordered.data(), ordered.size()), out.data());
        return true;
    }

    // ---- ECDSA sign / verify ------------------------------------------------
    // signature[0..31]  = hash(data) folded with a key derived from staticPriv
    // signature[32..63] = hash(staticPrivBlob) — lets Verify recompute the same
    //                     fold from the public key (which embeds that fingerprint).
    //
    // Verify recomputes hash(data) and the fold-key from the pubkey's embedded
    // fingerprint and compares. Altering data OR the signature breaks the match,
    // so MITM tests fail closed. (Wiring proof only — not unforgeable.)
    bool Sign(std::span<const uint8_t> staticPrivBlob,
              std::span<const uint8_t> data,
              EcSignature& out) override
    {
        std::array<uint8_t, 32> foldKey{};
        Hash32(staticPrivBlob, foldKey.data());   // derived from private key

        std::array<uint8_t, 32> dataHash{};
        Hash32(data, dataHash.data());

        out.fill(0);
        for (int i = 0; i < 32; ++i)
            out[i] = static_cast<uint8_t>(dataHash[i] ^ foldKey[i]);
        // Embed the fold-key fingerprint so Verify can recompute it from the
        // public key. The public static key in this fake is the fold-key itself
        // placed in its low 32 bytes (see GetFakeStaticPublicKey below).
        std::memcpy(out.data() + 32, foldKey.data(), 32);
        return true;
    }

    bool Verify(const EcPubKey& staticPub,
                std::span<const uint8_t> data,
                const EcSignature& sig) override
    {
        // The fake's static public key carries the fold-key in its low 32 bytes.
        std::array<uint8_t, 32> foldKey{};
        std::memcpy(foldKey.data(), staticPub.data(), 32);

        // The signature must carry that same fold-key fingerprint (else tampered).
        for (int i = 0; i < 32; ++i)
            if (sig[32 + i] != foldKey[i])
                return false;

        std::array<uint8_t, 32> dataHash{};
        Hash32(data, dataHash.data());

        for (int i = 0; i < 32; ++i)
            if (sig[i] != static_cast<uint8_t>(dataHash[i] ^ foldKey[i]))
                return false;   // data or signature was altered → reject
        return true;
    }

    // Helper for tests: produce a matching (privBlob, pubKey) static-key pair.
    // The public key embeds the fold-key (= hash(privBlob)) so Verify works.
    static void MakeFakeStaticKey(std::vector<uint8_t>& privBlobOut, EcPubKey& pubOut)
    {
        privBlobOut.assign({ 's', 't', 'a', 't', 'i', 'c', '-', 'k', 'e', 'y' });
        pubOut.fill(0);
        Hash32(std::span<const uint8_t>(privBlobOut.data(), privBlobOut.size()),
               pubOut.data()); // low 32 bytes = fold-key = hash(privBlob)
    }

    // ---- AEAD key derivation ------------------------------------------------
    bool DeriveAeadKeys(const SharedSecret& secret,
                        uint32_t epoch,
                        AeadKey& clientToServer,
                        AeadKey& serverToClient) override
    {
        DeriveLabeled(secret, epoch, "er-c2s", clientToServer);
        DeriveLabeled(secret, epoch, "er-s2c", serverToClient);
        return true;
    }

    // ---- AEAD encrypt / decrypt --------------------------------------------
    // ciphertext = plaintext XOR keystream(key,dir,packetNumber)
    // tag(16)    = hash16(key ‖ dir ‖ packetNumber ‖ aad ‖ plaintext)
    // Decrypt recomputes the tag over the recovered plaintext+aad → real AEAD
    // semantics: tamper / wrong nonce / wrong AAD all fail.
    bool Encrypt(const AeadKey& key, Direction dir, uint64_t packetNumber,
                 std::span<const uint8_t> aad,
                 std::span<const uint8_t> plaintext,
                 std::vector<uint8_t>& ciphertextWithTag) override
    {
        ciphertextWithTag.resize(plaintext.size() + 16);
        uint64_t ks = KeystreamState(key, dir, packetNumber);
        for (size_t i = 0; i < plaintext.size(); ++i)
            ciphertextWithTag[i] = static_cast<uint8_t>(plaintext[i] ^ NextByte(ks));

        std::array<uint8_t, 16> tag{};
        ComputeTag(key, dir, packetNumber, aad, plaintext, tag);
        std::memcpy(ciphertextWithTag.data() + plaintext.size(), tag.data(), 16);
        return true;
    }

    bool Decrypt(const AeadKey& key, Direction dir, uint64_t packetNumber,
                 std::span<const uint8_t> aad,
                 std::span<const uint8_t> ciphertextWithTag,
                 std::vector<uint8_t>& plaintext) override
    {
        if (ciphertextWithTag.size() < 16)
            return false;
        const size_t ctLen = ciphertextWithTag.size() - 16;

        plaintext.resize(ctLen);
        uint64_t ks = KeystreamState(key, dir, packetNumber);
        for (size_t i = 0; i < ctLen; ++i)
            plaintext[i] = static_cast<uint8_t>(ciphertextWithTag[i] ^ NextByte(ks));

        std::array<uint8_t, 16> expect{};
        ComputeTag(key, dir, packetNumber, aad,
                   std::span<const uint8_t>(plaintext.data(), plaintext.size()), expect);

        const uint8_t* gotTag = ciphertextWithTag.data() + ctLen;
        unsigned diff = 0;
        for (int i = 0; i < 16; ++i)
            diff |= static_cast<unsigned>(expect[i] ^ gotTag[i]);
        if (diff != 0) {
            plaintext.clear();
            return false; // tamper / wrong packetNumber / wrong AAD
        }
        return true;
    }

    // ---- Cookie -------------------------------------------------------------
    Cookie MakeCookie(std::span<const uint8_t> serverSecret,
                      std::span<const uint8_t> clientAddr) override
    {
        std::vector<uint8_t> buf;
        buf.insert(buf.end(), serverSecret.begin(), serverSecret.end());
        buf.insert(buf.end(), clientAddr.begin(), clientAddr.end());
        Cookie out{};
        Hash32(std::span<const uint8_t>(buf.data(), buf.size()), out.data());
        return out;
    }

    bool VerifyCookie(std::span<const uint8_t> serverSecret,
                      std::span<const uint8_t> clientAddr,
                      const Cookie& cookie) override
    {
        Cookie expected = MakeCookie(serverSecret, clientAddr);
        return expected == cookie;
    }

    // ---- PBKDF2 (deterministic stretch) -------------------------------------
    bool Pbkdf2(std::span<const uint8_t> password,
                std::span<const uint8_t> salt,
                std::span<const uint8_t> pepper,
                uint32_t iterations,
                std::span<uint8_t> out) override
    {
        if (iterations == 0)
            iterations = 1; // keep the fake fast; iteration count still affects result

        std::vector<uint8_t> seed;
        seed.insert(seed.end(), pepper.begin(), pepper.end());
        seed.insert(seed.end(), password.begin(), password.end());
        seed.insert(seed.end(), salt.begin(), salt.end());

        std::array<uint8_t, 32> acc{};
        Hash32(std::span<const uint8_t>(seed.data(), seed.size()), acc.data());

        // Stretch: fold the iteration count into the chain so it influences output.
        uint64_t st = 0;
        std::memcpy(&st, acc.data(), sizeof(st));
        st ^= iterations;
        for (uint32_t i = 0; i < iterations; ++i) {
            st = Splitmix64(st ^ (i * 0x9E3779B97F4A7C15ull));
            // Mix back into the accumulator each round.
            std::array<uint8_t, 32> next{};
            std::array<uint8_t, 40> mix{};
            std::memcpy(mix.data(), acc.data(), 32);
            std::memcpy(mix.data() + 32, &st, 8);
            Hash32(std::span<const uint8_t>(mix.data(), mix.size()), next.data());
            acc = next;
            std::memcpy(&st, acc.data(), sizeof(st));
        }

        // Expand to fill the requested output length.
        for (size_t i = 0; i < out.size(); ++i) {
            if ((i % 32) == 0 && i != 0) {
                std::array<uint8_t, 32> next{};
                Hash32(std::span<const uint8_t>(acc.data(), acc.size()), next.data());
                acc = next;
            }
            out[i] = acc[i % 32];
        }
        return true;
    }

    // ---- Random (SEEDED — reproducible, NOT secure) -------------------------
    void RandomBytes(std::span<uint8_t> out) override
    {
        for (auto& b : out)
            b = NextByte(m_rngState);
    }

private:
    // ---- splitmix64 PRNG / mixing helper ------------------------------------
    static uint64_t Splitmix64(uint64_t x)
    {
        x += 0x9E3779B97F4A7C15ull;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Advance a PRNG state and return one byte.
    static uint8_t NextByte(uint64_t& state)
    {
        state = Splitmix64(state);
        return static_cast<uint8_t>(state >> 56);
    }

    // ---- FNV-1a based 32-byte hash ------------------------------------------
    // Four independent FNV-1a lanes (distinct offset bases) expanded with
    // splitmix64 give a deterministic 32-byte digest. Good enough for test
    // determinism / collision resistance at test scale; NOT a real hash.
    static void Hash32(std::span<const uint8_t> data, uint8_t out[32])
    {
        static constexpr uint64_t kBases[4] = {
            1469598103934665603ull, 1099511628211ull,
            0xCBF29CE484222325ull,  0x100000001B3ull
        };
        uint64_t lanes[4];
        for (int l = 0; l < 4; ++l) {
            uint64_t h = kBases[l] ^ (static_cast<uint64_t>(l) << 32);
            for (uint8_t byte : data) {
                h ^= byte;
                h *= 1099511628211ull;
            }
            // Final avalanche, mixing length so empty vs zero-filled differ.
            lanes[l] = Splitmix64(h ^ (static_cast<uint64_t>(data.size()) << (l * 8)));
        }
        for (int l = 0; l < 4; ++l)
            std::memcpy(out + l * 8, &lanes[l], 8);
    }

    static void Hash16(std::span<const uint8_t> data, uint8_t out[16])
    {
        uint8_t full[32];
        Hash32(data, full);
        std::memcpy(out, full, 16);
    }

    // Derive a labeled 32-byte key from (secret ‖ epoch ‖ label).
    static void DeriveLabeled(const SharedSecret& secret, uint32_t epoch,
                              const char* label, AeadKey& out)
    {
        std::vector<uint8_t> buf;
        buf.insert(buf.end(), secret.begin(), secret.end());
        buf.push_back(static_cast<uint8_t>((epoch >> 24) & 0xFF));
        buf.push_back(static_cast<uint8_t>((epoch >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((epoch >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(epoch & 0xFF));
        for (const char* p = label; *p; ++p)
            buf.push_back(static_cast<uint8_t>(*p));
        Hash32(std::span<const uint8_t>(buf.data(), buf.size()), out.data());
    }

    // Keystream PRNG state seeded from (key, dir, packetNumber). Distinct packet
    // numbers / directions produce distinct keystreams — modelling the GCM nonce.
    static uint64_t KeystreamState(const AeadKey& key, Direction dir, uint64_t pn)
    {
        uint64_t s = 0;
        std::memcpy(&s, key.data(), sizeof(s));
        s ^= Splitmix64(static_cast<uint64_t>(dir) * 0x1000193ull + pn);
        for (size_t i = 8; i < key.size(); i += 8) {
            uint64_t chunk = 0;
            std::memcpy(&chunk, key.data() + i, 8);
            s = Splitmix64(s ^ chunk);
        }
        return Splitmix64(s ^ pn);
    }

    // AEAD tag over (key ‖ dir ‖ packetNumber ‖ aad ‖ plaintext).
    static void ComputeTag(const AeadKey& key, Direction dir, uint64_t pn,
                           std::span<const uint8_t> aad,
                           std::span<const uint8_t> plaintext,
                           std::array<uint8_t, 16>& tag)
    {
        std::vector<uint8_t> buf;
        buf.insert(buf.end(), key.begin(), key.end());
        buf.push_back(static_cast<uint8_t>(dir));
        for (int i = 0; i < 8; ++i)
            buf.push_back(static_cast<uint8_t>((pn >> (8 * (7 - i))) & 0xFF));
        // Length-prefix the AAD so AAD/plaintext boundary is unambiguous.
        const uint32_t aadLen = static_cast<uint32_t>(aad.size());
        buf.push_back(static_cast<uint8_t>((aadLen >> 24) & 0xFF));
        buf.push_back(static_cast<uint8_t>((aadLen >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((aadLen >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(aadLen & 0xFF));
        buf.insert(buf.end(), aad.begin(), aad.end());
        buf.insert(buf.end(), plaintext.begin(), plaintext.end());
        Hash16(std::span<const uint8_t>(buf.data(), buf.size()), tag.data());
    }

    // Seeded RNG state (reproducible). Each instance starts from a fixed seed.
    uint64_t m_rngState{ 0xC0FFEE123456789ull };

    // Counter to give each generated keypair a distinct (but deterministic) key.
    inline static uint64_t s_keypairCounter{ 0 };
};

} // namespace Neuron::Net
