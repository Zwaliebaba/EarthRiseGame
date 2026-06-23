// PBKDF2-HMAC-SHA512 reference tests (M5 area C; §14). Validates the portable
// password-hashing reference against FIPS-180-4 (SHA-512) and RFC-4231 (HMAC-SHA512)
// known answers, plus the PBKDF2 construction (RFC 8018) by its structural identity,
// plus the auth properties §14/M5 need (stable, salt-dependent, wrong-password
// rejected, iteration-cost-sensitive). The Windows CngCrypto PBKDF2 must produce
// byte-identical output to this reference (cross-checked on the agent). §16.2 mirror.

#include "Pbkdf2.h"
#include "TestRunner.h"

#include <array>
#include <span>
#include <string>
#include <vector>

using namespace ertest;
using namespace Neuron::Crypto;

namespace
{
    std::vector<uint8_t> Bytes(const std::string& s) { return { s.begin(), s.end() }; }

    template <size_t N>
    std::string ToHex(const std::array<uint8_t, N>& d)
    {
        static const char* k = "0123456789abcdef";
        std::string out;
        out.reserve(N * 2);
        for (uint8_t b : d) { out.push_back(k[b >> 4]); out.push_back(k[b & 0xF]); }
        return out;
    }
    std::string ToHex(const std::vector<uint8_t>& d)
    {
        static const char* k = "0123456789abcdef";
        std::string out;
        out.reserve(d.size() * 2);
        for (uint8_t b : d) { out.push_back(k[b >> 4]); out.push_back(k[b & 0xF]); }
        return out;
    }
}

ER_TEST(Pbkdf2, Sha512KnownAnswersFips180)
{
    const auto empty = Sha512::Hash({});
    ER_CHECK_EQ(ToHex(empty),
        std::string("cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                    "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"));

    const auto abc = Sha512::Hash(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>("abc"), 3));
    ER_CHECK_EQ(ToHex(abc),
        std::string("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
}

ER_TEST(Pbkdf2, HmacSha512KnownAnswerRfc4231)
{
    // RFC 4231 Test Case 1: key = 0x0b × 20, data = "Hi There".
    std::vector<uint8_t> key(20, 0x0b);
    const auto mac = HmacSha512(key, Bytes("Hi There"));
    ER_CHECK_EQ(ToHex(mac),
        std::string("87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
                    "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854"));
}

ER_TEST(Pbkdf2, Pbkdf2MatchesItsHmacConstructionC1AndC2)
{
    // PBKDF2 built correctly on the (RFC-4231-verified) HMAC: with one 64-byte block,
    //   c=1: T1 = U1 = HMAC(P, S || INT_32_BE(1))
    //   c=2: T1 = U1 XOR U2,  U2 = HMAC(P, U1)
    const auto P = Bytes("password");
    const auto S = Bytes("saltsalt");

    std::vector<uint8_t> saltBlock = S;
    saltBlock.insert(saltBlock.end(), { 0x00, 0x00, 0x00, 0x01 });
    const auto u1 = HmacSha512(P, saltBlock);

    const auto dk1 = Pbkdf2HmacSha512(P, S, /*iter=*/1, /*dkLen=*/64);
    ER_CHECK_EQ(ToHex(dk1), ToHex(u1));

    const auto u2 = HmacSha512(P, u1);
    std::array<uint8_t, 64> expect2{};
    for (size_t i = 0; i < 64; ++i) expect2[i] = u1[i] ^ u2[i];
    const auto dk2 = Pbkdf2HmacSha512(P, S, /*iter=*/2, /*dkLen=*/64);
    ER_CHECK_EQ(ToHex(dk2), ToHex(expect2));
}

ER_TEST(Pbkdf2, AuthPropertiesStableSaltedAndCostSensitive)
{
    const auto P = Bytes("correct horse battery staple");
    const auto salt = Bytes("0123456789abcdef0123456789abcdef");

    // Stable: same inputs → same 64-byte hash (the stored PasswordHash, §14 / schema).
    const auto h1 = Pbkdf2HmacSha512(P, salt, 4096, 64);
    const auto h2 = Pbkdf2HmacSha512(P, salt, 4096, 64);
    ER_CHECK_EQ(h1.size(), size_t{ 64 });
    ER_CHECK_EQ(ToHex(h1), ToHex(h2));

    // Wrong password is rejected (produces a different hash).
    const auto wrong = Pbkdf2HmacSha512(Bytes("Correct horse battery staple"), salt, 4096, 64);
    ER_CHECK(ToHex(wrong) != ToHex(h1));

    // Salt-dependent: a different per-user salt → a different hash (no rainbow reuse).
    const auto salt2 = Bytes("fedcba9876543210fedcba9876543210");
    ER_CHECK(ToHex(Pbkdf2HmacSha512(P, salt2, 4096, 64)) != ToHex(h1));

    // Iteration cost matters: changing the (stored, tunable) cost changes the hash,
    // so the cost can be raised later without ambiguity (§14).
    ER_CHECK(ToHex(Pbkdf2HmacSha512(P, salt, 8192, 64)) != ToHex(h1));
}
