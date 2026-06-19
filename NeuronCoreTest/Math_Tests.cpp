#include "CppUnitTest.h"
#include "math/GameMath.h"
#include "math/MathCommon.h"

#include <cmath>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

TEST_CLASS(MathTests)
{
public:
    TEST_METHOD(Normalize)
    {
        XMVECTOR v = Neuron::Math::Set(3.0f, 4.0f, 0.0f);
        XMVECTOR n = Neuron::Math::Normalize(v);
        const float len = Neuron::Math::Length(n);
        Assert::IsTrue(std::fabs(len - 1.0f) < 1e-6f);
    }

    TEST_METHOD(Dot)
    {
        XMVECTOR a = Neuron::Math::Set(1.0f, 0.0f, 0.0f);
        XMVECTOR b = Neuron::Math::Set(0.0f, 1.0f, 0.0f);
        const float d = Neuron::Math::Dotf(a, b);
        Assert::IsTrue(std::fabs(d) < 1e-6f);
    }

    TEST_METHOD(Cross)
    {
        XMVECTOR x = Neuron::Math::Set(1.0f, 0.0f, 0.0f);
        XMVECTOR y = Neuron::Math::Set(0.0f, 1.0f, 0.0f);
        XMVECTOR z = Neuron::Math::Cross(x, y);
        Assert::IsTrue(std::fabs(Neuron::Math::GetX(z))        < 1e-6f);
        Assert::IsTrue(std::fabs(Neuron::Math::GetY(z))        < 1e-6f);
        Assert::IsTrue(std::fabs(Neuron::Math::GetZ(z) - 1.0f) < 1e-6f);
    }

    TEST_METHOD(LengthSquare)
    {
        XMVECTOR v = Neuron::Math::Set(1.0f, 2.0f, 2.0f);
        Assert::IsTrue(std::fabs(Neuron::Math::LengthSquare(v) - 9.0f) < 1e-6f);
        Assert::IsTrue(std::fabs(Neuron::Math::Length(v)       - 3.0f) < 1e-6f);
    }

    TEST_METHOD(SetLength)
    {
        XMVECTOR v = Neuron::Math::Set(1.0f, 0.0f, 0.0f);
        XMVECTOR s = Neuron::Math::SetLength(v, 5.0f);
        Assert::IsTrue(std::fabs(Neuron::Math::Length(s) - 5.0f) < 1e-5f);
    }

    TEST_METHOD(SmoothStep01)
    {
        Assert::IsTrue(std::fabs(Neuron::Math::SmoothStep01(0.0f))        < 1e-6f);
        Assert::IsTrue(std::fabs(Neuron::Math::SmoothStep01(1.0f) - 1.0f) < 1e-6f);
        Assert::IsTrue(std::fabs(Neuron::Math::SmoothStep01(0.5f) - 0.5f) < 1e-6f);
        Assert::IsTrue(std::fabs(Neuron::Math::SmoothStep01(-1.0f))        < 1e-6f);
        Assert::IsTrue(std::fabs(Neuron::Math::SmoothStep01(2.0f) - 1.0f)  < 1e-6f);
    }

    TEST_METHOD(AlignUp)
    {
        Assert::AreEqual(size_t(16), Neuron::Math::AlignUp<size_t>(13, 4));
        Assert::AreEqual(size_t(16), Neuron::Math::AlignUp<size_t>(16, 4));
        Assert::AreEqual(size_t(8),  Neuron::Math::AlignUp<size_t>(1,  8));
    }

    TEST_METHOD(IsPowerOfTwo)
    {
        Assert::IsTrue (Neuron::Math::IsPowerOfTwo(1u));
        Assert::IsTrue (Neuron::Math::IsPowerOfTwo(16u));
        Assert::IsFalse(Neuron::Math::IsPowerOfTwo(15u));
        Assert::IsFalse(Neuron::Math::IsPowerOfTwo(0u));
    }

    TEST_METHOD(Hash01_Range)
    {
        for (uint32_t i = 0; i < 1000; ++i) {
            const float h = Neuron::Math::Hash01(i);
            Assert::IsTrue(h >= 0.0f);
            Assert::IsTrue(h <  1.0f);
        }
    }
};
