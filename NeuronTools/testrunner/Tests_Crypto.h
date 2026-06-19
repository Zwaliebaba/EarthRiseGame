#pragma once
// M1a crypto-layer unit tests — exercises the ICrypto contract via the
// deterministic, platform-independent FakeCrypto (no Windows CNG required).
//
// These tests prove the *semantics* the connection/handshake state machine
// relies on: symmetric ECDH agreement, AEAD round-trip + tamper/replay/AAD
// detection, signature accept/reject (MITM), per-direction key distinctness,
// cookie accept/reject, and PBKDF2 determinism. The real CngCrypto is expected
// to satisfy the same contract on Windows.

#include "TestRunner.h"
#include "net/FakeCrypto.h"

#include <cstdint>
#include <vector>

TEST_SUITE(Crypto)
{
    using namespace Neuron::Net;

    TEST_CASE(EcdhAgreementSymmetric) {
        FakeCrypto c;
        EcdhKeyPair a = c.GenerateEcdhKeyPair();
        EcdhKeyPair b = c.GenerateEcdhKeyPair();

        SharedSecret sa{}, sb{};
        REQUIRE(c.DeriveSharedSecret(a, b.publicKey, sa));
        REQUIRE(c.DeriveSharedSecret(b, a.publicKey, sb));
        CHECK(sa == sb);

        // Two distinct keypairs must not collide.
        CHECK(a.privateBlob != b.privateBlob);
    });

    TEST_CASE(AeadRoundTrip) {
        FakeCrypto c;
        AeadKey key{}; c.RandomBytes(key);
        std::vector<uint8_t> aad{0xDE, 0xAD};
        std::vector<uint8_t> plain{1, 2, 3, 4, 5};

        std::vector<uint8_t> ct;
        REQUIRE(c.Encrypt(key, Direction::ClientToServer, 42, aad, plain, ct));
        CHECK_EQ(ct.size(), plain.size() + 16);

        std::vector<uint8_t> out;
        REQUIRE(c.Decrypt(key, Direction::ClientToServer, 42, aad, ct, out));
        CHECK(out == plain);
    });

    TEST_CASE(AeadTamperDetected) {
        FakeCrypto c;
        AeadKey key{}; c.RandomBytes(key);
        std::vector<uint8_t> aad{7};
        std::vector<uint8_t> plain{9, 8, 7, 6};

        std::vector<uint8_t> ct;
        REQUIRE(c.Encrypt(key, Direction::ServerToClient, 1, aad, plain, ct));
        ct[0] ^= 0x01; // flip a ciphertext byte

        std::vector<uint8_t> out;
        CHECK(!c.Decrypt(key, Direction::ServerToClient, 1, aad, ct, out));
    });

    TEST_CASE(AeadWrongPacketNumberFails) {
        FakeCrypto c;
        AeadKey key{}; c.RandomBytes(key);
        std::vector<uint8_t> aad{};
        std::vector<uint8_t> plain{42, 42, 42};

        std::vector<uint8_t> ct;
        REQUIRE(c.Encrypt(key, Direction::ClientToServer, 100, aad, plain, ct));

        std::vector<uint8_t> out;
        // Different packet number → different nonce → tag mismatch (replay guard).
        CHECK(!c.Decrypt(key, Direction::ClientToServer, 101, aad, ct, out));
    });

    TEST_CASE(AeadWrongAadFails) {
        FakeCrypto c;
        AeadKey key{}; c.RandomBytes(key);
        std::vector<uint8_t> aad{1, 2, 3};
        std::vector<uint8_t> plain{55};

        std::vector<uint8_t> ct;
        REQUIRE(c.Encrypt(key, Direction::ServerToClient, 5, aad, plain, ct));

        std::vector<uint8_t> badAad{1, 2, 4}; // header altered
        std::vector<uint8_t> out;
        CHECK(!c.Decrypt(key, Direction::ServerToClient, 5, badAad, ct, out));
    });

    TEST_CASE(SignVerifyRoundTrip) {
        FakeCrypto c;
        std::vector<uint8_t> priv;
        EcPubKey pub{};
        FakeCrypto::MakeFakeStaticKey(priv, pub);

        std::vector<uint8_t> data{0xAB, 0xCD, 0xEF};
        EcSignature sig{};
        REQUIRE(c.Sign(priv, data, sig));
        CHECK(c.Verify(pub, data, sig));
    });

    TEST_CASE(SignVerifyTamperFails) {
        FakeCrypto c;
        std::vector<uint8_t> priv;
        EcPubKey pub{};
        FakeCrypto::MakeFakeStaticKey(priv, pub);

        std::vector<uint8_t> data{1, 2, 3, 4};
        EcSignature sig{};
        REQUIRE(c.Sign(priv, data, sig));

        // Altered data → MITM on the handshake → must fail.
        std::vector<uint8_t> tampered{1, 2, 3, 5};
        CHECK(!c.Verify(pub, tampered, sig));

        // Altered signature → must also fail.
        EcSignature badSig = sig;
        badSig[0] ^= 0x01;
        CHECK(!c.Verify(pub, data, badSig));
    });

    TEST_CASE(DeriveAeadKeysDistinct) {
        FakeCrypto c;
        SharedSecret secret{};
        c.RandomBytes(secret);

        AeadKey c2s{}, s2c{};
        REQUIRE(c.DeriveAeadKeys(secret, 1, c2s, s2c));
        CHECK(c2s != s2c); // directions must differ

        // Determinism: same inputs → same keys.
        AeadKey c2s2{}, s2c2{};
        REQUIRE(c.DeriveAeadKeys(secret, 1, c2s2, s2c2));
        CHECK(c2s == c2s2);
        CHECK(s2c == s2c2);

        // Different epoch → different keys (rekey).
        AeadKey c2sE{}, s2cE{};
        REQUIRE(c.DeriveAeadKeys(secret, 2, c2sE, s2cE));
        CHECK(c2s != c2sE);
    });

    TEST_CASE(CookieRoundTrip) {
        FakeCrypto c;
        std::vector<uint8_t> secret{'s', 'e', 'c', 'r', 'e', 't'};
        std::vector<uint8_t> addr{127, 0, 0, 1, 0x12, 0x34};

        Cookie cookie = c.MakeCookie(secret, addr);
        CHECK(c.VerifyCookie(secret, addr, cookie));

        // Different client address → reject.
        std::vector<uint8_t> otherAddr{10, 0, 0, 1, 0x12, 0x34};
        CHECK(!c.VerifyCookie(secret, otherAddr, cookie));
    });

    TEST_CASE(Pbkdf2Deterministic) {
        FakeCrypto c;
        std::vector<uint8_t> pw{'h', 'u', 'n', 't', 'e', 'r', '2'};
        std::vector<uint8_t> salt(kPwSaltBytes, 0xA5);
        std::vector<uint8_t> pepper{'p', 'e', 'p'};

        std::array<uint8_t, kPwHashBytes> h1{}, h2{}, h3{};
        REQUIRE(c.Pbkdf2(pw, salt, pepper, 1000, h1));
        REQUIRE(c.Pbkdf2(pw, salt, pepper, 1000, h2));
        CHECK(h1 == h2); // same inputs → same hash

        std::vector<uint8_t> salt2(kPwSaltBytes, 0x5A);
        REQUIRE(c.Pbkdf2(pw, salt2, pepper, 1000, h3));
        CHECK(h1 != h3); // different salt → different hash
    });
}
