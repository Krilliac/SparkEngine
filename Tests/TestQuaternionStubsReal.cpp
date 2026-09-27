/**
 * @file TestQuaternionStubsReal.cpp
 * @brief DirectXMath quaternion and decomposition semantics, checked against closed-form values.
 *
 * On Linux/macOS, Core/PlatformDirectXMathStubs.h stands in for DirectXMath. Its XMMatrixDecompose
 * used to return an identity rotation (every blend, additive layer, root-motion delta and IK blend
 * that decomposes a bone matrix lost its rotation), XMQuaternionMultiply composed q1*q2 where
 * DirectXMath returns q2*q1, and XMMatrixRotationAxis did not normalize its axis.
 *
 * Every expectation here is a closed-form value, not the output of another DirectXMath call, and
 * the file is compiled on every platform: on Windows it runs against real DirectXMath, which pins
 * the expected values to DirectXMath's behaviour; on Linux/macOS the same assertions run against
 * the stub. XMVECTOR and XMMATRIX are opaque SIMD types on Windows, so values are read through
 * XMStoreFloat4 / XMStoreFloat4x4 only.
 */

#include "TestFramework.h"

#include "Engine/Animation/AnimationSystem.h"

#include <cmath>
#include <vector>

namespace
{
    constexpr float kTolerance = 1e-4f;
    constexpr float kHalfPi = 1.57079632679f;
    constexpr float kHalfSqrt2 = 0.70710678f;

    // slerp(identity, 90 degrees about Z, 0.5) is a 45 degree Z turn: (0, 0, sin 22.5, cos 22.5).
    constexpr float kSin22_5 = 0.38268343f;
    constexpr float kCos22_5 = 0.92387953f;

    XMFLOAT4 ToFloat4(const XMVECTOR& v)
    {
        XMFLOAT4 out;
        XMStoreFloat4(&out, v);
        return out;
    }

    XMFLOAT4X4 ToFloat4x4(const XMMATRIX& m)
    {
        XMFLOAT4X4 out;
        XMStoreFloat4x4(&out, m);
        return out;
    }

    float Length4(const XMFLOAT4& q)
    {
        return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    }

    float Dot4(const XMFLOAT4& a, const XMFLOAT4& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }

    void ExpectQuaternionNear(const XMVECTOR& actual, float x, float y, float z, float w)
    {
        const XMFLOAT4 q = ToFloat4(actual);
        EXPECT_NEAR(q.x, x, kTolerance);
        EXPECT_NEAR(q.y, y, kTolerance);
        EXPECT_NEAR(q.z, z, kTolerance);
        EXPECT_NEAR(q.w, w, kTolerance);
    }

    void ExpectMatricesNear(const XMMATRIX& actual, const XMMATRIX& expected)
    {
        const XMFLOAT4X4 a = ToFloat4x4(actual);
        const XMFLOAT4X4 b = ToFloat4x4(expected);
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 4; ++col)
                EXPECT_NEAR(a.m[row][col], b.m[row][col], kTolerance);
        }
    }

    /// Z rotation by `angle` written out in DirectXMath's row-vector layout (v' = v * M).
    XMMATRIX ClosedFormRotationZ(float angle)
    {
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        return XMMatrixSet(c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    }

    XMMATRIX ComposeSRT(float sx, float sy, float sz, const XMVECTOR& rotation, float tx, float ty, float tz)
    {
        return XMMatrixMultiply(XMMatrixMultiply(XMMatrixScaling(sx, sy, sz), XMMatrixRotationQuaternion(rotation)),
                                XMMatrixTranslation(tx, ty, tz));
    }
} // namespace

// ---------------------------------------------------------------------------
// XMQuaternionSlerp
// ---------------------------------------------------------------------------

TEST(DXMathStub_SlerpMidpointIsUnitSphericalInterpolation)
{
    const XMVECTOR identity = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    const XMVECTOR quarterTurnZ = XMVectorSet(0.0f, 0.0f, kHalfSqrt2, kHalfSqrt2);

    // A plain lerp gives (0, 0, 0.3536, 0.8536), length 0.924.
    const XMVECTOR mid = XMQuaternionSlerp(identity, quarterTurnZ, 0.5f);
    ExpectQuaternionNear(mid, 0.0f, 0.0f, kSin22_5, kCos22_5);
    EXPECT_NEAR(Length4(ToFloat4(mid)), 1.0f, kTolerance);
}

TEST(DXMathStub_SlerpIsConstantAngularVelocity)
{
    // A quarter of the way through a 90 degree turn is exactly 22.5 degrees; a normalized lerp
    // would be ahead of that.
    const XMVECTOR identity = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    const XMVECTOR quarterTurnZ = XMVectorSet(0.0f, 0.0f, kHalfSqrt2, kHalfSqrt2);

    const float halfAngle = 0.25f * kHalfPi * 0.5f;
    ExpectQuaternionNear(XMQuaternionSlerp(identity, quarterTurnZ, 0.25f), 0.0f, 0.0f, std::sin(halfAngle),
                         std::cos(halfAngle));
}

TEST(DXMathStub_SlerpTakesShortestArcAcrossHemispheres)
{
    // -q is the same rotation as q, so slerp toward it must still take the 45 degree midpoint
    // rather than swinging the long way round (a -135 degree turn).
    const XMVECTOR identity = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    const XMVECTOR negatedQuarterTurnZ = XMVectorSet(0.0f, 0.0f, -kHalfSqrt2, -kHalfSqrt2);

    const XMVECTOR mid = XMQuaternionSlerp(identity, negatedQuarterTurnZ, 0.5f);
    ExpectQuaternionNear(mid, 0.0f, 0.0f, kSin22_5, kCos22_5);
    EXPECT_NEAR(Length4(ToFloat4(mid)), 1.0f, kTolerance);
}

TEST(DXMathStub_SlerpEndpointsAndNearlyParallelInputsStayUnit)
{
    const XMVECTOR a = XMVectorSet(0.0f, std::sin(0.35f), 0.0f, std::cos(0.35f)); // 0.7 rad about Y
    const XMVECTOR b = XMVectorSet(std::sin(-0.65f), 0.0f, 0.0f, std::cos(-0.65f)); // -1.3 rad about X

    ExpectQuaternionNear(XMQuaternionSlerp(a, b, 0.0f), 0.0f, std::sin(0.35f), 0.0f, std::cos(0.35f));
    ExpectQuaternionNear(XMQuaternionSlerp(a, b, 1.0f), std::sin(-0.65f), 0.0f, 0.0f, std::cos(-0.65f));

    // Inside the near-parallel epsilon slerp falls back to a lerp, which must stay a finite unit
    // quaternion halfway between the two angles.
    const float nearHalfAngle = 0.35f + 0.5e-4f;
    const XMVECTOR almostA = XMVectorSet(0.0f, std::sin(nearHalfAngle), 0.0f, std::cos(nearHalfAngle));
    const XMFLOAT4 mid = ToFloat4(XMQuaternionSlerp(a, almostA, 0.5f));
    EXPECT_TRUE(std::isfinite(mid.y) && std::isfinite(mid.w));
    EXPECT_NEAR(Length4(mid), 1.0f, kTolerance);
    EXPECT_NEAR(mid.y, std::sin(0.35f + 0.25e-4f), kTolerance);
}

// ---------------------------------------------------------------------------
// XMQuaternionMultiply / XMMatrixRotationQuaternion / XMMatrixRotationAxis
// ---------------------------------------------------------------------------

TEST(DXMathStub_QuaternionMultiplyAppliesFirstArgumentFirst)
{
    // XMQuaternionMultiply(q1, q2) is "rotate by q1, then by q2", the Hamilton product q2*q1.
    // For q1 = 90 degrees about X and q2 = 90 degrees about Y that is (0.5, 0.5, -0.5, 0.5);
    // the reversed product q1*q2 has z = +0.5.
    const XMVECTOR aboutX = XMVectorSet(kHalfSqrt2, 0.0f, 0.0f, kHalfSqrt2);
    const XMVECTOR aboutY = XMVectorSet(0.0f, kHalfSqrt2, 0.0f, kHalfSqrt2);
    const XMVECTOR combined = XMQuaternionMultiply(aboutX, aboutY);
    ExpectQuaternionNear(combined, 0.5f, 0.5f, -0.5f, 0.5f);

    // +Y turned 90 degrees about X is +Z, which 90 degrees about Y takes to +X.
    const XMFLOAT4 v =
        ToFloat4(XMVector3TransformNormal(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), XMMatrixRotationQuaternion(combined)));
    EXPECT_NEAR(v.x, 1.0f, kTolerance);
    EXPECT_NEAR(v.y, 0.0f, kTolerance);
    EXPECT_NEAR(v.z, 0.0f, kTolerance);
}

TEST(DXMathStub_RotationQuaternionMatchesClosedFormRotationZ)
{
    const XMVECTOR q = XMVectorSet(0.0f, 0.0f, std::sin(0.3f), std::cos(0.3f));
    ExpectMatricesNear(XMMatrixRotationQuaternion(q), ClosedFormRotationZ(0.6f));
}

TEST(DXMathStub_RotationAxisNormalizesTheAxis)
{
    ExpectMatricesNear(XMMatrixRotationAxis(XMVectorSet(0.0f, 0.0f, 5.0f, 0.0f), 0.6f), ClosedFormRotationZ(0.6f));
}

// ---------------------------------------------------------------------------
// XMMatrixDecompose / XMQuaternionRotationMatrix
// ---------------------------------------------------------------------------

TEST(DXMathStub_DecomposeRecoversScaleRotationAndTranslation)
{
    // Scale (2, 3, 4), 0.6 rad about Z, translation (5, -6, 7).
    const XMMATRIX composed =
        XMMatrixMultiply(XMMatrixMultiply(XMMatrixScaling(2.0f, 3.0f, 4.0f), ClosedFormRotationZ(0.6f)),
                         XMMatrixTranslation(5.0f, -6.0f, 7.0f));

    XMVECTOR scale, rotation, translation;
    EXPECT_TRUE(XMMatrixDecompose(&scale, &rotation, &translation, composed));

    const XMFLOAT4 s = ToFloat4(scale);
    EXPECT_NEAR(s.x, 2.0f, kTolerance);
    EXPECT_NEAR(s.y, 3.0f, kTolerance);
    EXPECT_NEAR(s.z, 4.0f, kTolerance);
    const XMFLOAT4 t = ToFloat4(translation);
    EXPECT_NEAR(t.x, 5.0f, kTolerance);
    EXPECT_NEAR(t.y, -6.0f, kTolerance);
    EXPECT_NEAR(t.z, 7.0f, kTolerance);

    // q and -q are the same rotation, so compare |q . expected|. The old stub returned identity (0.955).
    const XMFLOAT4 expected{0.0f, 0.0f, std::sin(0.3f), std::cos(0.3f)};
    const XMFLOAT4 r = ToFloat4(rotation);
    EXPECT_NEAR(std::fabs(Dot4(r, expected)), 1.0f, kTolerance);
    EXPECT_NEAR(Length4(r), 1.0f, kTolerance);
}

TEST(DXMathStub_RotationMatrixRoundTripsEveryBranch)
{
    // Angles near 0, 90, 180 and 270 degrees about different axes reach each of the four
    // largest-component branches of XMQuaternionRotationMatrix.
    const float angles[] = {0.2f, 1.6f, 3.1f, 4.7f};
    const float axes[][3] = {
        {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {2.0f / 3.0f, -2.0f / 3.0f, 1.0f / 3.0f}};
    for (const auto& axis : axes)
    {
        for (const float angle : angles)
        {
            const float s = std::sin(angle * 0.5f);
            const XMFLOAT4 q{axis[0] * s, axis[1] * s, axis[2] * s, std::cos(angle * 0.5f)};
            const XMFLOAT4 back = ToFloat4(XMQuaternionRotationMatrix(XMMatrixRotationQuaternion(XMLoadFloat4(&q))));
            EXPECT_NEAR(std::fabs(Dot4(back, q)), 1.0f, kTolerance);
        }
    }
}

TEST(DXMathStub_DecomposeMirroredMatrixRecomposes)
{
    const float halfAngle = 0.45f;
    const XMVECTOR rotation = XMVectorSet(0.0f, kHalfSqrt2 * std::sin(halfAngle), kHalfSqrt2 * std::sin(halfAngle),
                                          std::cos(halfAngle));
    const XMMATRIX mirrored = ComposeSRT(-2.0f, 3.0f, 4.0f, rotation, 1.0f, 2.0f, 3.0f);

    XMVECTOR scale, outRotation, translation;
    EXPECT_TRUE(XMMatrixDecompose(&scale, &outRotation, &translation, mirrored));

    // The reflection is folded into the largest axis, so check the sign of the product and that
    // the parts rebuild the input rather than the individual components.
    const XMFLOAT4 s = ToFloat4(scale);
    const XMFLOAT4 t = ToFloat4(translation);
    EXPECT_TRUE(s.x * s.y * s.z < 0.0f);
    EXPECT_NEAR(std::fabs(s.x * s.y * s.z), 24.0f, 1e-3f);
    ExpectMatricesNear(ComposeSRT(s.x, s.y, s.z, outRotation, t.x, t.y, t.z), mirrored);
}

TEST(DXMathStub_DecomposeRebuildsZeroScaleAxis)
{
    // A bone scaled to zero on one axis still has a rotation on the others. DirectXMath rebuilds
    // the degenerate basis vector and succeeds; the decomposition must reproduce the matrix.
    const XMMATRIX flattened = XMMatrixMultiply(XMMatrixScaling(1.0f, 0.0f, 1.0f), ClosedFormRotationZ(0.6f));

    XMVECTOR scale, rotation, translation;
    EXPECT_TRUE(XMMatrixDecompose(&scale, &rotation, &translation, flattened));

    const XMFLOAT4 s = ToFloat4(scale);
    EXPECT_NEAR(std::fabs(s.x), 1.0f, kTolerance);
    EXPECT_NEAR(s.y, 0.0f, kTolerance);
    EXPECT_NEAR(std::fabs(s.z), 1.0f, kTolerance);
    EXPECT_NEAR(Length4(ToFloat4(rotation)), 1.0f, kTolerance);
    ExpectMatricesNear(ComposeSRT(s.x, s.y, s.z, rotation, 0.0f, 0.0f, 0.0f), flattened);
}

TEST(DXMathStub_DecomposeRejectsShear)
{
    // The +Y basis vector leans into +X, so no scale * rotation reproduces it.
    const XMMATRIX sheared = XMMatrixSet(1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    XMVECTOR scale, rotation, translation;
    EXPECT_FALSE(XMMatrixDecompose(&scale, &rotation, &translation, sheared));
}

// ---------------------------------------------------------------------------
// Production animation code that goes through the math above
// ---------------------------------------------------------------------------

TEST(DXMathStub_AnimationInterpolateRotationSlerpsBetweenKeys)
{
    Spark::Animation::BoneAnimation channel;
    Spark::Animation::QuatKey start{};
    start.time = 0.0f;
    start.value = {0.0f, 0.0f, 0.0f, 1.0f};
    Spark::Animation::QuatKey end{};
    end.time = 1.0f;
    end.value = {0.0f, 0.0f, kHalfSqrt2, kHalfSqrt2};
    channel.rotationKeys = {start, end};

    const XMFLOAT4 mid = channel.InterpolateRotation(0.5f);
    EXPECT_NEAR(mid.x, 0.0f, kTolerance);
    EXPECT_NEAR(mid.y, 0.0f, kTolerance);
    EXPECT_NEAR(mid.z, kSin22_5, kTolerance);
    EXPECT_NEAR(mid.w, kCos22_5, kTolerance);
    EXPECT_NEAR(Length4(mid), 1.0f, kTolerance);
}

TEST(DXMathStub_AnimationBlendTransformsBlendsRotation)
{
    // Blending identity with a 90 degree Z turn at 0.5 gives a 45 degree Z turn. With the old
    // identity-rotation Decompose stub both inputs decomposed to identity and so did the blend.
    const XMFLOAT4X4 identity = ToFloat4x4(XMMatrixIdentity());
    const XMFLOAT4X4 quarterTurn = ToFloat4x4(ClosedFormRotationZ(kHalfPi));

    std::vector<XMFLOAT4X4> blended;
    Spark::Animation::AnimationEvaluator::BlendTransforms({identity}, {quarterTurn}, 0.5f, blended);

    ASSERT_EQ(blended.size(), static_cast<size_t>(1));
    const XMFLOAT4X4 expected = ToFloat4x4(ClosedFormRotationZ(kHalfPi * 0.5f));
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
            EXPECT_NEAR(blended[0].m[row][col], expected.m[row][col], kTolerance);
    }
}
