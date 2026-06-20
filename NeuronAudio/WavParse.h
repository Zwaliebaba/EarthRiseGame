#pragma once

// Pure RIFF/WAVE PCM-16 parser (no XAudio2 / Windows dependency).
//
// This is the platform-independent core shared by the NeuronAudio WavReader
// (masterplan §11.3, docs/design/neuronaudio-api.md §3) and the `wavcheck`
// build-time validator (§12.5). Keeping it dependency-free lets the same logic
// be exercised on Linux CI via NeuronTools/testrunner (§16.2) — the Windows
// WavReader is a thin HRESULT wrapper over WavParse::parse().
//
// Policy (§11.3): WAV / PCM-16 only. Non-PCM (ADPCM, float, EXTENSIBLE) and
// non-16-bit files are rejected; we never decode compressed formats at runtime.

#include <cstddef>
#include <cstdint>
#include <span>

namespace er::format
{
  enum class WavStatus : uint8_t
  {
    Ok = 0,
    NotRiffWave,       // missing 'RIFF' ... 'WAVE' container
    Truncated,         // a declared chunk runs past the end of the buffer
    MissingFmt,        // no 'fmt ' chunk before 'data'
    MissingData,       // no 'data' chunk
    UnsupportedFormat, // audioFormat != PCM (1) — ADPCM/float/EXTENSIBLE rejected
    NotPcm16           // bitsPerSample != 16
  };

  struct WavFormat
  {
    uint16_t channels = 0;       // 1 = mono (3D emitter), 2 = stereo (music/ambient/UI)
    uint32_t sampleRate = 0;     // e.g. 44100, 48000
    uint16_t bitsPerSample = 0;  // always 16 on success
  };

  struct WavData
  {
    WavFormat format{};
    std::span<const std::byte> pcm{}; // 16-bit PCM frames, interleaved by channel
  };

  // Linear-PCM format tag. Named WAV_PCM_TAG (not WAVE_FORMAT_PCM) because the
  // Windows SDK defines WAVE_FORMAT_PCM as a *macro* in <mmreg.h>, which would
  // otherwise rewrite this declaration when both headers are included.
  inline constexpr uint16_t WAV_PCM_TAG = 0x0001;

  inline constexpr bool isMono(const WavFormat& f) noexcept { return f.channels == 1; }

  namespace detail
  {
    inline uint16_t readU16(const std::byte* p) noexcept
    {
      return static_cast<uint16_t>(std::to_integer<uint16_t>(p[0]) |
                                   (std::to_integer<uint16_t>(p[1]) << 8));
    }

    inline uint32_t readU32(const std::byte* p) noexcept
    {
      return std::to_integer<uint32_t>(p[0]) |
             (std::to_integer<uint32_t>(p[1]) << 8) |
             (std::to_integer<uint32_t>(p[2]) << 16) |
             (std::to_integer<uint32_t>(p[3]) << 24);
    }

    inline bool tagEquals(const std::byte* p, char a, char b, char c, char d) noexcept
    {
      return std::to_integer<char>(p[0]) == a && std::to_integer<char>(p[1]) == b &&
             std::to_integer<char>(p[2]) == c && std::to_integer<char>(p[3]) == d;
    }
  } // namespace detail

  // Parse a RIFF/WAVE blob, validating it is linear PCM-16. On WavStatus::Ok,
  // `out.pcm` aliases into `file` (no copy) and `out.format` is filled.
  inline WavStatus parseWav(std::span<const std::byte> file, WavData& out) noexcept
  {
    using namespace detail;

    // RIFF header: 'RIFF' <u32 size> 'WAVE' = 12 bytes minimum.
    if (file.size() < 12)
      return WavStatus::NotRiffWave;
    if (!tagEquals(file.data(), 'R', 'I', 'F', 'F') ||
        !tagEquals(file.data() + 8, 'W', 'A', 'V', 'E'))
      return WavStatus::NotRiffWave;

    bool haveFmt = false;
    WavFormat fmt{};
    std::span<const std::byte> data{};
    bool haveData = false;

    // Walk sub-chunks starting after the 12-byte RIFF/WAVE header.
    size_t pos = 12;
    while (pos + 8 <= file.size())
    {
      const std::byte* hdr = file.data() + pos;
      const uint32_t chunkSize = readU32(hdr + 4);
      const size_t body = pos + 8;

      // Chunk body must fit inside the buffer.
      if (body + chunkSize > file.size())
        return WavStatus::Truncated;

      if (tagEquals(hdr, 'f', 'm', 't', ' '))
      {
        if (chunkSize < 16)
          return WavStatus::Truncated;
        const std::byte* f = file.data() + body;
        const uint16_t audioFormat = readU16(f + 0);
        fmt.channels = readU16(f + 2);
        fmt.sampleRate = readU32(f + 4);
        fmt.bitsPerSample = readU16(f + 14);

        if (audioFormat != WAV_PCM_TAG)
          return WavStatus::UnsupportedFormat;
        if (fmt.bitsPerSample != 16)
          return WavStatus::NotPcm16;
        haveFmt = true;
      }
      else if (tagEquals(hdr, 'd', 'a', 't', 'a'))
      {
        data = file.subspan(body, chunkSize);
        haveData = true;
      }

      // Advance; chunks are word-aligned (a pad byte follows odd sizes).
      size_t advance = static_cast<size_t>(chunkSize) + (chunkSize & 1u);
      pos = body + advance;
    }

    if (!haveFmt)
      return WavStatus::MissingFmt;
    if (!haveData)
      return WavStatus::MissingData;

    out.format = fmt;
    out.pcm = data;
    return WavStatus::Ok;
  }
} // namespace er::format
