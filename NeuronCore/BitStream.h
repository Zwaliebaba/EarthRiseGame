#pragma once
// Bit-packing stream — compact wire encoding for snapshots and commands.
// BitWriter appends N-bit fields to a byte buffer.
// BitReader reads them back in the same order.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace Neuron::Serde
{

// ---------------------------------------------------------------------------
// BitWriter — append N bits (1–64) to an internal byte buffer.
// ---------------------------------------------------------------------------
class BitWriter
{
public:
    explicit BitWriter(size_t reserveBytes = 256) { m_buf.reserve(reserveBytes); }

    // Write the low 'bits' bits of 'value'.
    void Write(uint64_t value, int bits)
    {
        assert(bits >= 1 && bits <= 64);
        // Mask to requested width
        if (bits < 64) value &= (uint64_t(1) << bits) - 1u;

        int bitsLeft = bits;
        while (bitsLeft > 0) {
            if (m_bitsFree == 0) {
                m_buf.push_back(0);
                m_bitsFree = 8;
            }
            const int chunk = (std::min)(bitsLeft, m_bitsFree);
            const int shift = m_bitsFree - chunk;
            m_buf.back() |= static_cast<uint8_t>((value >> (bitsLeft - chunk)) << shift);
            bitsLeft    -= chunk;
            m_bitsFree  -= chunk;
        }
    }

    void WriteUint8 (uint8_t  v) { Write(v,  8); }
    void WriteUint16(uint16_t v) { Write(v, 16); }
    void WriteUint32(uint32_t v) { Write(v, 32); }
    void WriteUint64(uint64_t v) { Write(v, 64); }
    void WriteFloat (float    v)
    {
        uint32_t raw;
        std::memcpy(&raw, &v, 4);
        WriteUint32(raw);
    }
    void WriteBool(bool v) { Write(v ? 1u : 0u, 1); }

    // Flush any partial byte (pad with zeros).
    void Flush() { m_bitsFree = 0; }

    [[nodiscard]] std::span<const uint8_t> Data() const noexcept
    {
        return { m_buf.data(), m_buf.size() };
    }
    [[nodiscard]] size_t ByteSize()  const noexcept { return m_buf.size(); }
    [[nodiscard]] size_t BitLength() const noexcept
    {
        return m_buf.size() * 8 - static_cast<size_t>(m_bitsFree);
    }

    void Reset() { m_buf.clear(); m_bitsFree = 0; }

private:
    std::vector<uint8_t> m_buf;
    int                  m_bitsFree{ 0 }; // free bits in last byte
};

// ---------------------------------------------------------------------------
// BitReader — read N bits in the same order they were written.
// ---------------------------------------------------------------------------
class BitReader
{
public:
    explicit BitReader(std::span<const uint8_t> data)
        : m_data(data) {}

    // Read 'bits' bits and return them in the low bits of the result.
    // Returns 0 and sets error on underflow.
    uint64_t Read(int bits)
    {
        assert(bits >= 1 && bits <= 64);
        if (m_error) return 0;

        uint64_t result    = 0;
        int      bitsLeft  = bits;
        while (bitsLeft > 0) {
            if (m_bitAvail == 0) {
                if (m_bytePos >= m_data.size()) { m_error = true; return 0; }
                m_current  = m_data[m_bytePos++];
                m_bitAvail = 8;
            }
            const int chunk  = (std::min)(bitsLeft, m_bitAvail);
            const int shift  = m_bitAvail - chunk;
            result    = (result << chunk) | ((m_current >> shift) & ((1u << chunk) - 1u));
            bitsLeft  -= chunk;
            m_bitAvail -= chunk;
        }
        return result;
    }

    uint8_t  ReadUint8 () { return static_cast<uint8_t> (Read( 8)); }
    uint16_t ReadUint16() { return static_cast<uint16_t>(Read(16)); }
    uint32_t ReadUint32() { return static_cast<uint32_t>(Read(32)); }
    uint64_t ReadUint64() { return Read(64); }
    float    ReadFloat ()
    {
        const uint32_t raw = ReadUint32();
        float v;
        std::memcpy(&v, &raw, 4);
        return v;
    }
    bool ReadBool() { return Read(1) != 0; }

    // Align to next byte boundary.
    void AlignToByte()
    {
        if (m_bitAvail > 0) {
            m_bitAvail = 0;
        }
    }

    [[nodiscard]] bool HasError() const noexcept { return m_error; }
    [[nodiscard]] bool IsEof()    const noexcept
    {
        return m_bytePos >= m_data.size() && m_bitAvail == 0;
    }

private:
    std::span<const uint8_t> m_data;
    size_t  m_bytePos{ 0 };
    uint8_t m_current{ 0 };
    int     m_bitAvail{ 0 };
    bool    m_error{ false };
};

} // namespace Neuron::Serde
