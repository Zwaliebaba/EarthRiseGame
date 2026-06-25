#pragma once
// Versioned binary buffer — wire protocol and snapshot serialization.
// Wraps BitWriter/BitReader with a version header and cursor management.

#include "BitStream.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Neuron::Serde
{

// Protocol version — increment on any wire-format change.
inline constexpr uint32_t PROTOCOL_VERSION = 1;

// ---------------------------------------------------------------------------
// WriteBuffer — serialise data into a flat byte array with version header.
// ---------------------------------------------------------------------------
class WriteBuffer
{
public:
    explicit WriteBuffer(size_t reserveBytes = 512)
        : m_writer(reserveBytes)
    {
        m_writer.WriteUint32(PROTOCOL_VERSION);
    }

    void WriteUint8 (uint8_t  v) { m_writer.WriteUint8(v);  }
    void WriteUint16(uint16_t v) { m_writer.WriteUint16(v); }
    void WriteUint32(uint32_t v) { m_writer.WriteUint32(v); }
    void WriteUint64(uint64_t v) { m_writer.WriteUint64(v); }
    void WriteInt64 (int64_t  v) { m_writer.WriteUint64(static_cast<uint64_t>(v)); }
    void WriteFloat (float    v) { m_writer.WriteFloat(v);  }
    void WriteBool  (bool     v) { m_writer.WriteBool(v);   }
    void WriteBits  (uint64_t v, int bits) { m_writer.Write(v, bits); }

    void WriteBytes(const void* data, size_t len)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        m_writer.Flush();
        for (size_t i = 0; i < len; ++i)
            m_writer.WriteUint8(p[i]);
    }

    void Finalise() { m_writer.Flush(); }

    [[nodiscard]] std::span<const uint8_t> Data() const noexcept
    {
        return m_writer.Data();
    }
    [[nodiscard]] size_t ByteSize() const noexcept { return m_writer.ByteSize(); }

private:
    BitWriter m_writer;
};

// ---------------------------------------------------------------------------
// ReadBuffer — deserialise, with version check and error propagation.
// ---------------------------------------------------------------------------
class ReadBuffer
{
public:
    explicit ReadBuffer(std::span<const uint8_t> data)
        : m_reader(data)
    {
        const uint32_t ver = m_reader.ReadUint32();
        if (ver != PROTOCOL_VERSION)
            m_versionMismatch = true;
    }

    uint8_t  ReadUint8 () { return m_reader.ReadUint8();  }
    uint16_t ReadUint16() { return m_reader.ReadUint16(); }
    uint32_t ReadUint32() { return m_reader.ReadUint32(); }
    uint64_t ReadUint64() { return m_reader.ReadUint64(); }
    int64_t  ReadInt64 () { return static_cast<int64_t>(m_reader.ReadUint64()); }
    float    ReadFloat () { return m_reader.ReadFloat();  }
    bool     ReadBool  () { return m_reader.ReadBool();   }
    uint64_t ReadBits  (int bits) { return m_reader.Read(bits); }

    void ReadBytes(void* dst, size_t len)
    {
        m_reader.AlignToByte();
        auto* p = static_cast<uint8_t*>(dst);
        for (size_t i = 0; i < len; ++i)
            p[i] = m_reader.ReadUint8();
    }

    [[nodiscard]] bool IsGood()           const noexcept { return !m_reader.HasError() && !m_versionMismatch; }
    [[nodiscard]] bool HasVersionMismatch() const noexcept { return m_versionMismatch; }

private:
    BitReader m_reader;
    bool      m_versionMismatch{ false };
};

} // namespace Neuron::Serde
