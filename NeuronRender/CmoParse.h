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

#include <cstddef>
#include <cstdint>
#include <span>
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
    std::vector<CmoSubmeshInfo> submeshes;
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

      // Skip `bytes`; flags overrun if it would pass the end.
      void skip(size_t bytes) noexcept
      {
        if (!ensure(bytes))
          return;
        m_pos += bytes;
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

    auto skipName = [&cur]() { cur.skip(static_cast<size_t>(cur.readU32()) * CMO_WCHAR_SIZE); };

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
        skipName();                       // material name
        cur.skip(CMO_MATERIAL_SIZE);      // Material struct
        skipName();                       // pixel-shader name
        for (uint32_t t = 0; t < CMO_MAX_TEXTURES; ++t)
          skipName();                     // texture names
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
        cur.skip(static_cast<size_t>(n) * sizeof(uint16_t));
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
        cur.skip(static_cast<size_t>(n) * CMO_VERTEX_SIZE);
      }

      // Skinning vertex buffers (walked for bounds validation only).
      mesh.skinningBufferCount = cur.readU32();
      if (mesh.skinningBufferCount > CMO_SANE_COUNT_LIMIT)
        return CmoStatus::TooLarge;
      for (uint32_t i = 0; i < mesh.skinningBufferCount && cur.ok(); ++i)
      {
        const uint32_t n = cur.readU32();
        if (n > CMO_SANE_COUNT_LIMIT)
          return CmoStatus::TooLarge;
        cur.skip(static_cast<size_t>(n) * CMO_SKIN_VERTEX_SIZE);
      }

      cur.skip(CMO_EXTENTS_SIZE); // MeshExtents

      // Optional skeleton + animation clips.
      if (mesh.hasSkeletalAnimation)
      {
        const uint32_t boneCount = cur.readU32();
        if (boneCount > CMO_SANE_COUNT_LIMIT)
          return CmoStatus::TooLarge;
        for (uint32_t i = 0; i < boneCount && cur.ok(); ++i)
        {
          skipName();
          cur.skip(CMO_BONE_SIZE);
        }

        const uint32_t clipCount = cur.readU32();
        if (clipCount > CMO_SANE_COUNT_LIMIT)
          return CmoStatus::TooLarge;
        for (uint32_t i = 0; i < clipCount && cur.ok(); ++i)
        {
          skipName();
          cur.skip(sizeof(float) * 2); // StartTime, EndTime
          const uint32_t keyframes = cur.readU32();
          if (keyframes > CMO_SANE_COUNT_LIMIT)
            return CmoStatus::TooLarge;
          cur.skip(static_cast<size_t>(keyframes) * CMO_KEYFRAME_SIZE);
        }
      }

      if (!cur.ok())
        return CmoStatus::Truncated;
      out.meshes.push_back(std::move(mesh));
    }

    return cur.overran() ? CmoStatus::Truncated : CmoStatus::Ok;
  }
} // namespace er::format
