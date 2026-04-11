/**
 * @file TestAnimationCompressionReal.cpp
 * @brief Real-class tests for Spark::Animation::AnimationCompressor
 */

#include "TestFramework.h"
#include "Engine/Animation/AnimationCompression.h"

#include <cmath>

namespace
{
    bool QuatNear(const XMFLOAT4& a, const XMFLOAT4& b, float eps = 0.01f)
    {
        // Quaternions may be negated and still represent the same rotation.
        bool direct = std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps && std::abs(a.z - b.z) < eps &&
                      std::abs(a.w - b.w) < eps;
        bool negated = std::abs(a.x + b.x) < eps && std::abs(a.y + b.y) < eps && std::abs(a.z + b.z) < eps &&
                       std::abs(a.w + b.w) < eps;
        return direct || negated;
    }

    bool Vec3Near(const XMFLOAT3& a, const XMFLOAT3& b, float eps = 0.01f)
    {
        return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps && std::abs(a.z - b.z) < eps;
    }
} // namespace

TEST(AnimationCompressionReal_QuantizeDequantizeIdentity)
{
    // Identity quaternion.
    XMFLOAT4 q{0.0f, 0.0f, 0.0f, 1.0f};
    auto packed = Spark::Animation::AnimationCompressor::QuantizeQuaternion(q);
    auto unpacked = Spark::Animation::AnimationCompressor::DequantizeQuaternion(packed);
    EXPECT_TRUE(QuatNear(q, unpacked));
}

TEST(AnimationCompressionReal_QuantizeDequantizeGeneric)
{
    // Arbitrary normalized quaternion.
    XMFLOAT4 q{0.5f, 0.5f, 0.5f, 0.5f};
    auto packed = Spark::Animation::AnimationCompressor::QuantizeQuaternion(q);
    auto unpacked = Spark::Animation::AnimationCompressor::DequantizeQuaternion(packed);
    EXPECT_TRUE(QuatNear(q, unpacked, 0.02f));
}

TEST(AnimationCompressionReal_QuantizeVectorIdentity)
{
    XMFLOAT3 v{0.5f, 0.5f, 0.5f};
    XMFLOAT3 rangeMin{0.0f, 0.0f, 0.0f};
    XMFLOAT3 rangeMax{1.0f, 1.0f, 1.0f};
    auto packed = Spark::Animation::AnimationCompressor::QuantizeVector(v, rangeMin, rangeMax);
    auto unpacked = Spark::Animation::AnimationCompressor::DequantizeVector(packed, rangeMin, rangeMax);
    EXPECT_TRUE(Vec3Near(v, unpacked, 0.001f));
}

TEST(AnimationCompressionReal_QuantizeVectorExtents)
{
    XMFLOAT3 rangeMin{-10.0f, -20.0f, -30.0f};
    XMFLOAT3 rangeMax{10.0f, 20.0f, 30.0f};

    XMFLOAT3 origin{0.0f, 0.0f, 0.0f};
    auto packed = Spark::Animation::AnimationCompressor::QuantizeVector(origin, rangeMin, rangeMax);
    auto unpacked = Spark::Animation::AnimationCompressor::DequantizeVector(packed, rangeMin, rangeMax);
    EXPECT_TRUE(Vec3Near(origin, unpacked, 0.01f));

    XMFLOAT3 extreme{10.0f, 20.0f, 30.0f};
    auto packed2 = Spark::Animation::AnimationCompressor::QuantizeVector(extreme, rangeMin, rangeMax);
    auto unpacked2 = Spark::Animation::AnimationCompressor::DequantizeVector(packed2, rangeMin, rangeMax);
    EXPECT_TRUE(Vec3Near(extreme, unpacked2, 0.01f));
}

TEST(AnimationCompressionReal_CompressDecompressRoundTripAxisAligned)
{
    // An axis-aligned rotation: 90° around Y axis.
    const float s = std::sin(1.57079632679f / 2.0f);
    const float c = std::cos(1.57079632679f / 2.0f);
    XMFLOAT4 q{0.0f, s, 0.0f, c};
    auto packed = Spark::Animation::AnimationCompressor::QuantizeQuaternion(q);
    auto unpacked = Spark::Animation::AnimationCompressor::DequantizeQuaternion(packed);
    EXPECT_TRUE(QuatNear(q, unpacked, 0.02f));
}
