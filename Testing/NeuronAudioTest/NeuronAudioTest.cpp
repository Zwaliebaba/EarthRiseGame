#include "pch.h"
#include "CppUnitTest.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "AudioTypes.h"
#include "SpatialMath.h"
#include "VoiceHandle.h"
#include "WavParse.h"
#include "WavReader.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron::Audio;

// NeuronAudioTest — device-free logic for the NeuronAudio library (§16.1).
// XAudio2 device init/teardown is a Windows-agent smoke test added separately;
// these cases need no audio device. The platform-independent RIFF parser itself
// is also covered in NeuronTools/testrunner (§16.2); here we test the WavReader
// wrapper + WAVEFORMATEX construction and the mixer/handle/spatial math.

namespace
{
  void pushU16(std::vector<std::byte>& b, uint16_t v)
  {
    b.push_back(static_cast<std::byte>(v & 0xff));
    b.push_back(static_cast<std::byte>((v >> 8) & 0xff));
  }
  void pushU32(std::vector<std::byte>& b, uint32_t v)
  {
    for (int i = 0; i < 4; ++i)
      b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xff));
  }
  void pushTag(std::vector<std::byte>& b, const char* t)
  {
    for (int i = 0; i < 4; ++i)
      b.push_back(static_cast<std::byte>(t[i]));
  }

  std::vector<std::byte> makeWav(uint16_t audioFormat, uint16_t channels, uint32_t sampleRate,
                                 uint16_t bits, uint32_t dataBytes)
  {
    std::vector<std::byte> b;
    const uint16_t blockAlign = static_cast<uint16_t>(channels * bits / 8);
    pushTag(b, "RIFF");
    pushU32(b, 4 + (8 + 16) + (8 + dataBytes));
    pushTag(b, "WAVE");
    pushTag(b, "fmt ");
    pushU32(b, 16);
    pushU16(b, audioFormat);
    pushU16(b, channels);
    pushU32(b, sampleRate);
    pushU32(b, sampleRate * blockAlign);
    pushU16(b, blockAlign);
    pushU16(b, bits);
    pushTag(b, "data");
    pushU32(b, dataBytes);
    b.insert(b.end(), dataBytes, std::byte{0});
    return b;
  }
} // namespace

namespace NeuronAudioTest
{
  TEST_CLASS(WavReaderTests)
  {
  public:
    TEST_METHOD(LoadsMonoPcm16)
    {
      auto file = makeWav(WAVE_FORMAT_PCM, 1, 44100, 16, 64);
      WavClip clip;
      Assert::IsTrue(clip.load(file) == WavLoadStatus::Ok);
      Assert::IsTrue(clip.isMono());
      Assert::AreEqual<uint32_t>(44100, clip.format().nSamplesPerSec);
      Assert::AreEqual<uint16_t>(16, clip.format().wBitsPerSample);
      Assert::AreEqual<uint16_t>(WAVE_FORMAT_PCM, clip.format().wFormatTag);
      Assert::AreEqual<size_t>(64, clip.pcm().size());
    }

    TEST_METHOD(LoadsStereoBlockAlign)
    {
      auto file = makeWav(WAVE_FORMAT_PCM, 2, 48000, 16, 128);
      WavClip clip;
      Assert::IsTrue(clip.load(file) == WavLoadStatus::Ok);
      Assert::IsFalse(clip.isMono());
      // Stereo 16-bit ⇒ 4-byte frames.
      Assert::AreEqual<uint16_t>(4, clip.format().nBlockAlign);
      Assert::AreEqual<uint32_t>(48000u * 4u, clip.format().nAvgBytesPerSec);
    }

    TEST_METHOD(RejectsNonPcm)
    {
      auto file = makeWav(0x0011 /*ADPCM*/, 1, 44100, 16, 32);
      WavClip clip;
      Assert::IsTrue(clip.load(file) == WavLoadStatus::UnsupportedFormat);
    }

    TEST_METHOD(RejectsNon16Bit)
    {
      auto file = makeWav(WAVE_FORMAT_PCM, 1, 44100, 24, 30);
      WavClip clip;
      Assert::IsTrue(clip.load(file) == WavLoadStatus::NotPcm16);
    }
  };

  TEST_CLASS(MixerMathTests)
  {
  public:
    TEST_METHOD(EffectiveGainCombinesMasterBusVoice)
    {
      MixSnapshot mix{};
      mix.master = 0.5f;
      mix.sfx = 0.5f;
      Assert::AreEqual(0.25f, effectiveGain(mix, Bus::Sfx, 1.0f), 1e-6f);
      Assert::AreEqual(0.125f, effectiveGain(mix, Bus::Sfx, 0.5f), 1e-6f);
    }

    TEST_METHOD(BusVolumeSelectsCorrectChannel)
    {
      MixSnapshot mix{};
      mix.music = 0.1f;
      mix.ambient = 0.2f;
      mix.sfx = 0.3f;
      mix.ui = 0.4f;
      Assert::AreEqual(0.1f, busVolume(mix, Bus::Music), 1e-6f);
      Assert::AreEqual(0.2f, busVolume(mix, Bus::Ambient), 1e-6f);
      Assert::AreEqual(0.3f, busVolume(mix, Bus::Sfx), 1e-6f);
      Assert::AreEqual(0.4f, busVolume(mix, Bus::Ui), 1e-6f);
    }
  };

  TEST_CLASS(VoiceHandleTests)
  {
  public:
    TEST_METHOD(PackUnpackRoundTrip)
    {
      VoiceId id = makeVoiceId(1234, 7);
      Assert::AreEqual<uint32_t>(1234, voiceIndex(id));
      Assert::AreEqual<uint32_t>(7, voiceGeneration(id));
      Assert::IsTrue(id.valid());
    }

    TEST_METHOD(NullHandleIsIndexZeroGenZero)
    {
      VoiceId nullId = makeVoiceId(0, 0);
      Assert::IsFalse(nullId.valid());
      // A real slot uses generation >= 1, so it never collides with null.
      Assert::IsTrue(makeVoiceId(0, 1).valid());
    }

    TEST_METHOD(GenerationDistinguishesRecycledSlot)
    {
      VoiceId older = makeVoiceId(5, 1);
      VoiceId newer = makeVoiceId(5, 2);
      Assert::IsFalse(older == newer);
      Assert::AreEqual(voiceIndex(older), voiceIndex(newer));
    }
  };

  TEST_CLASS(SpatialMathTests)
  {
  public:
    TEST_METHOD(AttenuationRolloff)
    {
      using namespace spatialmath;
      Assert::AreEqual(1.0f, attenuation(10.0f, 25.0f, 5000.0f), 1e-6f); // within min
      Assert::AreEqual(0.0f, attenuation(6000.0f, 25.0f, 5000.0f), 1e-6f); // beyond max
      // Midpoint of the linear band.
      const float mid = attenuation(2512.5f, 25.0f, 5000.0f);
      Assert::IsTrue(mid > 0.49f && mid < 0.51f);
    }

    TEST_METHOD(DistanceIsListenerRelative)
    {
      using namespace spatialmath;
      Listener l{};
      l.position = {0, 0, 0};
      Emitter e{};
      e.position = {3, 4, 0};
      Assert::AreEqual(5.0f, distance(l, e), 1e-5f);
    }

    TEST_METHOD(DopplerApproachRaisesPitch)
    {
      using namespace spatialmath;
      Listener l{};
      l.position = {0, 0, 0};
      Emitter e{};
      e.position = {0, 0, 100};
      e.velocity = {0, 0, -50}; // moving toward the listener along +Z axis
      const float ratio = dopplerRatio(l, e);
      Assert::IsTrue(ratio > 1.0f);
    }

    TEST_METHOD(DopplerRecedeLowersPitch)
    {
      using namespace spatialmath;
      Listener l{};
      Emitter e{};
      e.position = {0, 0, 100};
      e.velocity = {0, 0, 50}; // moving away
      const float ratio = dopplerRatio(l, e);
      Assert::IsTrue(ratio < 1.0f);
    }

    TEST_METHOD(StationaryDopplerIsUnity)
    {
      using namespace spatialmath;
      Listener l{};
      Emitter e{};
      e.position = {0, 0, 100};
      Assert::AreEqual(1.0f, dopplerRatio(l, e), 1e-4f);
    }
  };

  // WavParseTests — the platform-independent RIFF/WAVE PCM-16 parser core
  // (er::format::parseWav in WavParse.h) mirrored from NeuronTools/testrunner
  // (§16.2). WavReaderTests above covers the thin WavReader wrapper; these
  // exercise the parser directly, including chunk-walking edge cases.
  TEST_CLASS(WavParseTests)
  {
  public:
    TEST_METHOD(ValidMono16)
    {
      auto file = makeWav(er::format::WAV_PCM_TAG, 1, 44100, 16, 64);
      er::format::WavData out{};
      Assert::IsTrue(er::format::parseWav(file, out) == er::format::WavStatus::Ok);
      Assert::IsTrue(out.format.channels == static_cast<uint16_t>(1));
      Assert::IsTrue(out.format.sampleRate == 44100u);
      Assert::IsTrue(out.format.bitsPerSample == static_cast<uint16_t>(16));
      Assert::IsTrue(er::format::isMono(out.format));
      Assert::IsTrue(out.pcm.size() == static_cast<size_t>(64));
    }

    TEST_METHOD(ValidStereo16)
    {
      auto file = makeWav(er::format::WAV_PCM_TAG, 2, 48000, 16, 128);
      er::format::WavData out{};
      Assert::IsTrue(er::format::parseWav(file, out) == er::format::WavStatus::Ok);
      Assert::IsTrue(out.format.channels == static_cast<uint16_t>(2));
      Assert::IsTrue(!er::format::isMono(out.format));
      Assert::IsTrue(out.pcm.size() == static_cast<size_t>(128));
    }

    TEST_METHOD(RejectsNonPcm)
    {
      // audioFormat 0x0011 = IMA ADPCM — must be rejected (PCM-16 only, §11.3).
      auto file = makeWav(0x0011, 1, 44100, 16, 32);
      er::format::WavData out{};
      Assert::IsTrue(er::format::parseWav(file, out) == er::format::WavStatus::UnsupportedFormat);
    }

    TEST_METHOD(RejectsNon16Bit)
    {
      auto file = makeWav(er::format::WAV_PCM_TAG, 1, 44100, 24, 30);
      er::format::WavData out{};
      Assert::IsTrue(er::format::parseWav(file, out) == er::format::WavStatus::NotPcm16);
    }

    TEST_METHOD(RejectsNotRiff)
    {
      std::vector<std::byte> garbage(32, std::byte{0xAB});
      er::format::WavData out{};
      Assert::IsTrue(er::format::parseWav(garbage, out) == er::format::WavStatus::NotRiffWave);
    }

    TEST_METHOD(RejectsTruncatedDataChunk)
    {
      auto file = makeWav(er::format::WAV_PCM_TAG, 1, 44100, 16, 64);
      // Inflate the declared 'data' size beyond the buffer. The 'data' size field
      // sits after: 12 (RIFF) + 8+16 (fmt) + 4 ('data' tag).
      const size_t dataSizeOffset = 12 + 8 + 16 + 4;
      file[dataSizeOffset + 0] = std::byte{0xff};
      file[dataSizeOffset + 1] = std::byte{0xff};
      file[dataSizeOffset + 2] = std::byte{0xff};
      file[dataSizeOffset + 3] = std::byte{0x7f};
      er::format::WavData out{};
      Assert::IsTrue(er::format::parseWav(file, out) == er::format::WavStatus::Truncated);
    }

    TEST_METHOD(MissingDataChunk)
    {
      // fmt-only file (no data chunk).
      std::vector<std::byte> b;
      pushTag(b, "RIFF");
      pushU32(b, 4 + 8 + 16);
      pushTag(b, "WAVE");
      pushTag(b, "fmt ");
      pushU32(b, 16);
      pushU16(b, er::format::WAV_PCM_TAG);
      pushU16(b, 1);
      pushU32(b, 44100);
      pushU32(b, 88200);
      pushU16(b, 2);
      pushU16(b, 16);
      er::format::WavData out{};
      Assert::IsTrue(er::format::parseWav(b, out) == er::format::WavStatus::MissingData);
    }

    TEST_METHOD(SkipsUnknownChunkBeforeData)
    {
      // RIFF/WAVE with a 'LIST' chunk (odd size → padded) ahead of fmt/data.
      std::vector<std::byte> b;
      pushTag(b, "RIFF");
      const size_t riffSizeOffset = b.size();
      pushU32(b, 0); // patched below
      pushTag(b, "WAVE");

      pushTag(b, "LIST");
      pushU32(b, 3); // odd size → one pad byte follows
      b.push_back(std::byte{1});
      b.push_back(std::byte{2});
      b.push_back(std::byte{3});
      b.push_back(std::byte{0}); // pad

      pushTag(b, "fmt ");
      pushU32(b, 16);
      pushU16(b, er::format::WAV_PCM_TAG);
      pushU16(b, 1);
      pushU32(b, 22050);
      pushU32(b, 44100);
      pushU16(b, 2);
      pushU16(b, 16);

      pushTag(b, "data");
      pushU32(b, 16);
      b.insert(b.end(), 16, std::byte{0});

      // Patch the RIFF size to everything after the 8-byte RIFF header.
      const uint32_t riffSize = static_cast<uint32_t>(b.size() - 8);
      for (int i = 0; i < 4; ++i)
        b[riffSizeOffset + i] = static_cast<std::byte>((riffSize >> (8 * i)) & 0xff);

      er::format::WavData out{};
      Assert::IsTrue(er::format::parseWav(b, out) == er::format::WavStatus::Ok);
      Assert::IsTrue(out.format.sampleRate == 22050u);
      Assert::IsTrue(out.pcm.size() == static_cast<size_t>(16));
    }
  };
} // namespace NeuronAudioTest
