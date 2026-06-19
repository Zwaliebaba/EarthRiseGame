#include "CppUnitTest.h"
#include "net/FakeCrypto.h"

#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron::Net;

TEST_CLASS(CryptoTests)
{
public:
    TEST_METHOD(EcdhAgreementSymmetric)
    {
        FakeCrypto c;
        EcdhKeyPair a = c.GenerateEcdhKeyPair();
        EcdhKeyPair b = c.GenerateEcdhKeyPair();

        SharedSecret sa{}, sb{};
        Assert::IsTrue(c.DeriveSharedSecret(a, b.publicKey, sa));
        Assert::IsTrue(c.DeriveSharedSecret(b, a.publicKey, sb));
        Assert::IsTrue(sa == sb);
        Assert::IsTrue(a.privateBlob != b.privateBlob);
    }

    TEST_METHOD(AeadRoundTrip)
    {
        FakeCrypto c;
        AeadKey key{}; c.RandomBytes(key);
        std::vector<uint8_t> aad{0xDE, 0xAD};
        std::vector<uint8_t> plain{1, 2, 3, 4, 5};

        std::vector<uint8_t> ct;
        Assert::IsTrue(c.Encrypt(key, Direction::ClientToServer, 42, aad, plain, ct));
        Assert::AreEqual(plain.size() + 16, ct.size());

        std::vector<uint8_t> out;
        Assert::IsTrue(c.Decrypt(key, Direction::ClientToServer, 42, aad, ct, out));
        Assert::IsTrue(out == plain);
    }

    TEST_METHOD(AeadTamperDetected)
    {
        FakeCrypto c;
        AeadKey key{}; c.RandomBytes(key);
        std::vector<uint8_t> aad{7};
        std::vector<uint8_t> plain{9, 8, 7, 6};

        std::vector<uint8_t> ct;
        Assert::IsTrue(c.Encrypt(key, Direction::ServerToClient, 1, aad, plain, ct));
        ct[0] ^= 0x01;

        std::vector<uint8_t> out;
        Assert::IsFalse(c.Decrypt(key, Direction::ServerToClient, 1, aad, ct, out));
    }

    TEST_METHOD(AeadWrongPacketNumberFails)
    {
        FakeCrypto c;
        AeadKey key{}; c.RandomBytes(key);
        std::vector<uint8_t> aad{};
        std::vector<uint8_t> plain{42, 42, 42};

        std::vector<uint8_t> ct;
        Assert::IsTrue(c.Encrypt(key, Direction::ClientToServer, 100, aad, plain, ct));

        std::vector<uint8_t> out;
        Assert::IsFalse(c.Decrypt(key, Direction::ClientToServer, 101, aad, ct, out));
    }

    TEST_METHOD(AeadWrongAadFails)
    {
        FakeCrypto c;
        AeadKey key{}; c.RandomBytes(key);
        std::vector<uint8_t> aad{1, 2, 3};
        std::vector<uint8_t> plain{55};

        std::vector<uint8_t> ct;
        Assert::IsTrue(c.Encrypt(key, Direction::ServerToClient, 5, aad, plain, ct));

        std::vector<uint8_t> badAad{1, 2, 4};
        std::vector<uint8_t> out;
        Assert::IsFalse(c.Decrypt(key, Direction::ServerToClient, 5, badAad, ct, out));
    }

    TEST_METHOD(SignVerifyRoundTrip)
    {
        FakeCrypto c;
        std::vector<uint8_t> priv;
        EcPubKey pub{};
        FakeCrypto::MakeFakeStaticKey(priv, pub);

        std::vector<uint8_t> data{0xAB, 0xCD, 0xEF};
        EcSignature sig{};
        Assert::IsTrue(c.Sign(priv, data, sig));
        Assert::IsTrue(c.Verify(pub, data, sig));
    }

    TEST_METHOD(SignVerifyTamperFails)
    {
        FakeCrypto c;
        std::vector<uint8_t> priv;
        EcPubKey pub{};
        FakeCrypto::MakeFakeStaticKey(priv, pub);

        std::vector<uint8_t> data{1, 2, 3, 4};
        EcSignature sig{};
        Assert::IsTrue(c.Sign(priv, data, sig));

        std::vector<uint8_t> tampered{1, 2, 3, 5};
        Assert::IsFalse(c.Verify(pub, tampered, sig));

        EcSignature badSig = sig;
        badSig[0] ^= 0x01;
        Assert::IsFalse(c.Verify(pub, data, badSig));
    }

    TEST_METHOD(DeriveAeadKeysDistinct)
    {
        FakeCrypto c;
        SharedSecret secret{};
        c.RandomBytes(secret);

        AeadKey c2s{}, s2c{};
        Assert::IsTrue(c.DeriveAeadKeys(secret, 1, c2s, s2c));
        Assert::IsTrue(c2s != s2c);

        AeadKey c2s2{}, s2c2{};
        Assert::IsTrue(c.DeriveAeadKeys(secret, 1, c2s2, s2c2));
        Assert::IsTrue(c2s == c2s2);
        Assert::IsTrue(s2c == s2c2);

        AeadKey c2sE{}, s2cE{};
        Assert::IsTrue(c.DeriveAeadKeys(secret, 2, c2sE, s2cE));
        Assert::IsTrue(c2s != c2sE);
    }

    TEST_METHOD(CookieRoundTrip)
    {
        FakeCrypto c;
        std::vector<uint8_t> secret{'s', 'e', 'c', 'r', 'e', 't'};
        std::vector<uint8_t> addr{127, 0, 0, 1, 0x12, 0x34};

        Cookie cookie = c.MakeCookie(secret, addr);
        Assert::IsTrue(c.VerifyCookie(secret, addr, cookie));

        std::vector<uint8_t> otherAddr{10, 0, 0, 1, 0x12, 0x34};
        Assert::IsFalse(c.VerifyCookie(secret, otherAddr, cookie));
    }

    TEST_METHOD(Pbkdf2Deterministic)
    {
        FakeCrypto c;
        std::vector<uint8_t> pw{'h', 'u', 'n', 't', 'e', 'r', '2'};
        std::vector<uint8_t> salt(kPwSaltBytes, 0xA5);
        std::vector<uint8_t> pepper{'p', 'e', 'p'};

        std::array<uint8_t, kPwHashBytes> h1{}, h2{}, h3{};
        Assert::IsTrue(c.Pbkdf2(pw, salt, pepper, 1000, h1));
        Assert::IsTrue(c.Pbkdf2(pw, salt, pepper, 1000, h2));
        Assert::IsTrue(h1 == h2);

        std::vector<uint8_t> salt2(kPwSaltBytes, 0x5A);
        Assert::IsTrue(c.Pbkdf2(pw, salt2, pepper, 1000, h3));
        Assert::IsTrue(h1 != h3);
    }
};
