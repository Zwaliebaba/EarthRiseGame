#pragma once

// Pure CMO (Visual Studio Mesh Content Pipeline) structural parser.
//
// Direct3D ships no model loader and DirectXTK is barred (masterplan §12.3), so
// NeuronRender's CmoLoader (docs/design/neuronrender-architecture.md §9) walks
// the .cmo binary itself. This header is the platform-independent core: it
// validates the structure and reports counts (meshes, materials, submeshes,
// vertices, indices) with strict bounds checks, so a malformed file is
// *rejected*, never crashed on (M2 area B test gate). The Windows loader builds
// GPU buffers from the same walk.
//
// Scope: static meshes first; skinning/skeleton sections are walked for bounds
// validation but their payloads are not surfaced (skinning lands later, R11).
//
// Layout reference (VS / DirectXTK CMO): UTF-16 names are stored as a UINT
// character count followed by that many 16-bit code units (no terminator).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace er::format
{
  enum class CmoStatus : uint8_t
  {
    Ok = 0,
    Truncated, // a declared field/array runs past the end of the buffer
    TooLarge   // a declared count is implausibly large (guards against garbage)
  };

  struct CmoSubmeshInfo
  {
    uint32_t materialIndex = 0;
    uint32_t indexBufferIndex = 0;
    uint32_t vertexBufferIndex = 0;
    uint32_t startIndex = 0;
    uint32_t primCount = 0;
  };

  struct CmoMaterialInfo
  {
    std::string name;
    std::string diffuseTexture; // CMO texture slot 0 = diffuse (a .dds filename)
  };

  // --- Skeletal animation (parsed even though the current assets are static) ---

  // Per-vertex skin binding (4 weighted bones), parallel to a skinning VB.
  struct CmoSkinningVertex
  {
    uint32_t boneIndices[4] = {0, 0, 0, 0};
    float    boneWeights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  };

  // One skeleton joint. Matrices are 16 floats (row-major XMFLOAT4X4 on disk).
  struct CmoBone
  {
    std::string          name;
    int32_t              parentIndex = -1; // -1 = root
    std::array<float, 16> invBindPose{};   // bind-pose inverse (skinning)
    std::array<float, 16> bindPose{};
    std::array<float, 16> localTransform{}; // default local pose
  };

  // One animation key: a bone's local transform at a point in time.
  struct CmoKeyframe
  {
    uint32_t              boneIndex = 0;
    float                 time = 0.0f;
    std::array<float, 16> transform{};
  };

  struct CmoAnimationClip
  {
    std::string              name;
    float                    startTime = 0.0f;
    float                    endTime = 0.0f;
    std::vector<CmoKeyframe>  keyframes;
  };

  struct CmoMeshInfo
  {
    uint32_t nameLength = 0;       // UTF-16 code units
    uint32_t materialCount = 0;
    uint32_t submeshCount = 0;
    uint32_t indexBufferCount = 0;
    uint32_t vertexBufferCount = 0;
    uint32_t skinningBufferCount = 0;
    uint32_t totalIndices = 0;     // summed across all index buffers
    uint32_t totalVertices = 0;    // summed across all vertex buffers
    bool hasSkeletalAnimation = false;

    // MeshExtents (model space): bounding-sphere centre + radius. Lets callers
    // normalize on-screen size / cull regardless of the model's native scale.
    float centerX = 0.0f;
    float centerY = 0.0f;
    float centerZ = 0.0f;
    float boundingRadius = 0.0f;

    std::vector<CmoSubmeshInfo> submeshes;
    std::vector<CmoMaterialInfo> materials;

    // Raw buffer payloads (alias into the parsed file — keep it alive). Each
    // vertex is CMO_VERTEX_SIZE bytes; each index is a uint16_t.
    std::vector<uint32_t> vertexCounts;                   // verts per vertex buffer
    std::vector<uint32_t> indexCounts;                    // indices per index buffer
    std::vector<std::span<const std::byte>> vertexData;   // bytes per vertex buffer
    std::vector<std::span<const std::byte>> indexData;    // bytes per index buffer

    // Skeletal animation (empty for static meshes). skinningVertices is parallel
    // to the skinning vertex buffers (one inner vector per buffer).
    std::vector<std::vector<CmoSkinningVertex>> skinningVertices;
    std::vector<CmoBone>           bones;
    std::vector<CmoAnimationClip>  animations;

    bool isSkinned() const noexcept { return !bones.empty(); }
  };

  struct CmoModel
  {
    std::vector<CmoMeshInfo> meshes;
  };

  // On-disk struct sizes (bytes). UTF-16 wide chars are 2 bytes each.
  inline constexpr size_t CMO_WCHAR_SIZE = 2;
  inline constexpr size_t CMO_MATERIAL_SIZE = 132;  // 33 floats (Ambient/Diffuse/Specular/Power/Emissive/UVTransform)
  inline constexpr size_t CMO_SUBMESH_SIZE = 20;    // 5 UINTs
  inline constexpr size_t CMO_VERTEX_SIZE = 52;     // pos+normal+tangent+color+uv
  inline constexpr size_t CMO_SKIN_VERTEX_SIZE = 32; // 4 bone indices + 4 weights
  inline constexpr size_t CMO_EXTENTS_SIZE = 40;    // center+radius+min+max
  inline constexpr size_t CMO_BONE_SIZE = 196;      // parent index + 3 matrices
  inline constexpr size_t CMO_KEYFRAME_SIZE = 72;   // bone index + time + matrix
  inline constexpr uint32_t CMO_MAX_TEXTURES = 8;

  // Plausibility ceiling: reject counts that would clearly be garbage before we
  // try to size buffers from them (guards integer-overflow / hostile input).
  inline constexpr uint32_t CMO_SANE_COUNT_LIMIT = 100u * 1000u * 1000u;

  namespace detail
  {
    // Forward-only bounds-checked cursor over the CMO byte stream.
    class CmoCursor
    {
    public:
      explicit CmoCursor(std::span<const std::byte> data) noexcept : m_data(data) {}

      bool ok() const noexcept { return m_ok; }
      bool overran() const noexcept { return m_overran; }

      uint32_t readU32() noexcept
      {
        if (!ensure(4))
          return 0;
        const std::byte* p = m_data.data() + m_pos;
        m_pos += 4;
        return std::to_integer<uint32_t>(p[0]) | (std::to_integer<uint32_t>(p[1]) << 8) |
               (std::to_integer<uint32_t>(p[2]) << 16) | (std::to_integer<uint32_t>(p[3]) << 24);
      }

      uint8_t readU8() noexcept
      {
        if (!ensure(1))
          return 0;
        return std::to_integer<uint8_t>(m_data[m_pos++]);
      }

      float readFloat() noexcept
      {
        const uint32_t bits = readU32();
        float f = 0.0f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
      }

      int32_t readI32() noexcept
      {
        const uint32_t bits = readU32();
        int32_t v = 0;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
      }

      void readMatrix(std::array<float, 16>& out) noexcept
      {
        for (float& f : out)
          f = readFloat();
      }

      // Skip `bytes`; flags overrun if it would pass the end.
      void skip(size_t bytes) noexcept
      {
        if (!ensure(bytes))
          return;
        m_pos += bytes;
      }

      // Read `bytes` as a subspan aliasing the source; empty span on overrun.
      std::span<const std::byte> readSpan(size_t bytes) noexcept
      {
        if (!ensure(bytes))
          return {};
        auto s = m_data.subspan(m_pos, bytes);
        m_pos += bytes;
        return s;
      }

      // Read a UTF-16 name (UINT length + that many 16-bit units) as narrow
      // ASCII (CMO texture/material names are ASCII filenames). Empty on overrun.
      std::string readName()
      {
        const uint32_t len = readU32();
        std::string s;
        s.reserve(len);
        for (uint32_t i = 0; i < len; ++i)
        {
          if (!ensure(CMO_WCHAR_SIZE))
            break;
          s.push_back(static_cast<char>(std::to_integer<unsigned char>(m_data[m_pos])));
          m_pos += CMO_WCHAR_SIZE;
        }
        return s;
      }

    private:
      bool ensure(size_t bytes) noexcept
      {
        if (!m_ok)
          return false;
        if (m_pos + bytes > m_data.size())
        {
          m_ok = false;
          m_overran = true;
          return false;
        }
        return true;
      }

      std::span<const std::byte> m_data;
      size_t m_pos = 0;
      bool m_ok = true;
      bool m_overran = false;
    };
  } // namespace detail

  // Parse a CMO blob's structure. On Ok, `out.meshes` is populated with counts.
  inline CmoStatus parseCmo(std::span<const std::byte> file, CmoModel& out)
  {
    using detail::CmoCursor;
    CmoCursor cur(file);
    out.meshes.clear();

    const uint32_t meshCount = cur.readU32();
    if (!cur.ok())
      return CmoStatus::Truncated;
    if (meshCount > CMO_SANE_COUNT_LIMIT)
      return CmoStatus::TooLarge;

    for (uint32_t m = 0; m < meshCount && cur.ok(); ++m)
    {
      CmoMeshInfo mesh{};
      mesh.nameLength = cur.readU32();
      cur.skip(static_cast<size_t>(mesh.nameLength) * CMO_WCHAR_SIZE);

      // Materials: name + fixed struct + pixel-shader name + 8 texture names.
      mesh.materialCount = cur.readU32();
      if (mesh.materialCount > CMO_SANE_COUNT_LIMIT)
        return CmoStatus::TooLarge;
      for (uint32_t i = 0; i < mesh.materialCount && cur.ok(); ++i)
      {
        CmoMaterialInfo mat{};
        mat.name = cur.readName();        // material name
        cur.skip(CMO_MATERIAL_SIZE);      // Material struct
        cur.readName();                   // pixel-shader name (ignored)
        for (uint32_t t = 0; t < CMO_MAX_TEXTURES; ++t)
        {
          std::string tex = cur.readName(); // texture names
          if (t == 0)
            mat.diffuseTexture = std::move(tex); // slot 0 = diffuse
        }
        if (cur.ok())
          mesh.materials.push_back(std::move(mat));
      }

      mesh.hasSkeletalAnimation = cur.readU8() != 0;

      // Submeshes.
      mesh.submeshCount = cur.readU32();
      if (mesh.submeshCount > CMO_SANE_COUNT_LIMIT)
        return CmoStatus::TooLarge;
      mesh.submeshes.reserve(mesh.submeshCount);
      for (uint32_t i = 0; i < mesh.submeshCount && cur.ok(); ++i)
      {
        CmoSubmeshInfo sm{};
        sm.materialIndex = cur.readU32();
        sm.indexBufferIndex = cur.readU32();
        sm.vertexBufferIndex = cur.readU32();
        sm.startIndex = cur.readU32();
        sm.primCount = cur.readU32();
        if (cur.ok())
          mesh.submeshes.push_back(sm);
      }

      // Index buffers: each is a UINT count of 16-bit indices.
      mesh.indexBufferCount = cur.readU32();
      if (mesh.indexBufferCount > CMO_SANE_COUNT_LIMIT)
        return CmoStatus::TooLarge;
      for (uint32_t i = 0; i < mesh.indexBufferCount && cur.ok(); ++i)
      {
        const uint32_t n = cur.readU32();
        if (n > CMO_SANE_COUNT_LIMIT)
          return CmoStatus::TooLarge;
        mesh.totalIndices += n;
        auto span = cur.readSpan(static_cast<size_t>(n) * sizeof(uint16_t));
        if (cur.ok())
        {
          mesh.indexCounts.push_back(n);
          mesh.indexData.push_back(span);
        }
      }

      // Vertex buffers.
      mesh.vertexBufferCount = cur.readU32();
      if (mesh.vertexBufferCount > CMO_SANE_COUNT_LIMIT)
        return CmoStatus::TooLarge;
      for (uint32_t i = 0; i < mesh.vertexBufferCount && cur.ok(); ++i)
      {
        const uint32_t n = cur.readU32();
        if (n > CMO_SANE_COUNT_LIMIT)
          return CmoStatus::TooLarge;
        mesh.totalVertices += n;
        auto span = cur.readSpan(static_cast<size_t>(n) * CMO_VERTEX_SIZE);
        if (cur.ok())
        {
          mesh.vertexCounts.push_back(n);
          mesh.vertexData.push_back(span);
        }
      }

      // Skinning vertex buffers (4 weighted bone influences per vertex).
      mesh.skinningBufferCount = cur.readU32();
      if (mesh.skinningBufferCount > CMO_SANE_COUNT_LIMIT)
        return CmoStatus::TooLarge;
      for (uint32_t i = 0; i < mesh.skinningBufferCount && cur.ok(); ++i)
      {
        const uint32_t n = cur.readU32();
        if (n > CMO_SANE_COUNT_LIMIT)
          return CmoStatus::TooLarge;
        std::vector<CmoSkinningVertex> verts;
        verts.reserve(n);
        for (uint32_t k = 0; k < n && cur.ok(); ++k)
        {
          CmoSkinningVertex sv;
          for (uint32_t b = 0; b < 4; ++b)
            sv.boneIndices[b] = cur.readU32();
          for (uint32_t b = 0; b < 4; ++b)
            sv.boneWeights[b] = cur.readFloat();
          if (cur.ok())
            verts.push_back(sv);
        }
        if (cur.ok())
          mesh.skinningVertices.push_back(std::move(verts));
      }

      // MeshExtents: centre(3 float) + radius(1) + min(3) + max(3) = 40 bytes.
      mesh.centerX = cur.readFloat();
      mesh.centerY = cur.readFloat();
      mesh.centerZ = cur.readFloat();
      mesh.boundingRadius = cur.readFloat();
      cur.skip(sizeof(float) * 6); // Min + Max (unused for now)

      // Optional skeleton + animation clips.
      if (mesh.hasSkeletalAnimation)
      {
        const uint32_t boneCount = cur.readU32();
        if (boneCount > CMO_SANE_COUNT_LIMIT)
          return CmoStatus::TooLarge;
        mesh.bones.reserve(boneCount);
        for (uint32_t i = 0; i < boneCount && cur.ok(); ++i)
        {
          CmoBone bone;
          bone.name = cur.readName();
          bone.parentIndex = cur.readI32();
          cur.readMatrix(bone.invBindPose);
          cur.readMatrix(bone.bindPose);
          cur.readMatrix(bone.localTransform);
          if (cur.ok())
            mesh.bones.push_back(std::move(bone));
        }

        const uint32_t clipCount = cur.readU32();
        if (clipCount > CMO_SANE_COUNT_LIMIT)
          return CmoStatus::TooLarge;
        mesh.animations.reserve(clipCount);
        for (uint32_t i = 0; i < clipCount && cur.ok(); ++i)
        {
          CmoAnimationClip clip;
          clip.name = cur.readName();
          clip.startTime = cur.readFloat();
          clip.endTime = cur.readFloat();
          const uint32_t keyframes = cur.readU32();
          if (keyframes > CMO_SANE_COUNT_LIMIT)
            return CmoStatus::TooLarge;
          clip.keyframes.reserve(keyframes);
          for (uint32_t k = 0; k < keyframes && cur.ok(); ++k)
          {
            CmoKeyframe key;
            key.boneIndex = cur.readU32();
            key.time = cur.readFloat();
            cur.readMatrix(key.transform);
            if (cur.ok())
              clip.keyframes.push_back(key);
          }
          if (cur.ok())
            mesh.animations.push_back(std::move(clip));
        }
      }

      if (!cur.ok())
        return CmoStatus::Truncated;
      out.meshes.push_back(std::move(mesh));
    }

    return cur.overran() ? CmoStatus::Truncated : CmoStatus::Ok;
  }
} // namespace er::format
