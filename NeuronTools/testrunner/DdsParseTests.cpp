// DDS header parser tests (mirror NeuronRenderTest, docs/design/
// neuronrender-architecture.md §11 + darwinia-menu-ui.md §2.1).

#include "../../NeuronRender/DdsParse.h"
#include "Fixtures.h"
#include "TestRunner.h"

using namespace er::format;
using namespace ertest;

namespace
{
  uint32_t fourCC(char a, char b, char c, char d)
  {
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
  }
} // namespace

ER_TEST(DdsParse, Dxt1IsBc1)
{
  auto file = makeDds(256, 256, 9, fourCC('D', 'X', 'T', '1'), 0, 0, 0, 0, 0);
  DdsImage img{};
  ER_CHECK(parseDds(file, img) == DdsStatus::Ok);
  ER_CHECK(img.format == DxgiFormat::BC1_UNORM);
  ER_CHECK(img.blockCompressed);
  ER_CHECK_EQ(img.width, 256u);
  ER_CHECK_EQ(img.height, 256u);
  ER_CHECK_EQ(img.mipCount, 9u);
}

ER_TEST(DdsParse, Dxt3IsBc2_Dxt5IsBc3)
{
  DdsImage img{};
  auto dxt3 = makeDds(64, 64, 1, fourCC('D', 'X', 'T', '3'), 0, 0, 0, 0, 0);
  ER_CHECK(parseDds(dxt3, img) == DdsStatus::Ok);
  ER_CHECK(img.format == DxgiFormat::BC2_UNORM);

  auto dxt5 = makeDds(64, 64, 1, fourCC('D', 'X', 'T', '5'), 0, 0, 0, 0, 0);
  ER_CHECK(parseDds(dxt5, img) == DdsStatus::Ok);
  ER_CHECK(img.format == DxgiFormat::BC3_UNORM);
}

ER_TEST(DdsParse, Ati1IsBc4_Ati2IsBc5)
{
  DdsImage img{};
  auto bc4 = makeDds(32, 32, 1, fourCC('A', 'T', 'I', '1'), 0, 0, 0, 0, 0);
  ER_CHECK(parseDds(bc4, img) == DdsStatus::Ok);
  ER_CHECK(img.format == DxgiFormat::BC4_UNORM);

  auto bc5 = makeDds(32, 32, 1, fourCC('A', 'T', 'I', '2'), 0, 0, 0, 0, 0);
  ER_CHECK(parseDds(bc5, img) == DdsStatus::Ok);
  ER_CHECK(img.format == DxgiFormat::BC5_UNORM);
}

ER_TEST(DdsParse, Dxt10Bc7)
{
  auto file = makeDds(128, 128, 1, fourCC('D', 'X', '1', '0'), 0, 0, 0, 0, 0,
                      static_cast<uint32_t>(DxgiFormat::BC7_UNORM));
  DdsImage img{};
  ER_CHECK(parseDds(file, img) == DdsStatus::Ok);
  ER_CHECK(img.format == DxgiFormat::BC7_UNORM);
  ER_CHECK(img.blockCompressed);
  // Data offset is past the magic + header + DXT10 ext.
  ER_CHECK_EQ(img.dataOffset, DDS_MAGIC_SIZE + DDS_HEADER_SIZE + DDS_DXT10_SIZE);
}

ER_TEST(DdsParse, UncompressedBgra)
{
  // The Darwinia chrome/font sheet: 32-bit BGRA, mip count 1.
  auto file = makeDds(256, 224, 1, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
  DdsImage img{};
  ER_CHECK(parseDds(file, img) == DdsStatus::Ok);
  ER_CHECK(img.format == DxgiFormat::B8G8R8A8_UNORM);
  ER_CHECK(!img.blockCompressed);
  ER_CHECK_EQ(img.mipCount, 1u);
}

ER_TEST(DdsParse, UncompressedRgba)
{
  auto file = makeDds(8, 8, 1, 0, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
  DdsImage img{};
  ER_CHECK(parseDds(file, img) == DdsStatus::Ok);
  ER_CHECK(img.format == DxgiFormat::R8G8B8A8_UNORM);
}

ER_TEST(DdsParse, RejectsNotDds)
{
  std::vector<std::byte> garbage(160, std::byte{0x11});
  DdsImage img{};
  ER_CHECK(parseDds(garbage, img) == DdsStatus::NotDds);
}

ER_TEST(DdsParse, RejectsTruncatedHeader)
{
  auto file = makeDds(64, 64, 1, fourCC('D', 'X', 'T', '1'), 0, 0, 0, 0, 0);
  file.resize(40); // chop the header
  DdsImage img{};
  ER_CHECK(parseDds(file, img) == DdsStatus::Truncated);
}

ER_TEST(DdsParse, RejectsTruncatedDxt10Ext)
{
  // 'DX10' fourCC but the file ends right after the base header.
  auto file = makeDds(64, 64, 1, fourCC('D', 'X', '1', '0'), 0, 0, 0, 0, 0,
                      static_cast<uint32_t>(DxgiFormat::BC7_UNORM));
  file.resize(DDS_MAGIC_SIZE + DDS_HEADER_SIZE + 4); // partial ext
  DdsImage img{};
  ER_CHECK(parseDds(file, img) == DdsStatus::Truncated);
}

ER_TEST(DdsParse, RejectsUnsupportedRgbMasks)
{
  // 16-bit RGB (unsupported) → clean rejection, not a crash.
  auto file = makeDds(16, 16, 1, 0, 16, 0xf800, 0x07e0, 0x001f, 0x0000);
  DdsImage img{};
  ER_CHECK(parseDds(file, img) == DdsStatus::UnsupportedFormat);
}

ER_TEST(DdsParse, RejectsZeroDimensions)
{
  auto file = makeDds(0, 0, 1, fourCC('D', 'X', 'T', '1'), 0, 0, 0, 0, 0);
  DdsImage img{};
  ER_CHECK(parseDds(file, img) == DdsStatus::BadDimensions);
}
