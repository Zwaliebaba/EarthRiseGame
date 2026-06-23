#pragma once
// Pbkdf2.h — portable PBKDF2-HMAC-SHA512 reference (M5 area C, §14).
//
// §14 chooses **PBKDF2-HMAC-SHA512** for password hashing (CNG has no Argon2; this
// is the deliberate MS-only choice). Production hashing runs through **CngCrypto**
// (Windows CNG, `BCryptDeriveKeyPBKDF2`). This header is the **portable reference**
// (mirrored on the Linux testrunner, §16.2): it lets the algorithm be checked against
// FIPS-180-4 / RFC-4231 known-answer vectors here, and lets the Windows CngCrypto
// PBKDF2 be cross-checked to produce **byte-identical** output for the same
// (password, salt, iterations, dkLen) — so the two implementations can never silently
// diverge. The server pepper is applied by the caller (appended to the password,
// never stored, §14); this is the raw KDF only.
//
// Self-contained (no CNG, no third-party): SHA-512 → HMAC-SHA512 → PBKDF2.

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace Neuron::Crypto
{

// --- SHA-512 (FIPS 180-4) --------------------------------------------------------
class Sha512
{
public:
    static constexpr size_t kDigest = 64;
    static constexpr size_t kBlock  = 128;

    [[nodiscard]] static std::array<uint8_t, kDigest> Hash(std::span<const uint8_t> msg)
    {
        std::array<uint64_t, 8> h{ 0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull,
                                   0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
                                   0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
                                   0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull };

        // Padded message length: msg + 0x80 + zeros + 16-byte (128-bit) bit length.
        const uint64_t bitLen = static_cast<uint64_t>(msg.size()) * 8ull;
        std::vector<uint8_t> data(msg.begin(), msg.end());
        data.push_back(0x80);
        while (data.size() % kBlock != 112) data.push_back(0x00);
        for (int i = 0; i < 8; ++i) data.push_back(0x00);          // high 64 bits of length = 0
        for (int i = 7; i >= 0; --i) data.push_back(static_cast<uint8_t>(bitLen >> (i * 8)));

        for (size_t off = 0; off < data.size(); off += kBlock)
            Compress(h, data.data() + off);

        std::array<uint8_t, kDigest> out{};
        for (int i = 0; i < 8; ++i)
            for (int b = 0; b < 8; ++b)
                out[static_cast<size_t>(i) * 8 + b] = static_cast<uint8_t>(h[i] >> ((7 - b) * 8));
        return out;
    }

private:
    static uint64_t Ror(uint64_t x, int n) noexcept { return (x >> n) | (x << (64 - n)); }

    static void Compress(std::array<uint64_t, 8>& h, const uint8_t* p) noexcept
    {
        static const uint64_t K[80] = {
            0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
            0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
            0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
            0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
            0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
            0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
            0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
            0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
            0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
            0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
            0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
            0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
            0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
            0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
            0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
            0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
            0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
            0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
            0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
            0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull };

        uint64_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = 0;
            for (int b = 0; b < 8; ++b)
                w[i] = (w[i] << 8) | p[static_cast<size_t>(i) * 8 + b];
        }
        for (int i = 16; i < 80; ++i) {
            const uint64_t s0 = Ror(w[i - 15], 1) ^ Ror(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const uint64_t s1 = Ror(w[i - 2], 19) ^ Ror(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint64_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint64_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 80; ++i) {
            const uint64_t S1 = Ror(e, 14) ^ Ror(e, 18) ^ Ror(e, 41);
            const uint64_t ch = (e & f) ^ (~e & g);
            const uint64_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint64_t S0 = Ror(a, 28) ^ Ror(a, 34) ^ Ror(a, 39);
            const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint64_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
};

// --- HMAC-SHA512 (RFC 2104 / RFC 4231) -------------------------------------------
[[nodiscard]] inline std::array<uint8_t, 64>
HmacSha512(std::span<const uint8_t> key, std::span<const uint8_t> msg)
{
    std::array<uint8_t, Sha512::kBlock> k0{};
    if (key.size() > Sha512::kBlock) {
        const auto hk = Sha512::Hash(key);
        std::memcpy(k0.data(), hk.data(), hk.size());     // remaining bytes stay 0
    } else {
        std::memcpy(k0.data(), key.data(), key.size());
    }

    std::array<uint8_t, Sha512::kBlock> ipad{}, opad{};
    for (size_t i = 0; i < Sha512::kBlock; ++i) {
        ipad[i] = static_cast<uint8_t>(k0[i] ^ 0x36);
        opad[i] = static_cast<uint8_t>(k0[i] ^ 0x5c);
    }

    std::vector<uint8_t> inner;
    inner.reserve(Sha512::kBlock + msg.size());
    inner.insert(inner.end(), ipad.begin(), ipad.end());
    inner.insert(inner.end(), msg.begin(), msg.end());
    const auto innerHash = Sha512::Hash(inner);

    std::vector<uint8_t> outer;
    outer.reserve(Sha512::kBlock + innerHash.size());
    outer.insert(outer.end(), opad.begin(), opad.end());
    outer.insert(outer.end(), innerHash.begin(), innerHash.end());
    return Sha512::Hash(outer);
}

// --- PBKDF2-HMAC-SHA512 (RFC 2898 / RFC 8018) ------------------------------------
// Derive 'dkLen' bytes from (password, salt) with 'iterations' rounds. The caller
// appends the server pepper to 'password' (§14). iterations is high + tunable and
// stored per-hash so the cost can be raised later (§14, M5 plan area C).
[[nodiscard]] inline std::vector<uint8_t>
Pbkdf2HmacSha512(std::span<const uint8_t> password, std::span<const uint8_t> salt,
                 uint32_t iterations, size_t dkLen)
{
    constexpr size_t hLen = Sha512::kDigest;
    std::vector<uint8_t> dk;
    dk.reserve(((dkLen + hLen - 1) / hLen) * hLen);

    const uint32_t blocks = static_cast<uint32_t>((dkLen + hLen - 1) / hLen);
    for (uint32_t i = 1; i <= blocks; ++i) {
        // U1 = HMAC(P, S || INT_32_BE(i))
        std::vector<uint8_t> saltBlock(salt.begin(), salt.end());
        saltBlock.push_back(static_cast<uint8_t>(i >> 24));
        saltBlock.push_back(static_cast<uint8_t>(i >> 16));
        saltBlock.push_back(static_cast<uint8_t>(i >> 8));
        saltBlock.push_back(static_cast<uint8_t>(i));

        std::array<uint8_t, hLen> u = HmacSha512(password, saltBlock);
        std::array<uint8_t, hLen> t = u; // T_i starts as U1
        for (uint32_t c = 1; c < iterations; ++c) {
            u = HmacSha512(password, u);                 // U_{c+1} = HMAC(P, U_c)
            for (size_t b = 0; b < hLen; ++b) t[b] ^= u[b];
        }
        dk.insert(dk.end(), t.begin(), t.end());
    }

    dk.resize(dkLen);
    return dk;
}

} // namespace Neuron::Crypto
