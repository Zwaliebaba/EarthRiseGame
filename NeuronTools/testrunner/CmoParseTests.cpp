// CMO structural parser tests (mirror NeuronRenderTest, M2 area B gate:
// "submesh/material/index counts on a known mesh; malformed → rejected").

#include "../CmoParse.h"
#include "Fixtures.h"
#include "TestRunner.h"

using namespace er::format;
using namespace ertest;

namespace
{
  // Write a UTF-16 name as: UINT char count + count*2 zero bytes (content
  // irrelevant to the structural walk).
  void writeName(ByteWriter& w, uint32_t chars)
  {
    w.u32(chars);
    w.zeros(static_cast<size_t>(chars) * 2);
  }

  void writeMaterial(ByteWriter& w)
  {
    writeName(w, 4);                  // material name
    w.zeros(CMO_MATERIAL_SIZE);       // Material struct
    writeName(w, 0);                  // pixel-shader name
    for (uint32_t t = 0; t < CMO_MAX_TEXTURES; ++t)
      writeName(w, 0);                // texture names
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
} // namespace

ER_TEST(CmoParse, StaticMeshCounts)
{
  auto file = makeStaticCmo();
  CmoModel model{};
  ER_CHECK(parseCmo(file, model) == CmoStatus::Ok);
  ER_CHECK_EQ(model.meshes.size(), static_cast<size_t>(1));

  const auto& mesh = model.meshes[0];
  ER_CHECK_EQ(mesh.materialCount, 1u);
  ER_CHECK_EQ(mesh.submeshCount, 2u);
  ER_CHECK_EQ(mesh.submeshes.size(), static_cast<size_t>(2));
  ER_CHECK_EQ(mesh.indexBufferCount, 1u);
  ER_CHECK_EQ(mesh.totalIndices, 6u);
  ER_CHECK_EQ(mesh.vertexBufferCount, 1u);
  ER_CHECK_EQ(mesh.totalVertices, 4u);
  ER_CHECK(!mesh.hasSkeletalAnimation);
  ER_CHECK_EQ(mesh.submeshes[1].startIndex, 3u);
}

ER_TEST(CmoParse, EmptyModel)
{
  ByteWriter w;
  w.u32(0); // zero meshes
  CmoModel model{};
  ER_CHECK(parseCmo(w.data(), model) == CmoStatus::Ok);
  ER_CHECK_EQ(model.meshes.size(), static_cast<size_t>(0));
}

ER_TEST(CmoParse, RejectsTruncated)
{
  auto file = makeStaticCmo();
  file.resize(file.size() - 8); // chop the trailing extents
  CmoModel model{};
  ER_CHECK(parseCmo(file, model) == CmoStatus::Truncated);
}

ER_TEST(CmoParse, RejectsGarbageMeshCount)
{
  // A huge mesh count with no payload must be rejected, not allocated against.
  ByteWriter w;
  w.u32(0xffffffffu);
  CmoModel model{};
  auto status = parseCmo(w.data(), model);
  ER_CHECK(status == CmoStatus::TooLarge || status == CmoStatus::Truncated);
}

ER_TEST(CmoParse, RejectsGarbageIndexCount)
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
  ER_CHECK(status == CmoStatus::TooLarge || status == CmoStatus::Truncated);
}

ER_TEST(CmoParse, EmptyBufferRejected)
{
  std::vector<std::byte> empty;
  CmoModel model{};
  ER_CHECK(parseCmo(empty, model) == CmoStatus::Truncated);
}
