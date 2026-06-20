// WAV/RIFF parser tests (mirror NeuronAudioTest, docs/design/neuronaudio-api.md §10).

#include "../../NeuronAudio/WavParse.h"
#include "Fixtures.h"
#include "TestRunner.h"

using namespace er::format;
using namespace ertest;

ER_TEST(WavParse, ValidMono16)
{
  auto file = makeWav(WAV_PCM_TAG, 1, 44100, 16, 64);
  WavData out{};
  ER_CHECK(parseWav(file, out) == WavStatus::Ok);
  ER_CHECK_EQ(out.format.channels, static_cast<uint16_t>(1));
  ER_CHECK_EQ(out.format.sampleRate, 44100u);
  ER_CHECK_EQ(out.format.bitsPerSample, static_cast<uint16_t>(16));
  ER_CHECK(isMono(out.format));
  ER_CHECK_EQ(out.pcm.size(), static_cast<size_t>(64));
}

ER_TEST(WavParse, ValidStereo16)
{
  auto file = makeWav(WAV_PCM_TAG, 2, 48000, 16, 128);
  WavData out{};
  ER_CHECK(parseWav(file, out) == WavStatus::Ok);
  ER_CHECK_EQ(out.format.channels, static_cast<uint16_t>(2));
  ER_CHECK(!isMono(out.format));
  ER_CHECK_EQ(out.pcm.size(), static_cast<size_t>(128));
}

ER_TEST(WavParse, RejectsNonPcm)
{
  // audioFormat 0x0011 = IMA ADPCM — must be rejected (PCM-16 only, §11.3).
  auto file = makeWav(0x0011, 1, 44100, 16, 32);
  WavData out{};
  ER_CHECK(parseWav(file, out) == WavStatus::UnsupportedFormat);
}

ER_TEST(WavParse, RejectsNon16Bit)
{
  auto file = makeWav(WAV_PCM_TAG, 1, 44100, 24, 30);
  WavData out{};
  ER_CHECK(parseWav(file, out) == WavStatus::NotPcm16);
}

ER_TEST(WavParse, RejectsNotRiff)
{
  std::vector<std::byte> garbage(32, std::byte{0xAB});
  WavData out{};
  ER_CHECK(parseWav(garbage, out) == WavStatus::NotRiffWave);
}

ER_TEST(WavParse, RejectsTruncatedDataChunk)
{
  auto file = makeWav(WAV_PCM_TAG, 1, 44100, 16, 64);
  // Inflate the declared 'data' size beyond the buffer. 'data' size field sits
  // right after the 16-byte fmt chunk body: 12 (RIFF) +8+16 (fmt) +4 ('data').
  WavData out{};
  std::vector<std::byte> mutable_file = file;
  // Patch the data chunk size to a huge value.
  const size_t dataSizeOffset = 12 + 8 + 16 + 4;
  mutable_file[dataSizeOffset + 0] = std::byte{0xff};
  mutable_file[dataSizeOffset + 1] = std::byte{0xff};
  mutable_file[dataSizeOffset + 2] = std::byte{0xff};
  mutable_file[dataSizeOffset + 3] = std::byte{0x7f};
  ER_CHECK(parseWav(mutable_file, out) == WavStatus::Truncated);
}

ER_TEST(WavParse, MissingDataChunk)
{
  // fmt-only file (no data chunk).
  ByteWriter w;
  w.tag('R', 'I', 'F', 'F');
  w.u32(4 + 8 + 16);
  w.tag('W', 'A', 'V', 'E');
  w.tag('f', 'm', 't', ' ');
  w.u32(16);
  w.u16(WAV_PCM_TAG);
  w.u16(1);
  w.u32(44100);
  w.u32(88200);
  w.u16(2);
  w.u16(16);
  WavData out{};
  ER_CHECK(parseWav(w.data(), out) == WavStatus::MissingData);
}

ER_TEST(WavParse, SkipsUnknownChunkBeforeData)
{
  // RIFF/WAVE with a 'LIST' chunk (odd size → padded) ahead of fmt/data.
  ByteWriter w;
  w.tag('R', 'I', 'F', 'F');
  const size_t riffSizeOffset = w.size();
  w.u32(0); // patched below
  w.tag('W', 'A', 'V', 'E');

  w.tag('L', 'I', 'S', 'T');
  w.u32(3); // odd size → one pad byte follows
  w.u8(1);
  w.u8(2);
  w.u8(3);
  w.u8(0); // pad

  w.tag('f', 'm', 't', ' ');
  w.u32(16);
  w.u16(WAV_PCM_TAG);
  w.u16(1);
  w.u32(22050);
  w.u32(44100);
  w.u16(2);
  w.u16(16);

  w.tag('d', 'a', 't', 'a');
  w.u32(16);
  w.zeros(16);

  ByteWriter patched = w;
  patched.patchU32(riffSizeOffset, static_cast<uint32_t>(w.size() - 8));

  WavData out{};
  ER_CHECK(parseWav(patched.data(), out) == WavStatus::Ok);
  ER_CHECK_EQ(out.format.sampleRate, 22050u);
  ER_CHECK_EQ(out.pcm.size(), static_cast<size_t>(16));
}
