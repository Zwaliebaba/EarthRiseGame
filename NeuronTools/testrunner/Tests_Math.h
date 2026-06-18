#pragma once
// M0 unit tests — DirectXMath wrappers (GameMath.h / MathCommon.h).

#include "TestRunner.h"
#include "math/GameMath.h"
#include "math/MathCommon.h"

#include <cmath>

TEST_SUITE(Math)
{
    TEST_CASE(Normalize) {
        XMVECTOR v = Neuron::Math::Set(3.0f, 4.0f, 0.0f);
        XMVECTOR n = Neuron::Math::Normalize(v);
        const float len = Neuron::Math::Length(n);
        CHECK(std::fabsf(len - 1.0f) < 1e-6f);
    });

    TEST_CASE(Dot) {
        XMVECTOR a = Neuron::Math::Set(1.0f, 0.0f, 0.0f);
        XMVECTOR b = Neuron::Math::Set(0.0f, 1.0f, 0.0f);
        const float d = Neuron::Math::Dotf(a, b);
        CHECK(std::fabsf(d) < 1e-6f); // perpendicular → 0
    });

    TEST_CASE(Cross) {
        XMVECTOR x = Neuron::Math::Set(1.0f, 0.0f, 0.0f);
        XMVECTOR y = Neuron::Math::Set(0.0f, 1.0f, 0.0f);
        XMVECTOR z = Neuron::Math::Cross(x, y);
        CHECK(std::fabsf(Neuron::Math::GetX(z))        < 1e-6f);
        CHECK(std::fabsf(Neuron::Math::GetY(z))        < 1e-6f);
        CHECK(std::fabsf(Neuron::Math::GetZ(z) - 1.0f) < 1e-6f);
    });

    TEST_CASE(LengthSquare) {
        XMVECTOR v = Neuron::Math::Set(1.0f, 2.0f, 2.0f);
        CHECK(std::fabsf(Neuron::Math::LengthSquare(v) - 9.0f) < 1e-6f);
        CHECK(std::fabsf(Neuron::Math::Length(v)       - 3.0f) < 1e-6f);
    });

    TEST_CASE(SetLength) {
        XMVECTOR v = Neuron::Math::Set(1.0f, 0.0f, 0.0f);
        XMVECTOR s = Neuron::Math::SetLength(v, 5.0f);
        CHECK(std::fabsf(Neuron::Math::Length(s) - 5.0f) < 1e-5f);
    });

    TEST_CASE(SmoothStep01) {
        CHECK(std::fabsf(Neuron::Math::SmoothStep01(0.0f))        < 1e-6f);
        CHECK(std::fabsf(Neuron::Math::SmoothStep01(1.0f) - 1.0f) < 1e-6f);
        // Mid-point: smooth-step(0.5) = 0.5
        CHECK(std::fabsf(Neuron::Math::SmoothStep01(0.5f) - 0.5f) < 1e-6f);
        // Clamped outside [0,1]
        CHECK(std::fabsf(Neuron::Math::SmoothStep01(-1.0f))        < 1e-6f);
        CHECK(std::fabsf(Neuron::Math::SmoothStep01(2.0f) - 1.0f)  < 1e-6f);
    });

    TEST_CASE(AlignUp) {
        CHECK_EQ(Neuron::Math::AlignUp<size_t>(13, 4), size_t(16));
        CHECK_EQ(Neuron::Math::AlignUp<size_t>(16, 4), size_t(16));
        CHECK_EQ(Neuron::Math::AlignUp<size_t>(1,  8), size_t(8));
    });

    TEST_CASE(IsPowerOfTwo) {
        CHECK(Neuron::Math::IsPowerOfTwo(1u));
        CHECK(Neuron::Math::IsPowerOfTwo(16u));
        CHECK(!Neuron::Math::IsPowerOfTwo(15u));
        CHECK(!Neuron::Math::IsPowerOfTwo(0u));
    });

    TEST_CASE(Hash01_Range) {
        // Hash01 must produce values in [0, 1).
        for (uint32_t i = 0; i < 1000; ++i) {
            const float h = Neuron::Math::Hash01(i);
            CHECK_GE(h, 0.0f);
            CHECK_LT(h, 1.0f);
        }
    });
}
