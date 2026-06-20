#include "pch.h"
#include "CppUnitTest.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "CmoParse.h"
#include "DdsParse.h"
#include "FontAtlasLayout.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace er::format;

// NeuronRenderTest — pure parser logic mirrored from NeuronTools/testrunner
// (§16.2) so the same DDS/CMO/font cases also run in the Windows MSTest suite.
// Device-dependent loader tests (DdsLoader/CmoLoader upload paths) need a D3D12
// device and are covered separately on a Windows agent.

namespace
{
  uint32_t fourCC(char a, char b, char c, char d)
  {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
  }

  void pushU32(std::vector<std::byte>& b, uint32_t v)
  {
    for (int i = 0; i < 4; ++i)
      b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xff));
  }

  std::vector<std::byte> makeDds(uint32_t w, uint32_t h, uint32_t mips, uint32_t cc,
                                 uint32_t bitCount, uint32_t rM, uint32_t gM, uint32_t bM,
                                 uint32_t aM, uint32_t payload)
  {
    std::vector<std::byte> b;
    pushU32(b, fourCC('D', 'D', 'S', ' '));
    pushU32(b, 124);
    pushU32(b, 0x1007);
    pushU32(b, h);
    pushU32(b, w);
    pushU32(b, 0);
    pushU32(b, 0);
    pushU32(b, mips);
    for (int i = 0; i < 11; ++i)
      pushU32(b, 0);
    pushU32(b, 32);
    pushU32(b, cc != 0 ? 0x4u : 0x40u);
    pushU32(b, cc);
    pushU32(b, bitCount);
    pushU32(b, rM);
    pushU32(b, gM);
    pushU32(b, bM);
    pushU32(b, aM);
    pushU32(b, 0x1000);
    pushU32(b, 0);
    pushU32(b, 0);
    pushU32(b, 0);
    pushU32(b, 0);
    b.insert(b.end(), payload, std::byte{0});
    return b;
  }
} // namespace

namespace NeuronRenderTest
{
  TEST_CLASS(DdsParseTests)
  {
  public:
    TEST_METHOD(Dxt1IsBc1WithMips)
    {
      auto file = makeDds(256, 256, 9, fourCC('D', 'X', 'T', '1'), 0, 0, 0, 0, 0, 0);
      DdsImage img{};
      Assert::IsTrue(parseDds(file, img) == DdsStatus::Ok);
      Assert::IsTrue(img.format == DxgiFormat::BC1_UNORM);
      Assert::IsTrue(img.blockCompressed);
      Assert::AreEqual<uint32_t>(9, img.mipCount);
    }

    TEST_METHOD(UncompressedBgra)
    {
      auto file = makeDds(256, 224, 1, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000, 0);
      DdsImage img{};
      Assert::IsTrue(parseDds(file, img) == DdsStatus::Ok);
      Assert::IsTrue(img.format == DxgiFormat::B8G8R8A8_UNORM);
    }

    TEST_METHOD(RejectsGarbage)
    {
      std::vector<std::byte> garbage(160, std::byte{0x11});
      DdsImage img{};
      Assert::IsTrue(parseDds(garbage, img) == DdsStatus::NotDds);
    }

    TEST_METHOD(SubresourceMipChain)
    {
      auto file = makeDds(8, 8, 4, fourCC('D', 'X', 'T', '1'), 0, 0, 0, 0, 0, 56);
      DdsImage img{};
      Assert::IsTrue(parseDds(file, img) == DdsStatus::Ok);
      std::vector<DdsSubresource> subs;
      Assert::IsTrue(enumerateDdsSubresources(img, file.size(), subs));
      Assert::AreEqual<size_t>(4, subs.size());
      Assert::AreEqual<uint32_t>(16, subs[0].rowPitch);
    }
  };

  TEST_CLASS(FontAtlasTests)
  {
  public:
    TEST_METHOD(EditorFontRangeAndUv)
    {
      FontAtlasConfig cfg{16, 14, 32, 16, 256, 224};
      Assert::AreEqual<uint32_t>(224, cfg.cellCount());
      Assert::AreEqual<uint32_t>(255, cfg.lastCodepoint());
      Assert::AreEqual<uint32_t>(33, glyphCellIndex(cfg, 65)); // 'A'
      auto uv = glyphUv(cfg, 65);
      Assert::AreEqual(1.0f * 16.0f / 256.0f, uv.u0, 1e-5f);
    }

    TEST_METHOD(OutOfRangeClampsBlank)
    {
      FontAtlasConfig cfg{16, 14, 32, 16, 256, 224};
      Assert::AreEqual<uint32_t>(0, glyphCellIndex(cfg, 0x3042));
    }
  };

  TEST_CLASS(CmoParseTests)
  {
  public:
    TEST_METHOD(RejectsTruncated)
    {
      // A lone huge mesh count with no payload must be rejected, not crash.
      std::vector<std::byte> b;
      pushU32(b, 0xffffffffu);
      CmoModel model{};
      auto status = parseCmo(b, model);
      Assert::IsTrue(status == CmoStatus::TooLarge || status == CmoStatus::Truncated);
    }

    TEST_METHOD(EmptyModelOk)
    {
      std::vector<std::byte> b;
      pushU32(b, 0);
      CmoModel model{};
      Assert::IsTrue(parseCmo(b, model) == CmoStatus::Ok);
      Assert::AreEqual<size_t>(0, model.meshes.size());
    }
  };
} // namespace NeuronRenderTest
