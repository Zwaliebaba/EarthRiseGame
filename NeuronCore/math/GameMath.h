#pragma once

#include <DirectXCollision.h>
#include <DirectXMath.h>
#include "MathCommon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace DirectX;

namespace Neuron::Math
{
  [[nodiscard]] inline XMVECTOR XM_CALLCONV Set(float _x, float _y, float _z) { return XMVectorSet(_x, _y, _z, 0.0f); }
  [[nodiscard]] inline XMVECTOR XM_CALLCONV Normalize(const XMVECTOR _v) { return XMVector3Normalize(_v); }
  [[nodiscard]] inline XMVECTOR XM_CALLCONV Cross(const XMVECTOR _v1, const XMVECTOR _v2) { return XMVector3Cross(_v1, _v2); }
  [[nodiscard]] inline XMVECTOR XM_CALLCONV Dot(const XMVECTOR _v1, const XMVECTOR _v2) { return XMVector3Dot(_v1, _v2); }
  [[nodiscard]] inline float XM_CALLCONV Dotf(const XMVECTOR _v1, const XMVECTOR _v2) { return XMVectorGetX(XMVector3Dot(_v1, _v2)); }

  [[nodiscard]] inline float XM_CALLCONV Length(const XMVECTOR _v) { return XMVectorGetX(XMVector3Length(_v)); }
  [[nodiscard]] inline float XM_CALLCONV LengthSquare(const XMVECTOR _v) { return XMVectorGetX(XMVector3LengthSq(_v)); }

  [[nodiscard]] inline XMVECTOR XM_CALLCONV SetLength(const XMVECTOR _v1, float _length)
  {
    return XMVectorScale(XMVector3Normalize(_v1), _length);
  }

  [[nodiscard]] inline float XM_CALLCONV GetX(const XMVECTOR _v) { return XMVectorGetX(_v); }
  [[nodiscard]] inline float XM_CALLCONV GetY(const XMVECTOR _v) { return XMVectorGetY(_v); }
  [[nodiscard]] inline float XM_CALLCONV GetZ(const XMVECTOR _v) { return XMVectorGetZ(_v); }

  [[nodiscard]] inline XMVECTOR XM_CALLCONV RotateAround(const XMVECTOR _v1, const XMVECTOR _v2)
  {
    float angle = Length(_v2);
    if (angle < 1e-8)
      return _v1;

    return XMVector3Transform(_v1, XMMatrixRotationAxis(_v2, angle));
  }

  [[nodiscard]] inline XMVECTOR XM_CALLCONV RotateAroundX(const XMVECTOR _v, float _angle)
  {
    return XMVector3Transform(_v, XMMatrixRotationX(_angle));
  }

  [[nodiscard]] inline XMVECTOR XM_CALLCONV RotateAroundY(const XMVECTOR _v, float _angle)
  {
    return XMVector3Transform(_v, XMMatrixRotationY(_angle));
  }

  [[nodiscard]] inline XMVECTOR XM_CALLCONV RotateAroundZ(const XMVECTOR _v, float _angle)
  {
    return XMVector3Transform(_v, XMMatrixRotationZ(_angle));
  }

  [[nodiscard]] inline XMMATRIX XM_CALLCONV CreateRotationMatrix(XMVECTOR _axis, float _angle)
  {
    return XMMatrixRotationAxis(_axis, _angle);
  }

  inline bool XM_CALLCONV operator==(const XMVECTOR _v1, const XMVECTORF32 _v2)
  {
    return XMVector4EqualInt(XMVectorEqual(_v1, _v2), XMVectorTrueInt());
  }

  [[nodiscard]] inline XMMATRIX XM_CALLCONV Invert(const XMMATRIX _m) noexcept { return XMMatrixInverse(nullptr, _m); }

  [[nodiscard]] inline XMVECTOR XM_CALLCONV operator-(const XMVECTOR _v1, const XMVECTORF32 _v2) { return XMVectorSubtract(_v1, _v2); }
  [[nodiscard]] inline XMVECTOR XM_CALLCONV operator-(const XMVECTORF32 _v1, const XMVECTOR _v2) { return XMVectorSubtract(_v1, _v2); }

  namespace Vector3
  {
    static constexpr XMVECTORF32 ZERO = {};
    static constexpr XMVECTORF32 ONE = {{{1.0f, 1.0f, 1.0f, 0.0f}}};

    static constexpr XMVECTORF32 UNITX = {{{1.0f, 0.0f, 0.0f, 0.0f}}};
    static constexpr XMVECTORF32 UNITY = {{{0.0f, 1.0f, 0.0f, 0.0f}}};
    static constexpr XMVECTORF32 UNITZ = {{{0.0f, 0.0f, 1.0f, 0.0f}}};

    static constexpr XMVECTORF32 UP = {{{0.0f, 1.0f, 0.0f, 0.0f}}};
    static constexpr XMVECTORF32 DOWN = {{{0.0f, -1.0f, 0.0f, 0.0f}}};
    static constexpr XMVECTORF32 RIGHT = {{{1.0f, 0.0f, 0.0f, 0.0f}}};
    static constexpr XMVECTORF32 LEFT = {{{-1.0f, 0.0f, 0.0f, 0.0f}}};
    static constexpr XMVECTORF32 FORWARD = {{{0.0f, 0.0f, 1.0f, 0.0f}}};
    static constexpr XMVECTORF32 BACKWARD = {{{0.0f, 0.0f, -1.0f, 0.0f}}};

    static constexpr XMVECTORF32 TOLERANCE = {{{0.2f, 0.2f, 0.2f, 0.0f}}};
  }

  // Scalar utilities with no DirectXMath equivalent. Used by the particle and
  // debris systems and by the camera rig blend curve.
  [[nodiscard]] inline float SmoothStep01(float t) noexcept
  {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  }

  [[nodiscard]] inline float Hash01(std::uint32_t x) noexcept
  {
    x = (x ^ 61u) ^ (x >> 16);
    x = x + (x << 3);
    x = x ^ (x >> 4);
    x = x * 0x27d4eb2du;
    x = x ^ (x >> 15);
    return static_cast<float>(x & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
  }
}

using namespace Neuron::Math;
