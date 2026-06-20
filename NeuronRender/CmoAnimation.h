#pragma once

// Pure skeletal-animation math for CMO skinned meshes (no D3D/Windows deps, so
// it is unit-testable on any platform). Bridges parsed CMO skeleton/clips
// (CmoParse.h) to the per-bone skinning palette a GPU skinning shader consumes.
//
// Matrices are row-major 16-float arrays (matching CMO's XMFLOAT4X4 on disk) and
// composed in the DirectXMath ROW-VECTOR convention (v' = v · M), so a child's
// world matrix is `localTransform · parentWorld`. The skinning matrix for bone i
// is `invBindPose[i] · world[i]` (transforms a bind-pose vertex into the
// animated pose). Keyframes are interpolated component-wise (simple lerp; a
// TRS-decompose + slerp upgrade can come later without changing this interface).

#include <array>
#include <cstdint>
#include <vector>

#include "CmoParse.h"

namespace er::format
{
  using Mat4 = std::array<float, 16>;

  inline Mat4 mat4Identity() noexcept
  {
    Mat4 m{};
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    return m;
  }

  // Row-major product r = a · b (r[row][col] = Σ a[row][k]·b[k][col]).
  inline Mat4 mat4Multiply(const Mat4& a, const Mat4& b) noexcept
  {
    Mat4 r{};
    for (int row = 0; row < 4; ++row)
      for (int col = 0; col < 4; ++col)
      {
        float s = 0.0f;
        for (int k = 0; k < 4; ++k)
          s += a[row * 4 + k] * b[k * 4 + col];
        r[row * 4 + col] = s;
      }
    return r;
  }

  inline Mat4 mat4Lerp(const Mat4& a, const Mat4& b, float t) noexcept
  {
    Mat4 r{};
    for (int i = 0; i < 16; ++i)
      r[i] = a[i] + (b[i] - a[i]) * t;
    return r;
  }

  // Animated local transform for one bone at `time` within `clip`: interpolate
  // between the bracketing keyframes for that bone; fall back to the bone's
  // default local pose when it has no keys.
  inline Mat4 sampleBoneLocal(const std::vector<CmoBone>& bones, uint32_t boneIndex,
                              const CmoAnimationClip& clip, float time) noexcept
  {
    const CmoKeyframe* prev = nullptr;
    const CmoKeyframe* next = nullptr;
    for (const CmoKeyframe& k : clip.keyframes)
    {
      if (k.boneIndex != boneIndex)
        continue;
      if (k.time <= time && (!prev || k.time > prev->time))
        prev = &k;
      if (k.time >= time && (!next || k.time < next->time))
        next = &k;
    }

    if (prev && next)
    {
      if (next->time <= prev->time)
        return prev->transform;
      const float t = (time - prev->time) / (next->time - prev->time);
      return mat4Lerp(prev->transform, next->transform, t);
    }
    if (prev)
      return prev->transform;
    if (next)
      return next->transform;
    return (boneIndex < bones.size()) ? bones[boneIndex].localTransform : mat4Identity();
  }

  // Per-bone animated local transforms for a clip at a time.
  inline std::vector<Mat4> samplePose(const std::vector<CmoBone>& bones,
                                      const CmoAnimationClip& clip, float time)
  {
    std::vector<Mat4> out(bones.size());
    for (size_t i = 0; i < bones.size(); ++i)
      out[i] = sampleBoneLocal(bones, static_cast<uint32_t>(i), clip, time);
    return out;
  }

  // Skinning palette: invBindPose[i] · world[i], where world[i] = local[i] ·
  // world[parent[i]]. Assumes parent-before-child ordering (CMO convention:
  // a bone's parent index is less than its own index); a parent index outside
  // [0, i) is treated as a root.
  inline std::vector<Mat4> computeSkinningPalette(const std::vector<CmoBone>& bones,
                                                  const std::vector<Mat4>& localPoses)
  {
    const size_t n = bones.size();
    std::vector<Mat4> world(n);
    std::vector<Mat4> palette(n);
    for (size_t i = 0; i < n; ++i)
    {
      const Mat4& local = (i < localPoses.size()) ? localPoses[i] : bones[i].localTransform;
      const int parent = bones[i].parentIndex;
      world[i] = (parent >= 0 && static_cast<size_t>(parent) < i)
                     ? mat4Multiply(local, world[static_cast<size_t>(parent)])
                     : local;
      palette[i] = mat4Multiply(bones[i].invBindPose, world[i]);
    }
    return palette;
  }

  // Convenience: sample a clip at a time and return the ready-to-upload palette.
  inline std::vector<Mat4> evaluateClip(const std::vector<CmoBone>& bones,
                                        const CmoAnimationClip& clip, float time)
  {
    return computeSkinningPalette(bones, samplePose(bones, clip, time));
  }
} // namespace er::format
