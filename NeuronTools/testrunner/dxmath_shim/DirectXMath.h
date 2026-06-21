#pragma once
// Minimal DirectXMath shim — JUST enough of DirectX::XMFLOAT3/XMFLOAT4X4 for the
// platform-independent sim headers (Components.h / UniversePos.h / Snapshot.h /
// ServerUniverse.h) to compile under the Linux test runner. The real DirectXMath
// is used on Windows; these tests exercise only the integer/struct sim logic
// (catalog, snapshot wire, ECS spawn), none of the SIMD math, so the field
// layout is all that matters here.
namespace DirectX
{
  struct XMFLOAT3
  {
    float x{ 0 }, y{ 0 }, z{ 0 };
    XMFLOAT3() = default;
    XMFLOAT3(float a, float b, float c) : x(a), y(b), z(c) {}
  };
  struct XMFLOAT4X4 { float m[4][4]{}; };
}
