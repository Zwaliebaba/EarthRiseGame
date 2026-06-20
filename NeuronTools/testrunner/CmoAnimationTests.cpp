// Skeletal-animation math tests (CmoAnimation.h): matrix ops, keyframe sampling,
// and skinning-palette parent accumulation. Pure math — mirrors what a Windows
// skinned-render path would feed the GPU.

#include "../../NeuronRender/CmoAnimation.h"
#include "TestRunner.h"

#include <cmath>
#include <vector>

using namespace er::format;
using namespace ertest;

namespace
{
  bool nearly(float a, float b) { return std::fabs(a - b) < 1e-4f; }
} // namespace

ER_TEST(CmoAnim, MatrixMultiplyByIdentity)
{
  Mat4 m = mat4Identity();
  m[12] = 3.0f; // translation X
  m[13] = 4.0f; // translation Y
  Mat4 r = mat4Multiply(m, mat4Identity());
  for (int i = 0; i < 16; ++i)
    ER_CHECK(nearly(r[i], m[i]));
}

ER_TEST(CmoAnim, SamplePoseInterpolatesKeyframes)
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
  ER_CHECK_EQ(pose.size(), static_cast<size_t>(1));
  ER_CHECK(nearly(pose[0][12], 5.0f)); // halfway

  auto poseStart = samplePose(bones, clip, 0.0f);
  ER_CHECK(nearly(poseStart[0][12], 0.0f));
  auto poseEnd = samplePose(bones, clip, 1.0f);
  ER_CHECK(nearly(poseEnd[0][12], 10.0f));
}

ER_TEST(CmoAnim, SkinningPaletteAccumulatesParent)
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
  ER_CHECK_EQ(palette.size(), static_cast<size_t>(2));
  ER_CHECK(nearly(palette[0][12], 2.0f));
  ER_CHECK(nearly(palette[1][12], 5.0f)); // child world translation = 2 + 3 (row-vector compose)
}

ER_TEST(CmoAnim, BoneWithoutKeysUsesDefaultLocal)
{
  std::vector<CmoBone> bones(1);
  bones[0].localTransform = mat4Identity();
  bones[0].localTransform[14] = 7.0f; // default Z translation
  CmoAnimationClip clip; // no keyframes for bone 0
  clip.endTime = 1.0f;
  auto pose = samplePose(bones, clip, 0.5f);
  ER_CHECK(nearly(pose[0][14], 7.0f));
}
