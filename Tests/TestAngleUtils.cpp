// TestAngleUtils.cpp - Tests for AngleUtils helpers
// Tests ToRadians, ToDegrees, NormalizeAngle, AngleDelta, LerpAngle

#include "TestFramework.h"
#include <cmath>

// Simplified AngleUtils for testing (mirrors engine implementation)
namespace AngleUtils
{
    inline constexpr float PI = 3.14159265358979323846f;
    inline constexpr float TWO_PI = 2.0f * PI;
    inline constexpr float DEG_TO_RAD = PI / 180.0f;
    inline constexpr float RAD_TO_DEG = 180.0f / PI;

    [[nodiscard]] constexpr float ToRadians(float degrees) noexcept
    {
        return degrees * DEG_TO_RAD;
    }
    [[nodiscard]] constexpr float ToDegrees(float radians) noexcept
    {
        return radians * RAD_TO_DEG;
    }

    namespace Detail
    {
        [[nodiscard]] constexpr float Fmod(float a, float b) noexcept
        {
            return a - static_cast<float>(static_cast<int>(a / b)) * b;
        }
    } // namespace Detail

    [[nodiscard]] constexpr float NormalizeAngle(float radians) noexcept
    {
        radians = Detail::Fmod(radians + PI, TWO_PI);
        if (radians < 0.0f)
            radians += TWO_PI;
        return radians - PI;
    }

    [[nodiscard]] constexpr float AngleDelta(float from, float to) noexcept
    {
        return NormalizeAngle(to - from);
    }

    [[nodiscard]] constexpr float LerpAngle(float from, float to, float t) noexcept
    {
        return from + AngleDelta(from, to) * t;
    }
} // namespace AngleUtils

TEST(AngleUtils_ToRadians)
{
    EXPECT_NEAR(AngleUtils::ToRadians(0.0f), 0.0f, 1e-6f);
    EXPECT_NEAR(AngleUtils::ToRadians(180.0f), AngleUtils::PI, 1e-5f);
    EXPECT_NEAR(AngleUtils::ToRadians(360.0f), AngleUtils::TWO_PI, 1e-5f);
    EXPECT_NEAR(AngleUtils::ToRadians(90.0f), AngleUtils::PI / 2.0f, 1e-5f);
}

TEST(AngleUtils_ToDegrees)
{
    EXPECT_NEAR(AngleUtils::ToDegrees(0.0f), 0.0f, 1e-6f);
    EXPECT_NEAR(AngleUtils::ToDegrees(AngleUtils::PI), 180.0f, 1e-4f);
    EXPECT_NEAR(AngleUtils::ToDegrees(AngleUtils::TWO_PI), 360.0f, 1e-4f);
}

TEST(AngleUtils_RoundTrip)
{
    float deg = 45.0f;
    EXPECT_NEAR(AngleUtils::ToDegrees(AngleUtils::ToRadians(deg)), deg, 1e-5f);
}

TEST(AngleUtils_NormalizeAngle_AlreadyNormal)
{
    EXPECT_NEAR(AngleUtils::NormalizeAngle(0.0f), 0.0f, 1e-6f);
    EXPECT_NEAR(AngleUtils::NormalizeAngle(1.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(AngleUtils::NormalizeAngle(-1.0f), -1.0f, 1e-6f);
}

TEST(AngleUtils_NormalizeAngle_LargePositive)
{
    // 720 degrees = 4*PI radians -> should normalize near 0
    float result = AngleUtils::NormalizeAngle(4.0f * AngleUtils::PI);
    EXPECT_NEAR(result, 0.0f, 1e-4f);
}

TEST(AngleUtils_NormalizeAngle_LargeNegative)
{
    // -450 degrees = -2.5*PI -> should normalize to -0.5*PI
    float result = AngleUtils::NormalizeAngle(-2.5f * AngleUtils::PI);
    EXPECT_NEAR(result, -AngleUtils::PI / 2.0f, 1e-4f);
}

TEST(AngleUtils_AngleDelta_ShortestPath)
{
    // From 350 degrees to 10 degrees should be +20 degrees (crossing 0)
    float from = AngleUtils::ToRadians(350.0f);
    float to = AngleUtils::ToRadians(10.0f);
    float delta = AngleUtils::AngleDelta(from, to);
    EXPECT_NEAR(AngleUtils::ToDegrees(delta), 20.0f, 0.1f);
}

TEST(AngleUtils_AngleDelta_NegativeShortcut)
{
    // From 10 degrees to 350 degrees should be -20 degrees
    float from = AngleUtils::ToRadians(10.0f);
    float to = AngleUtils::ToRadians(350.0f);
    float delta = AngleUtils::AngleDelta(from, to);
    EXPECT_NEAR(AngleUtils::ToDegrees(delta), -20.0f, 0.1f);
}

TEST(AngleUtils_LerpAngle_Endpoints)
{
    float from = AngleUtils::ToRadians(10.0f);
    float to = AngleUtils::ToRadians(350.0f);

    EXPECT_NEAR(AngleUtils::LerpAngle(from, to, 0.0f), from, 1e-6f);
    EXPECT_NEAR(AngleUtils::NormalizeAngle(AngleUtils::LerpAngle(from, to, 1.0f)), AngleUtils::NormalizeAngle(to),
                1e-4f);
}

TEST(AngleUtils_LerpAngle_Midpoint)
{
    // Midpoint of 350 and 10 degrees (crossing 0) should be 0 degrees
    float from = AngleUtils::ToRadians(350.0f);
    float to = AngleUtils::ToRadians(10.0f);
    float mid = AngleUtils::LerpAngle(from, to, 0.5f);
    float midNorm = AngleUtils::NormalizeAngle(mid);
    EXPECT_NEAR(midNorm, 0.0f, 0.1f);
}
