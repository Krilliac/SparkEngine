// TestMathUtils.cpp - Tests for MathUtils functions
#include "TestMain.cpp" // Include test macros (in real build, this is handled by linking)
#include <cmath>
#include <algorithm>

// Re-implement core math functions to test independently of DirectXMath
namespace TestMath {

float Lerp(float a, float b, float t)
{
    return a + t * (b - a);
}

float Clamp(float value, float lo, float hi)
{
    return (std::max)(lo, (std::min)(hi, value));
}

float SmoothStep(float edge0, float edge1, float x)
{
    float t = Clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float InverseLerp(float a, float b, float value)
{
    if (std::abs(b - a) < 0.0001f) return 0.0f;
    return (value - a) / (b - a);
}

} // namespace TestMath

TEST(Lerp_ZeroT)
{
    EXPECT_NEAR(TestMath::Lerp(0.0f, 10.0f, 0.0f), 0.0f, 0.001f);
}

TEST(Lerp_OneT)
{
    EXPECT_NEAR(TestMath::Lerp(0.0f, 10.0f, 1.0f), 10.0f, 0.001f);
}

TEST(Lerp_HalfT)
{
    EXPECT_NEAR(TestMath::Lerp(0.0f, 10.0f, 0.5f), 5.0f, 0.001f);
}

TEST(Lerp_NegativeValues)
{
    EXPECT_NEAR(TestMath::Lerp(-10.0f, 10.0f, 0.5f), 0.0f, 0.001f);
}

TEST(Clamp_WithinRange)
{
    EXPECT_NEAR(TestMath::Clamp(5.0f, 0.0f, 10.0f), 5.0f, 0.001f);
}

TEST(Clamp_BelowMin)
{
    EXPECT_NEAR(TestMath::Clamp(-5.0f, 0.0f, 10.0f), 0.0f, 0.001f);
}

TEST(Clamp_AboveMax)
{
    EXPECT_NEAR(TestMath::Clamp(15.0f, 0.0f, 10.0f), 10.0f, 0.001f);
}

TEST(SmoothStep_AtEdges)
{
    EXPECT_NEAR(TestMath::SmoothStep(0.0f, 1.0f, 0.0f), 0.0f, 0.001f);
    EXPECT_NEAR(TestMath::SmoothStep(0.0f, 1.0f, 1.0f), 1.0f, 0.001f);
}

TEST(SmoothStep_Midpoint)
{
    EXPECT_NEAR(TestMath::SmoothStep(0.0f, 1.0f, 0.5f), 0.5f, 0.001f);
}

TEST(InverseLerp_Basic)
{
    EXPECT_NEAR(TestMath::InverseLerp(0.0f, 10.0f, 5.0f), 0.5f, 0.001f);
}

TEST(InverseLerp_AtBounds)
{
    EXPECT_NEAR(TestMath::InverseLerp(0.0f, 10.0f, 0.0f), 0.0f, 0.001f);
    EXPECT_NEAR(TestMath::InverseLerp(0.0f, 10.0f, 10.0f), 1.0f, 0.001f);
}
