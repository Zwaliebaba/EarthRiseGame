// CMO structural parser tests (mirror NeuronRenderTest, M2 area B gate:
// "submesh/material/index counts on a known mesh; malformed → rejected").

#include "../../NeuronRender/CmoParse.h"
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

ER_TEST(CmoParse, ExtractsBufferData)
{
  auto file = makeStaticCmo();
  CmoModel model{};
  ER_CHECK(parseCmo(file, model) == CmoStatus::Ok);
  const auto& mesh = model.meshes[0];

  // Materials captured; the synthetic material has no diffuse texture.
  ER_CHECK_EQ(mesh.materials.size(), static_cast<size_t>(1));
  ER_CHECK(mesh.materials[0].diffuseTexture.empty());

  // Vertex/index payloads aliased with the right sizes (52 B/vert, 2 B/index).
  ER_CHECK_EQ(mesh.vertexData.size(), static_cast<size_t>(1));
  ER_CHECK_EQ(mesh.vertexCounts[0], 4u);
  ER_CHECK_EQ(mesh.vertexData[0].size(), static_cast<size_t>(4 * 52));
  ER_CHECK_EQ(mesh.indexData.size(), static_cast<size_t>(1));
  ER_CHECK_EQ(mesh.indexCounts[0], 6u);
  ER_CHECK_EQ(mesh.indexData[0].size(), static_cast<size_t>(6 * 2));
}

ER_TEST(CmoParse, ExtractsSkeletonAndAnimation)
{
  auto file = makeAnimatedCmo();
  CmoModel model{};
  ER_CHECK(parseCmo(file, model) == CmoStatus::Ok);
  const auto& mesh = model.meshes[0];

  ER_CHECK(mesh.hasSkeletalAnimation);
  ER_CHECK(mesh.isSkinned());

  // Skeleton: 2 bones, child references the root.
  ER_CHECK_EQ(mesh.bones.size(), static_cast<size_t>(2));
  ER_CHECK_EQ(mesh.bones[0].parentIndex, -1);
  ER_CHECK_EQ(mesh.bones[1].parentIndex, 0);

  // Skinning vertices: 1 buffer of 3, each weighted to bone 1 @ 0.5.
  ER_CHECK_EQ(mesh.skinningVertices.size(), static_cast<size_t>(1));
  ER_CHECK_EQ(mesh.skinningVertices[0].size(), static_cast<size_t>(3));
  ER_CHECK_EQ(mesh.skinningVertices[0][0].boneIndices[1], 1u);
  ER_CHECK(mesh.skinningVertices[0][0].boneWeights[0] > 0.49f &&
           mesh.skinningVertices[0][0].boneWeights[0] < 0.51f);

  // Animation: 1 clip, 2 keyframes; kf1 carries the +10 X translation.
  ER_CHECK_EQ(mesh.animations.size(), static_cast<size_t>(1));
  ER_CHECK(mesh.animations[0].endTime > 0.99f);
  ER_CHECK_EQ(mesh.animations[0].keyframes.size(), static_cast<size_t>(2));
  ER_CHECK_EQ(mesh.animations[0].keyframes[1].boneIndex, 1u);
  ER_CHECK(mesh.animations[0].keyframes[1].transform[12] > 9.9f);
  ER_CHECK(mesh.boundingRadius > 4.9f && mesh.boundingRadius < 5.1f);
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
