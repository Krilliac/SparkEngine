/**
 * @file TestSplineMathReal.cpp
 * @brief Real-class tests for SplineMath (replacing fake-coverage TestSplineMath*.cpp)
 *
 * Phase JJ deep-wire.
 */

#include "TestFramework.h"
#include "Utils/SplineMath.h"

namespace
{
    bool NearlyEqual(const XMFLOAT3& a, const XMFLOAT3& b, float eps = 0.001f)
    {
        return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps && std::abs(a.z - b.z) < eps;
    }
} // namespace

TEST(SplineMathReal_CatmullRomT0EqualsP1)
{
    XMFLOAT3 p0{-1, 0, 0};
    XMFLOAT3 p1{0, 0, 0};
    XMFLOAT3 p2{1, 0, 0};
    XMFLOAT3 p3{2, 0, 0};
    auto r = SplineMath::CatmullRom(p0, p1, p2, p3, 0.0f);
    EXPECT_TRUE(NearlyEqual(r, p1));
}

TEST(SplineMathReal_CatmullRomT1EqualsP2)
{
    XMFLOAT3 p0{-1, 0, 0};
    XMFLOAT3 p1{0, 0, 0};
    XMFLOAT3 p2{1, 0, 0};
    XMFLOAT3 p3{2, 0, 0};
    auto r = SplineMath::CatmullRom(p0, p1, p2, p3, 1.0f);
    EXPECT_TRUE(NearlyEqual(r, p2));
}

TEST(SplineMathReal_CatmullRomMidpointCollinear)
{
    // For collinear evenly-spaced points, the midpoint should be halfway.
    XMFLOAT3 p0{-1, 0, 0};
    XMFLOAT3 p1{0, 0, 0};
    XMFLOAT3 p2{1, 0, 0};
    XMFLOAT3 p3{2, 0, 0};
    auto r = SplineMath::CatmullRom(p0, p1, p2, p3, 0.5f);
    EXPECT_NEAR(r.x, 0.5f, 0.01f);
    EXPECT_NEAR(r.y, 0.0f, 0.01f);
}

TEST(SplineMathReal_LerpPointHalfway)
{
    XMFLOAT3 a{0, 0, 0};
    XMFLOAT3 b{10, 20, 30};
    auto r = SplineMath::LerpPoint(a, b, 0.5f);
    EXPECT_NEAR(r.x, 5.0f, 0.001f);
    EXPECT_NEAR(r.y, 10.0f, 0.001f);
    EXPECT_NEAR(r.z, 15.0f, 0.001f);
}

TEST(SplineMathReal_LerpPointEndpoints)
{
    XMFLOAT3 a{1, 2, 3};
    XMFLOAT3 b{4, 5, 6};
    auto start = SplineMath::LerpPoint(a, b, 0.0f);
    auto end = SplineMath::LerpPoint(a, b, 1.0f);
    EXPECT_TRUE(NearlyEqual(start, a));
    EXPECT_TRUE(NearlyEqual(end, b));
}

TEST(SplineMathReal_CubicBezierEndpoints)
{
    XMFLOAT3 p0{0, 0, 0};
    XMFLOAT3 p1{0, 10, 0};
    XMFLOAT3 p2{10, 10, 0};
    XMFLOAT3 p3{10, 0, 0};
    auto start = SplineMath::CubicBezier(p0, p1, p2, p3, 0.0f);
    auto end = SplineMath::CubicBezier(p0, p1, p2, p3, 1.0f);
    EXPECT_TRUE(NearlyEqual(start, p0));
    EXPECT_TRUE(NearlyEqual(end, p3));
}

TEST(SplineMathReal_CatmullRomSegmentLengthPositive)
{
    XMFLOAT3 p0{-1, 0, 0};
    XMFLOAT3 p1{0, 0, 0};
    XMFLOAT3 p2{1, 0, 0};
    XMFLOAT3 p3{2, 0, 0};
    const float len = SplineMath::CatmullRomSegmentLength(p0, p1, p2, p3);
    EXPECT_TRUE(len > 0.5f);
    EXPECT_TRUE(len < 2.0f);
}
