#pragma once

// Byte-buffer builders for synthetic test assets (WAV / DDS / CMO). These let
// the parser tests construct known-good and deliberately-malformed inputs in
// code, with no binary fixtures checked into the tree.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ertest
{
  // Little-endian byte writer.
  class ByteWriter
  {
  public:
    void u8(uint8_t v) { m_bytes.push_back(static_cast<std::byte>(v)); }
    void u16(uint16_t v)
    {
      u8(static_cast<uint8_t>(v & 0xff));
      u8(static_cast<uint8_t>((v >> 8) & 0xff));
    }
    void u32(uint32_t v)
    {
      u8(static_cast<uint8_t>(v & 0xff));
      u8(static_cast<uint8_t>((v >> 8) & 0xff));
      u8(static_cast<uint8_t>((v >> 16) & 0xff));
      u8(static_cast<uint8_t>((v >> 24) & 0xff));
    }
    void f32(float f)
    {
      uint32_t bits;
      static_assert(sizeof(bits) == sizeof(f));
      std::memcpy(&bits, &f, sizeof(bits));
      u32(bits);
    }
    void tag(char a, char b, char c, char d)
    {
      u8(static_cast<uint8_t>(a));
      u8(static_cast<uint8_t>(b));
      u8(static_cast<uint8_t>(c));
      u8(static_cast<uint8_t>(d));
    }
    void zeros(size_t n)
    {
      for (size_t i = 0; i < n; ++i)
        u8(0);
    }
    void bytes(const std::vector<uint8_t>& b)
    {
      for (uint8_t v : b)
        u8(v);
    }

    const std::vector<std::byte>& data() const { return m_bytes; }
    size_t size() const { return m_bytes.size(); }
    void patchU32(size_t offset, uint32_t v)
    {
      m_bytes[offset + 0] = static_cast<std::byte>(v & 0xff);
      m_bytes[offset + 1] = static_cast<std::byte>((v >> 8) & 0xff);
      m_bytes[offset + 2] = static_cast<std::byte>((v >> 16) & 0xff);
      m_bytes[offset + 3] = static_cast<std::byte>((v >> 24) & 0xff);
    }

  private:
    std::vector<std::byte> m_bytes;
  };

  // Build a minimal RIFF/WAVE PCM file with the given format and `dataBytes`
  // bytes of (zeroed) sample data.
  inline std::vector<std::byte> makeWav(uint16_t audioFormat, uint16_t channels,
                                        uint32_t sampleRate, uint16_t bits, uint32_t dataBytes)
  {
    ByteWriter w;
    const uint16_t blockAlign = static_cast<uint16_t>(channels * bits / 8);
    const uint32_t byteRate = sampleRate * blockAlign;

    w.tag('R', 'I', 'F', 'F');
    w.u32(4 + (8 + 16) + (8 + dataBytes)); // RIFF size = 'WAVE' + fmt chunk + data chunk
    w.tag('W', 'A', 'V', 'E');

    w.tag('f', 'm', 't', ' ');
    w.u32(16);
    w.u16(audioFormat);
    w.u16(channels);
    w.u32(sampleRate);
    w.u32(byteRate);
    w.u16(blockAlign);
    w.u16(bits);

    w.tag('d', 'a', 't', 'a');
    w.u32(dataBytes);
    w.zeros(dataBytes);
    return w.data();
  }

  // DDS pixel-format flags.
  inline constexpr uint32_t DDPF_FOURCC = 0x4;
  inline constexpr uint32_t DDPF_RGB = 0x40;

  // Build a DDS file header (124-byte DDS_HEADER). If `fourCC` is non-zero a
  // FOURCC pixel format is written; otherwise an RGB pixel format with the given
  // masks. `dxt10Format` (when non-zero with fourCC 'DX10') appends the ext.
  inline std::vector<std::byte> makeDds(uint32_t width, uint32_t height, uint32_t mips,
                                        uint32_t fourCC, uint32_t rgbBitCount, uint32_t rMask,
                                        uint32_t gMask, uint32_t bMask, uint32_t aMask,
                                        uint32_t dxt10Format = 0, uint32_t payloadBytes = 0)
  {
    ByteWriter w;
    w.tag('D', 'D', 'S', ' ');
    w.u32(124);    // dwSize
    w.u32(0x1007); // dwFlags (caps|height|width|pixelformat)
    w.u32(height);
    w.u32(width);
    w.u32(0); // pitch/linear size
    w.u32(0); // depth
    w.u32(mips);
    w.zeros(11 * 4); // dwReserved1[11]

    // DDS_PIXELFORMAT (32 bytes) at header offset 72.
    w.u32(32); // dwSize
    w.u32(fourCC != 0 ? DDPF_FOURCC : DDPF_RGB);
    w.u32(fourCC);
    w.u32(rgbBitCount);
    w.u32(rMask);
    w.u32(gMask);
    w.u32(bMask);
    w.u32(aMask);

    w.u32(0x1000); // dwCaps (texture)
    w.u32(0);
    w.u32(0);
    w.u32(0);
    w.u32(0); // dwReserved2

    if (dxt10Format != 0)
    {
      w.u32(dxt10Format); // dxgiFormat
      w.u32(3);           // resourceDimension = TEXTURE2D
      w.u32(0);           // miscFlag
      w.u32(1);           // arraySize
      w.u32(0);           // miscFlags2
    }

    w.zeros(payloadBytes);
    return w.data();
  }
} // namespace ertest
