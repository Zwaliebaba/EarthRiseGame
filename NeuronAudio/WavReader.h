#pragma once
// WAV → in-memory PCM-16 clip for XAudio2 (wraps the platform-independent
// er::format::parseWav core in NeuronTools/WavParse.h).
//
// Kept header-inline (no .cpp, no XAudio2 dependency) so the parser-wrapping
// logic — RIFF validation + WAVEFORMATEX construction — is unit-testable in
// NeuronAudioTest without an audio device. AudioEngine submits WavClip::pcm()
// to an IXAudio2SourceVoice created from WavClip::format(). (§11.3, §12.5;
// neuronaudio-api.md §3.)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <mmreg.h>

#include <cstddef>
#include <span>
#include <vector>

#include "AudioTypes.h"
#include "WavParse.h" // platform-independent RIFF/PCM-16 core (owned by NeuronAudio)

namespace Neuron::Audio
{
  enum class WavLoadStatus : uint8_t
  {
    Ok = 0,
    NotRiffWave,
    Truncated,
    MissingFmt,
    MissingData,
    UnsupportedFormat,
    NotPcm16
  };

  inline WavLoadStatus fromParseStatus(er::format::WavStatus s) noexcept
  {
    using S = er::format::WavStatus;
    switch (s)
    {
    case S::Ok:
      return WavLoadStatus::Ok;
    case S::NotRiffWave:
      return WavLoadStatus::NotRiffWave;
    case S::Truncated:
      return WavLoadStatus::Truncated;
    case S::MissingFmt:
      return WavLoadStatus::MissingFmt;
    case S::MissingData:
      return WavLoadStatus::MissingData;
    case S::UnsupportedFormat:
      return WavLoadStatus::UnsupportedFormat;
    case S::NotPcm16:
      return WavLoadStatus::NotPcm16;
    }
    return WavLoadStatus::UnsupportedFormat;
  }

  // Build a PCM WAVEFORMATEX from a parsed WAV format (PCM-16 ⇒ cbSize 0).
  inline WAVEFORMATEX makeWaveFormat(const er::format::WavFormat& f) noexcept
  {
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = f.channels;
    wfx.nSamplesPerSec = f.sampleRate;
    wfx.wBitsPerSample = f.bitsPerSample;
    wfx.nBlockAlign = static_cast<WORD>(f.channels * f.bitsPerSample / 8);
    wfx.nAvgBytesPerSec = f.sampleRate * wfx.nBlockAlign;
    wfx.cbSize = 0;
    return wfx;
  }

  // Fully-decoded short sound held in memory (event SFX / UI; also used for
  // looped ambient/music until buffer-queue streaming lands — see AudioEngine).
  class WavClip
  {
  public:
    // Parse + copy PCM out of `file`. The clip owns its sample bytes afterwards,
    // so `file` need not outlive it.
    WavLoadStatus load(std::span<const std::byte> file)
    {
      er::format::WavData data{};
      const er::format::WavStatus s = er::format::parseWav(file, data);
      if (s != er::format::WavStatus::Ok)
        return fromParseStatus(s);

      m_format = makeWaveFormat(data.format);
      m_pcm.assign(data.pcm.begin(), data.pcm.end());
      return WavLoadStatus::Ok;
    }

    const WAVEFORMATEX& format() const noexcept { return m_format; }
    std::span<const std::byte> pcm() const noexcept { return m_pcm; }
    bool isMono() const noexcept { return m_format.nChannels == 1; }
    bool empty() const noexcept { return m_pcm.empty(); }

  private:
    WAVEFORMATEX m_format{};
    std::vector<std::byte> m_pcm;
  };
} // namespace Neuron::Audio
