#pragma once
// Deterministic 3D-audio math (device-free, header-only).
//
// X3DAudio computes the real output matrix / Doppler / LPF in the device path
// (Spatializer.cpp), but the underlying model — distance attenuation, the
// listener-relative geometry, and the Doppler frequency ratio — is expressed
// here as pure functions so it can be unit-tested deterministically without an
// audio device (masterplan §11.3 test list; neuronaudio-api.md §10). The device
// path feeds these into X3DAudio's emitter curve / Doppler scaler.

#include <algorithm>
#include <cmath>

#include "AudioTypes.h"

namespace Neuron::Audio::spatialmath
{
  // Speed of sound used for Doppler (m/s); matches X3DAudio's default.
  inline constexpr float SPEED_OF_SOUND = 343.5f;

  inline float length(const Vec3& v) noexcept
  {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  }

  inline Vec3 sub(const Vec3& a, const Vec3& b) noexcept
  {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
  }

  inline float dot(const Vec3& a, const Vec3& b) noexcept
  {
    return a.x * b.x + a.y * b.y + a.z * b.z;
  }

  // Distance between listener and emitter (camera-relative space).
  inline float distance(const Listener& l, const Emitter& e) noexcept
  {
    return length(sub(e.position, l.position));
  }

  // Linear-rolloff attenuation in [0,1]: full volume within minDistance, silent
  // beyond maxDistance, linearly interpolated in between. Degenerate ranges
  // (max <= min) collapse to a hard on/off at minDistance.
  inline float attenuation(float dist, float minDistance, float maxDistance) noexcept
  {
    if (dist <= minDistance)
      return 1.0f;
    if (maxDistance <= minDistance || dist >= maxDistance)
      return dist >= maxDistance ? 0.0f : 1.0f;
    const float t = (dist - minDistance) / (maxDistance - minDistance);
    return std::clamp(1.0f - t, 0.0f, 1.0f);
  }

  // Doppler frequency ratio from the closing speed along the listener→emitter
  // axis. Positive closing speed (approaching) raises pitch; receding lowers it.
  // Clamped to a sane band to avoid extreme ratios near the speed of sound.
  inline float dopplerRatio(const Listener& l, const Emitter& e,
                            float speedOfSound = SPEED_OF_SOUND) noexcept
  {
    Vec3 toEmitter = sub(e.position, l.position);
    const float dist = length(toEmitter);
    if (dist < 1e-4f || speedOfSound <= 0.0f)
      return 1.0f;

    // Unit axis listener→emitter.
    const Vec3 axis{toEmitter.x / dist, toEmitter.y / dist, toEmitter.z / dist};

    // Velocity components projected onto the listener→emitter axis. The emitter
    // moving toward the listener (negative dot with the axis) shrinks the
    // denominator and raises pitch; the listener moving toward the emitter
    // (positive dot) raises the numerator and pitch.
    const float vListener = dot(l.velocity, axis);
    const float vEmitter = dot(e.velocity, axis);

    const float denom = speedOfSound + vEmitter;
    if (std::fabs(denom) < 1e-3f)
      return 4.0f; // avoid blow-up; clamp below anyway
    const float ratio = (speedOfSound + vListener) / denom;
    return std::clamp(ratio, 0.25f, 4.0f); // X3DAUDIO-style guard band
  }
} // namespace Neuron::Audio::spatialmath
