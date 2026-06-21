#include "pch.h"
#include "CppUnitTest.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "CmoAnimation.h"
#include "CmoParse.h"
#include "DdsParse.h"
#include "FontAtlasLayout.h"
#include "UiLayout.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace er::format;

// NeuronRenderTest — pure parser/layout/animation logic mirrored from
// NeuronTools/testrunner (§16.2) so the same DDS/CMO/font/CmoAnimation/UiLayout
// cases also run in the Windows MSTest suite. Device-dependent loader tests
// (DdsLoader/CmoLoader upload paths) need a D3D12 device and are covered
// separately on a Windows agent.

namespace
{
  // --- byte-buffer builders (mirror NeuronTools/testrunner Fixtures.h) ---------

  // Little-endian byte writer for constructing synthetic DDS/CMO blobs.
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

    const std::vector<std::byte>& data() const { return m_bytes; }

  private:
    std::vector<std::byte> m_bytes;
  };

  uint32_t fourCC(char a, char b, char c, char d)
  {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
  }

  // DDS pixel-format flags.
  constexpr uint32_t DDPF_FOURCC = 0x4;
  constexpr uint32_t DDPF_RGB = 0x40;

  // Build a DDS file header (124-byte DDS_HEADER). If `fourCCv` is non-zero a
  // FOURCC pixel format is written; otherwise an RGB pixel format with the given
  // masks. `dxt10Format` (when non-zero with fourCC 'DX10') appends the ext.
  std::vector<std::byte> makeDds(uint32_t width, uint32_t height, uint32_t mips, uint32_t fourCCv,
                                 uint32_t rgbBitCount, uint32_t rMask, uint32_t gMask, uint32_t bMask,
                                 uint32_t aMask, uint32_t dxt10Format = 0, uint32_t payloadBytes = 0)
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
    w.u32(fourCCv != 0 ? DDPF_FOURCC : DDPF_RGB);
    w.u32(fourCCv);
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

  // --- CMO builders (mirror NeuronTools/testrunner CmoParseTests.cpp) ----------

  // Write a UTF-16 name as: UINT char count + count*2 zero bytes.
  void writeName(ByteWriter& w, uint32_t chars)
  {
    w.u32(chars);
    w.zeros(static_cast<size_t>(chars) * 2);
  }

  void writeMaterial(ByteWriter& w)
  {
    writeName(w, 4);            // material name
    w.zeros(CMO_MATERIAL_SIZE); // Material struct
    writeName(w, 0);            // pixel-shader name
    for (uint32_t t = 0; t < CMO_MAX_TEXTURES; ++t)
      writeName(w, 0);          // texture names
  }

  // A single static mesh: 1 material, 2 submeshes, 1 index buffer (6 idx),
  // 1 vertex buffer (4 verts), no skinning, no skeleton.
  std::vector<std::byte> makeStaticCmo()
  {
    ByteWriter w;
    w.u32(1); // mesh count

    writeName(w, 4); // mesh name "ship"
    w.u32(1);        // material count
    writeMaterial(w);
    w.u8(0); // no skeletal animation

    w.u32(2); // submesh count
    for (int i = 0; i < 2; ++i)
    {
      w.u32(0); // materialIndex
      w.u32(0); // indexBufferIndex
      w.u32(0); // vertexBufferIndex
      w.u32(static_cast<uint32_t>(i * 3)); // startIndex
      w.u32(1); // primCount
    }

    w.u32(1); // index buffer count
    w.u32(6); // 6 indices
    w.zeros(6 * sizeof(uint16_t));

    w.u32(1); // vertex buffer count
    w.u32(4); // 4 verts
    w.zeros(4 * CMO_VERTEX_SIZE);

    w.u32(0); // skinning vertex buffer count

    w.zeros(CMO_EXTENTS_SIZE); // MeshExtents
    return w.data();
  }

  void writeIdentity(ByteWriter& w)
  {
    for (int i = 0; i < 16; ++i)
      w.f32((i % 5 == 0) ? 1.0f : 0.0f); // diagonal 0,5,10,15
  }

  // A skinned mesh: 2 bones, 1 skinning VB (3 verts), 1 clip with 2 keyframes
  // animating bone 1 from identity to a +10 X translation.
  std::vector<std::byte> makeAnimatedCmo()
  {
    ByteWriter w;
    w.u32(1);        // mesh count
    writeName(w, 4); // mesh name
    w.u32(1);
    writeMaterial(w);
    w.u8(1); // HAS skeletal animation

    w.u32(1); // 1 submesh
    w.u32(0);
    w.u32(0);
    w.u32(0);
    w.u32(0);
    w.u32(1);

    w.u32(1); // 1 index buffer, 3 indices
    w.u32(3);
    w.zeros(3 * sizeof(uint16_t));

    w.u32(1); // 1 vertex buffer, 3 verts
    w.u32(3);
    w.zeros(3 * CMO_VERTEX_SIZE);

    w.u32(1); // 1 skinning vertex buffer, 3 verts
    w.u32(3);
    for (int v = 0; v < 3; ++v)
    {
      w.u32(0);
      w.u32(1);
      w.u32(0);
      w.u32(0); // bone indices
      w.f32(0.5f);
      w.f32(0.5f);
      w.f32(0.0f);
      w.f32(0.0f); // weights
    }

    // MeshExtents: center (0,0,0), radius 5, min, max.
    w.f32(0);
    w.f32(0);
    w.f32(0);
    w.f32(5.0f);
    w.f32(-5);
    w.f32(-5);
    w.f32(-5);
    w.f32(5);
    w.f32(5);
    w.f32(5);

    // Skeleton: 2 bones (root + child).
    w.u32(2);
    writeName(w, 4);
    w.u32(0xFFFFFFFFu); // bone0 parent = -1
    writeIdentity(w);
    writeIdentity(w);
    writeIdentity(w);
    writeName(w, 4);
    w.u32(0); // bone1 parent = 0
    writeIdentity(w);
    writeIdentity(w);
    writeIdentity(w);

    // 1 animation clip, 2 keyframes for bone 1.
    w.u32(1);
    writeName(w, 4);
    w.f32(0.0f); // start
    w.f32(1.0f); // end
    w.u32(2);    // 2 keyframes
    w.u32(1);
    w.f32(0.0f);
    writeIdentity(w); // kf0: bone1 @ t=0 identity
    w.u32(1);
    w.f32(1.0f);
    for (int i = 0; i < 16; ++i) // kf1: bone1 @ t=1 translate X=10
      w.f32(i == 12 ? 10.0f : ((i % 5 == 0) ? 1.0f : 0.0f));
    return w.data();
  }

  bool nearly(float a, float b, float eps) { return std::fabs(a - b) < eps; }

  // The reference EditorFont atlas.
  FontAtlasConfig editorFont() { return FontAtlasConfig{16, 14, 32, 16, 256, 224}; }
} // namespace

namespace NeuronRenderTest
{
  // --- DDS header parser ------------------------------------------------------

  TEST_CLASS(DdsParseTests)
  {
  public:
    TEST_METHOD(Dxt1IsBc1)
    {
      auto file = makeDds(256, 256, 9, fourCC('D', 'X', 'T', '1'), 0, 0, 0, 0, 0);
      DdsImage img{};
      Assert::IsTrue(parseDds(file, img) == DdsStatus::Ok);
      Assert::IsTrue(img.format == DxgiFormat::BC1_UNORM);
      Assert::IsTrue(img.blockCompressed);
      Assert::IsTrue(img.width == 256u);
      Assert::IsTrue(img.height == 256u);
      Assert::IsTrue(img.mipCount == 9u);
    }

    TEST_METHOD(Dxt3IsBc2_Dxt5IsBc3)
    {
      DdsImage img{};
      auto dxt3 = makeDds(64, 64, 1, fourCC('D', 'X', 'T', '3'), 0, 0, 0, 0, 0);
      Assert::IsTrue(parseDds(dxt3, img) == DdsStatus::Ok);
      Assert::IsTrue(img.format == DxgiFormat::BC2_UNORM);

      auto dxt5 = makeDds(64, 64, 1, fourCC('D', 'X', 'T', '5'), 0, 0, 0, 0, 0);
      Assert::IsTrue(parseDds(dxt5, img) == DdsStatus::Ok);
      Assert::IsTrue(img.format == DxgiFormat::BC3_UNORM);
    }

    TEST_METHOD(Ati1IsBc4_Ati2IsBc5)
    {
      DdsImage img{};
      auto bc4 = makeDds(32, 32, 1, fourCC('A', 'T', 'I', '1'), 0, 0, 0, 0, 0);
      Assert::IsTrue(parseDds(bc4, img) == DdsStatus::Ok);
      Assert::IsTrue(img.format == DxgiFormat::BC4_UNORM);

      auto bc5 = makeDds(32, 32, 1, fourCC('A', 'T', 'I', '2'), 0, 0, 0, 0, 0);
      Assert::IsTrue(parseDds(bc5, img) == DdsStatus::Ok);
      Assert::IsTrue(img.format == DxgiFormat::BC5_UNORM);
    }

    TEST_METHOD(Dxt10Bc7)
    {
      auto file = makeDds(128, 128, 1, fourCC('D', 'X', '1', '0'), 0, 0, 0, 0, 0,
                          static_cast<uint32_t>(DxgiFormat::BC7_UNORM));
      DdsImage img{};
      Assert::IsTrue(parseDds(file, img) == DdsStatus::Ok);
      Assert::IsTrue(img.format == DxgiFormat::BC7_UNORM);
      Assert::IsTrue(img.blockCompressed);
      // Data offset is past the magic + header + DXT10 ext.
      Assert::IsTrue(img.dataOffset == DDS_MAGIC_SIZE + DDS_HEADER_SIZE + DDS_DXT10_SIZE);
    }

    TEST_METHOD(UncompressedBgra)
    {
      // The Darwinia chrome/font sheet: 32-bit BGRA, mip count 1.
      auto file = makeDds(256, 224, 1, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
      DdsImage img{};
      Assert::IsTrue(parseDds(file, img) == DdsStatus::Ok);
      Assert::IsTrue(img.format == DxgiFormat::B8G8R8A8_UNORM);
      Assert::IsTrue(!img.blockCompressed);
      Assert::IsTrue(img.mipCount == 1u);
    }

    TEST_METHOD(UncompressedRgba)
    {
      auto file = makeDds(8, 8, 1, 0, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
      DdsImage img{};
      Assert::IsTrue(parseDds(file, img) == DdsStatus::Ok);
      Assert::IsTrue(img.format == DxgiFormat::R8G8B8A8_UNORM);
    }

    TEST_METHOD(RejectsNotDds)
    {
      std::vector<std::byte> garbage(160, std::byte{0x11});
      DdsImage img{};
      Assert::IsTrue(parseDds(garbage, img) == DdsStatus::NotDds);
    }

    TEST_METHOD(RejectsTruncatedHeader)
    {
      auto file = makeDds(64, 64, 1, fourCC('D', 'X', 'T', '1'), 0, 0, 0, 0, 0);
      file.resize(40); // chop the header
      DdsImage img{};
      Assert::IsTrue(parseDds(file, img) == DdsStatus::Truncated);
    }

    TEST_METHOD(RejectsTruncatedDxt10Ext)
    {
      // 'DX10' fourCC but the file ends right after the base header.
      auto file = makeDds(64, 64, 1, fourCC('D', 'X', '1', '0'), 0, 0, 0, 0, 0,
                          static_cast<uint32_t>(DxgiFormat::BC7_UNORM));
      file.resize(DDS_MAGIC_SIZE + DDS_HEADER_SIZE + 4); // partial ext
      DdsImage img{};
      Assert::IsTrue(parseDds(file, img) == DdsStatus::Truncated);
    }

    TEST_METHOD(RejectsUnsupportedRgbMasks)
    {
      // 16-bit RGB (unsupported) -> clean rejection, not a crash.
      auto file = makeDds(16, 16, 1, 0, 16, 0xf800, 0x07e0, 0x001f, 0x0000);
      DdsImage img{};
      Assert::IsTrue(parseDds(file, img) == DdsStatus::UnsupportedFormat);
    }

    TEST_METHOD(RejectsZeroDimensions)
    {
      auto file = makeDds(0, 0, 1, fourCC('D', 'X', 'T', '1'), 0, 0, 0, 0, 0);
      DdsImage img{};
      Assert::IsTrue(parseDds(file, img) == DdsStatus::BadDimensions);
    }

    TEST_METHOD(SubresourceBc1SingleMip)
    {
      // BC1 8x8: 2x2 blocks x 8 bytes = rowPitch 16, 2 block-rows, 32-byte slice.
      auto file = makeDds(8, 8, 1, fourCC('D', 'X', 'T', '1'), 0, 0, 0, 0, 0, 0, /*payload*/ 32);
      DdsImage img{};
      Assert::IsTrue(parseDds(file, img) == DdsStatus::Ok);
      std::vector<DdsSubresource> subs;
      Assert::IsTrue(enumerateDdsSubresources(img, file.size(), subs));
      Assert::IsTrue(subs.size() == static_cast<size_t>(1));
      Assert::IsTrue(subs[0].rowPitch == 16u);
      Assert::IsTrue(subs[0].numRows == 2u);
      Assert::IsTrue(subs[0].slicePitch == static_cast<size_t>(32));
      Assert::IsTrue(subs[0].offset == DDS_MAGIC_SIZE + DDS_HEADER_SIZE);
    }

    TEST_METHOD(SubresourceMipChainOffsets)
    {
      // BC1 8x8 with 4 mips (8,4,2,1) -> 32 + 8 + 8 + 8 = 56 payload bytes.
      auto file = makeDds(8, 8, 4, fourCC('D', 'X', 'T', '1'), 0, 0, 0, 0, 0, 0, /*payload*/ 56);
      DdsImage img{};
      Assert::IsTrue(parseDds(file, img) == DdsStatus::Ok);
      std::vector<DdsSubresource> subs;
      Assert::IsTrue(enumerateDdsSubresources(img, file.size(), subs));
      Assert::IsTrue(subs.size() == static_cast<size_t>(4));
      const size_t base = DDS_MAGIC_SIZE + DDS_HEADER_SIZE;
      Assert::IsTrue(subs[0].offset == base);
      Assert::IsTrue(subs[1].offset == base + 32); // mip 0 is 32 bytes
      Assert::IsTrue(subs[3].width == 1u);
      Assert::IsTrue(subs[3].height == 1u);
    }

    TEST_METHOD(SubresourceUncompressedBgra)
    {
      // 4x4 BGRA32: rowPitch 16, 4 rows, 64-byte slice.
      auto file = makeDds(4, 4, 1, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000, 0, 64);
      DdsImage img{};
      Assert::IsTrue(parseDds(file, img) == DdsStatus::Ok);
      std::vector<DdsSubresource> subs;
      Assert::IsTrue(enumerateDdsSubresources(img, file.size(), subs));
      Assert::IsTrue(subs.size() == static_cast<size_t>(1));
      Assert::IsTrue(subs[0].rowPitch == 16u);
      Assert::IsTrue(subs[0].numRows == 4u);
    }

    TEST_METHOD(SubresourceRejectsTruncatedPayload)
    {
      // Declares a mip but provides no payload -> enumeration fails cleanly.
      auto file = makeDds(8, 8, 1, fourCC('D', 'X', 'T', '1'), 0, 0, 0, 0, 0, 0, /*payload*/ 0);
      DdsImage img{};
      Assert::IsTrue(parseDds(file, img) == DdsStatus::Ok);
      std::vector<DdsSubresource> subs;
      Assert::IsTrue(!enumerateDdsSubresources(img, file.size(), subs));
    }
  };

  // --- Font-atlas layout ------------------------------------------------------

  TEST_CLASS(FontAtlasTests)
  {
  public:
    TEST_METHOD(RangeMetrics)
    {
      auto cfg = editorFont();
      Assert::IsTrue(cfg.cellCount() == 224u);
      Assert::IsTrue(cfg.firstCodepoint == 32u);
      Assert::IsTrue(cfg.lastCodepoint() == 255u); // 32 + 224 - 1
      Assert::IsTrue(cfg.inRange(32));
      Assert::IsTrue(cfg.inRange(255));
      Assert::IsTrue(!cfg.inRange(31));
      Assert::IsTrue(!cfg.inRange(256));
    }

    TEST_METHOD(SpaceIsCellZero)
    {
      auto cfg = editorFont();
      Assert::IsTrue(glyphCellIndex(cfg, 32) == 0u);
      auto uv = glyphUv(cfg, 32);
      Assert::IsTrue(nearly(uv.u0, 0.0f, 1e-5f));
      Assert::IsTrue(nearly(uv.v0, 0.0f, 1e-5f));
      Assert::IsTrue(nearly(uv.u1, 16.0f / 256.0f, 1e-5f));
      Assert::IsTrue(nearly(uv.v1, 16.0f / 224.0f, 1e-5f));
    }

    TEST_METHOD(LetterAUv)
    {
      // 'A' = cp 65 -> cell index 33 -> col 1, row 2 (16 cols).
      auto cfg = editorFont();
      Assert::IsTrue(glyphCellIndex(cfg, 65) == 33u);
      auto uv = glyphUv(cfg, 65);
      Assert::IsTrue(nearly(uv.u0, 1.0f * 16.0f / 256.0f, 1e-5f));
      Assert::IsTrue(nearly(uv.v0, 2.0f * 16.0f / 224.0f, 1e-5f));
      Assert::IsTrue(nearly(uv.u1, 2.0f * 16.0f / 256.0f, 1e-5f));
      Assert::IsTrue(nearly(uv.v1, 3.0f * 16.0f / 224.0f, 1e-5f));
    }

    TEST_METHOD(LastCodepoint)
    {
      // cp 255 -> cell 223 -> col 15, row 13 (the bottom-right cell).
      auto cfg = editorFont();
      Assert::IsTrue(glyphCellIndex(cfg, 255) == 223u);
      auto uv = glyphUv(cfg, 255);
      Assert::IsTrue(nearly(uv.u1, 1.0f, 1e-5f)); // 16 * 16 / 256
      Assert::IsTrue(nearly(uv.v1, 1.0f, 1e-5f)); // 14 * 16 / 224
    }

    TEST_METHOD(OutOfRangeClampsToBlank)
    {
      auto cfg = editorFont();
      // Out-of-range codepoints fall back to cell 0 (space) -> blank glyph.
      Assert::IsTrue(glyphCellIndex(cfg, 0) == 0u);
      Assert::IsTrue(glyphCellIndex(cfg, 0x3042) == 0u); // a CJK codepoint outside the atlas
      auto uv = glyphUv(cfg, 0x3042);
      Assert::IsTrue(nearly(uv.u0, 0.0f, 1e-5f));
      Assert::IsTrue(nearly(uv.v0, 0.0f, 1e-5f));
    }
  };

  // --- CMO structural parser --------------------------------------------------

  TEST_CLASS(CmoParseTests)
  {
  public:
    TEST_METHOD(StaticMeshCounts)
    {
      auto file = makeStaticCmo();
      CmoModel model{};
      Assert::IsTrue(parseCmo(file, model) == CmoStatus::Ok);
      Assert::IsTrue(model.meshes.size() == static_cast<size_t>(1));

      const auto& mesh = model.meshes[0];
      Assert::IsTrue(mesh.materialCount == 1u);
      Assert::IsTrue(mesh.submeshCount == 2u);
      Assert::IsTrue(mesh.submeshes.size() == static_cast<size_t>(2));
      Assert::IsTrue(mesh.indexBufferCount == 1u);
      Assert::IsTrue(mesh.totalIndices == 6u);
      Assert::IsTrue(mesh.vertexBufferCount == 1u);
      Assert::IsTrue(mesh.totalVertices == 4u);
      Assert::IsTrue(!mesh.hasSkeletalAnimation);
      Assert::IsTrue(mesh.submeshes[1].startIndex == 3u);
    }

    TEST_METHOD(ExtractsBufferData)
    {
      auto file = makeStaticCmo();
      CmoModel model{};
      Assert::IsTrue(parseCmo(file, model) == CmoStatus::Ok);
      const auto& mesh = model.meshes[0];

      // Materials captured; the synthetic material has no diffuse texture.
      Assert::IsTrue(mesh.materials.size() == static_cast<size_t>(1));
      Assert::IsTrue(mesh.materials[0].diffuseTexture.empty());

      // Vertex/index payloads aliased with the right sizes (52 B/vert, 2 B/index).
      Assert::IsTrue(mesh.vertexData.size() == static_cast<size_t>(1));
      Assert::IsTrue(mesh.vertexCounts[0] == 4u);
      Assert::IsTrue(mesh.vertexData[0].size() == static_cast<size_t>(4 * 52));
      Assert::IsTrue(mesh.indexData.size() == static_cast<size_t>(1));
      Assert::IsTrue(mesh.indexCounts[0] == 6u);
      Assert::IsTrue(mesh.indexData[0].size() == static_cast<size_t>(6 * 2));
    }

    TEST_METHOD(ExtractsSkeletonAndAnimation)
    {
      auto file = makeAnimatedCmo();
      CmoModel model{};
      Assert::IsTrue(parseCmo(file, model) == CmoStatus::Ok);
      const auto& mesh = model.meshes[0];

      Assert::IsTrue(mesh.hasSkeletalAnimation);
      Assert::IsTrue(mesh.isSkinned());

      // Skeleton: 2 bones, child references the root.
      Assert::IsTrue(mesh.bones.size() == static_cast<size_t>(2));
      Assert::IsTrue(mesh.bones[0].parentIndex == -1);
      Assert::IsTrue(mesh.bones[1].parentIndex == 0);

      // Skinning vertices: 1 buffer of 3, each weighted to bone 1 @ 0.5.
      Assert::IsTrue(mesh.skinningVertices.size() == static_cast<size_t>(1));
      Assert::IsTrue(mesh.skinningVertices[0].size() == static_cast<size_t>(3));
      Assert::IsTrue(mesh.skinningVertices[0][0].boneIndices[1] == 1u);
      Assert::IsTrue(mesh.skinningVertices[0][0].boneWeights[0] > 0.49f &&
                     mesh.skinningVertices[0][0].boneWeights[0] < 0.51f);

      // Animation: 1 clip, 2 keyframes; kf1 carries the +10 X translation.
      Assert::IsTrue(mesh.animations.size() == static_cast<size_t>(1));
      Assert::IsTrue(mesh.animations[0].endTime > 0.99f);
      Assert::IsTrue(mesh.animations[0].keyframes.size() == static_cast<size_t>(2));
      Assert::IsTrue(mesh.animations[0].keyframes[1].boneIndex == 1u);
      Assert::IsTrue(mesh.animations[0].keyframes[1].transform[12] > 9.9f);
      Assert::IsTrue(mesh.boundingRadius > 4.9f && mesh.boundingRadius < 5.1f);
    }

    TEST_METHOD(EmptyModel)
    {
      ByteWriter w;
      w.u32(0); // zero meshes
      CmoModel model{};
      Assert::IsTrue(parseCmo(w.data(), model) == CmoStatus::Ok);
      Assert::IsTrue(model.meshes.size() == static_cast<size_t>(0));
    }

    TEST_METHOD(RejectsTruncated)
    {
      auto file = makeStaticCmo();
      file.resize(file.size() - 8); // chop the trailing extents
      CmoModel model{};
      Assert::IsTrue(parseCmo(file, model) == CmoStatus::Truncated);
    }

    TEST_METHOD(RejectsGarbageMeshCount)
    {
      // A huge mesh count with no payload must be rejected, not allocated against.
      ByteWriter w;
      w.u32(0xffffffffu);
      CmoModel model{};
      auto status = parseCmo(w.data(), model);
      Assert::IsTrue(status == CmoStatus::TooLarge || status == CmoStatus::Truncated);
    }

    TEST_METHOD(RejectsGarbageIndexCount)
    {
      // Valid up to the index buffer, then an absurd index count.
      ByteWriter w;
      w.u32(1);        // mesh count
      writeName(w, 0); // mesh name
      w.u32(0);        // material count
      w.u8(0);         // no skeleton
      w.u32(0);        // submesh count
      w.u32(1);        // index buffer count
      w.u32(0xffffffffu); // absurd index count
      CmoModel model{};
      auto status = parseCmo(w.data(), model);
      Assert::IsTrue(status == CmoStatus::TooLarge || status == CmoStatus::Truncated);
    }

    TEST_METHOD(EmptyBufferRejected)
    {
      std::vector<std::byte> empty;
      CmoModel model{};
      Assert::IsTrue(parseCmo(empty, model) == CmoStatus::Truncated);
    }
  };

  // --- Skeletal-animation math ------------------------------------------------

  TEST_CLASS(CmoAnimationTests)
  {
  public:
    TEST_METHOD(MatrixMultiplyByIdentity)
    {
      Mat4 m = mat4Identity();
      m[12] = 3.0f; // translation X
      m[13] = 4.0f; // translation Y
      Mat4 r = mat4Multiply(m, mat4Identity());
      for (int i = 0; i < 16; ++i)
        Assert::IsTrue(nearly(r[i], m[i], 1e-4f));
    }

    TEST_METHOD(SamplePoseInterpolatesKeyframes)
    {
      std::vector<CmoBone> bones(1);
      CmoAnimationClip clip;
      clip.startTime = 0.0f;
      clip.endTime = 1.0f;
      CmoKeyframe k0;
      k0.boneIndex = 0;
      k0.time = 0.0f;
      k0.transform = mat4Identity();
      CmoKeyframe k1;
      k1.boneIndex = 0;
      k1.time = 1.0f;
      k1.transform = mat4Identity();
      k1.transform[12] = 10.0f; // translate X 10
      clip.keyframes = {k0, k1};

      auto pose = samplePose(bones, clip, 0.5f);
      Assert::IsTrue(pose.size() == static_cast<size_t>(1));
      Assert::IsTrue(nearly(pose[0][12], 5.0f, 1e-4f)); // halfway

      auto poseStart = samplePose(bones, clip, 0.0f);
      Assert::IsTrue(nearly(poseStart[0][12], 0.0f, 1e-4f));
      auto poseEnd = samplePose(bones, clip, 1.0f);
      Assert::IsTrue(nearly(poseEnd[0][12], 10.0f, 1e-4f));
    }

    TEST_METHOD(SkinningPaletteAccumulatesParent)
    {
      std::vector<CmoBone> bones(2);
      bones[0].parentIndex = -1;
      bones[0].invBindPose = mat4Identity();
      bones[1].parentIndex = 0;
      bones[1].invBindPose = mat4Identity();

      std::vector<Mat4> local(2, mat4Identity());
      local[0][12] = 2.0f; // root translate X 2
      local[1][12] = 3.0f; // child translate X 3 (local to parent)

      auto palette = computeSkinningPalette(bones, local);
      Assert::IsTrue(palette.size() == static_cast<size_t>(2));
      Assert::IsTrue(nearly(palette[0][12], 2.0f, 1e-4f));
      Assert::IsTrue(nearly(palette[1][12], 5.0f, 1e-4f)); // child world translation = 2 + 3
    }

    TEST_METHOD(BoneWithoutKeysUsesDefaultLocal)
    {
      std::vector<CmoBone> bones(1);
      bones[0].localTransform = mat4Identity();
      bones[0].localTransform[14] = 7.0f; // default Z translation
      CmoAnimationClip clip; // no keyframes for bone 0
      clip.endTime = 1.0f;
      auto pose = samplePose(bones, clip, 0.5f);
      Assert::IsTrue(nearly(pose[0][14], 7.0f, 1e-4f));
    }
  };

  // --- Darwinia menu UI layout ------------------------------------------------

  TEST_CLASS(UiLayoutTests)
  {
  public:
    TEST_METHOD(RectContains)
    {
      using namespace Neuron::Render::Ui;
      Rect r{ 10, 20, 100, 40 };
      Assert::IsTrue(r.Contains(10, 20));      // top-left inclusive
      Assert::IsTrue(r.Contains(59, 39));
      Assert::IsTrue(!r.Contains(9, 20));      // left of
      Assert::IsTrue(!r.Contains(110, 40));    // right edge exclusive
      Assert::IsTrue(!r.Contains(50, 60));     // bottom edge exclusive
    }

    TEST_METHOD(MainMenuButtonCountIncludesClose)
    {
      using namespace Neuron::Render::Ui;
      const auto L = BuildMainMenu(0.f, 0.f, 1.f, 6); // 6 list items + Close
      Assert::IsTrue(L.count == 7);
    }

    TEST_METHOD(ButtonsStackInsideWindowBelowTitle)
    {
      using namespace Neuron::Render::Ui;
      const float s = 1.f;
      const auto L = BuildMainMenu(100.f, 50.f, s, 6);
      const MenuMetrics m = MenuMetrics::At(s);

      // Window top-left honoured; title bar spans the window width.
      Assert::IsTrue(nearly(L.window.x, 100.f, 1e-3f) && nearly(L.window.y, 50.f, 1e-3f));
      Assert::IsTrue(nearly(L.titleBar.h, m.titleH, 1e-3f));

      // Every button sits inside the window, below the title bar, and is ordered
      // strictly top-to-bottom (no overlap).
      float prevBottom = L.titleBar.y + L.titleBar.h;
      for (int i = 0; i < L.count; ++i)
      {
        const Rect& b = L.buttons[i];
        Assert::IsTrue(b.y >= prevBottom - 1e-3f);
        Assert::IsTrue(b.x >= L.window.x && b.x + b.w <= L.window.x + L.window.w + 1e-3f);
        Assert::IsTrue(b.y + b.h <= L.window.y + L.window.h + 1e-3f);
        prevBottom = b.y + b.h;
      }

      // The trailing Close has an extra gap above it (separated from the list).
      const float listGap = L.buttons[1].y - (L.buttons[0].y + L.buttons[0].h);
      const float closeGap = L.buttons[6].y - (L.buttons[5].y + L.buttons[5].h);
      Assert::IsTrue(closeGap > listGap + 1e-3f);
    }

    TEST_METHOD(CloseBoxInTitleBarTopRight)
    {
      using namespace Neuron::Render::Ui;
      const auto L = BuildMainMenu(0.f, 0.f, 1.f, 6);
      // Close box is within the title bar and toward the right edge.
      Assert::IsTrue(L.closeBox.y >= L.titleBar.y - 1e-3f);
      Assert::IsTrue(L.closeBox.y + L.closeBox.h <= L.titleBar.y + L.titleBar.h + 1e-3f);
      Assert::IsTrue(L.closeBox.x > L.window.x + L.window.w * 0.5f);
    }

    TEST_METHOD(CenterPlacesWindowOnScreen)
    {
      using namespace Neuron::Render::Ui;
      float gx = 0, gy = 0;
      CenterMainMenu(1920.f, 1080.f, 1.f, 6, gx, gy);
      const auto L = BuildMainMenu(gx, gy, 1.f, 6);
      // Horizontally centred; fully on-screen vertically.
      Assert::IsTrue(nearly(L.window.x + L.window.w * 0.5f, 960.f, 1e-3f));
      Assert::IsTrue(L.window.y > 0.f);
      Assert::IsTrue(L.window.y + L.window.h < 1080.f);
    }

    TEST_METHOD(PanelRowsAndFooterInsideWindow)
    {
      using namespace Neuron::Render::Ui;
      const float s = 1.f;
      const auto L = BuildPanel(200.f, 100.f, s, 460.f, 7, /*footer*/ true);
      Assert::IsTrue(L.rowCount == 7);
      Assert::IsTrue(L.hasFooter);

      float prevBottom = L.titleBar.y + L.titleBar.h;
      for (int i = 0; i < L.rowCount; ++i)
      {
        const Rect& r = L.rows[i];
        Assert::IsTrue(r.y >= prevBottom - 1e-3f);
        Assert::IsTrue(r.x >= L.window.x && r.x + r.w <= L.window.x + L.window.w + 1e-3f);
        prevBottom = r.y + r.h;
      }
      // Footer buttons are side-by-side, inside the window, below the last row.
      Assert::IsTrue(L.footerClose.y >= prevBottom - 1e-3f);
      Assert::IsTrue(L.footerApply.x > L.footerClose.x);
      Assert::IsTrue(L.footerApply.x + L.footerApply.w <= L.window.x + L.window.w + 1e-3f);
      Assert::IsTrue(L.footerClose.y + L.footerClose.h <= L.window.y + L.window.h + 1e-3f);
    }

    TEST_METHOD(ValueBoxAndPopupGeometry)
    {
      using namespace Neuron::Render::Ui;
      const auto L = BuildPanel(0.f, 0.f, 1.f, 460.f, 3, false);
      const Rect row = L.rows[0];
      const Rect vb = ValueBox(row);
      // Value box is the right side of the row, fully inside it.
      Assert::IsTrue(vb.x > row.x);
      Assert::IsTrue(nearly(vb.x + vb.w, row.x + row.w, 1e-3f));
      // Arrow square is at the right end of the value box.
      const Rect ar = ArrowBox(vb);
      Assert::IsTrue(nearly(ar.x + ar.w, vb.x + vb.w, 1e-3f));
      Assert::IsTrue(nearly(ar.w, vb.h, 1e-3f));
      // Popup rows stack directly under the value box.
      const Rect p0 = PopupRow(vb, 0);
      const Rect p1 = PopupRow(vb, 1);
      Assert::IsTrue(nearly(p0.y, vb.y + vb.h, 1e-3f));
      Assert::IsTrue(nearly(p1.y, vb.y + 2 * vb.h, 1e-3f));
      const Rect panel = PopupPanel(vb, 4);
      Assert::IsTrue(nearly(panel.h, 4 * vb.h, 1e-3f));
    }
  };
} // namespace NeuronRenderTest
