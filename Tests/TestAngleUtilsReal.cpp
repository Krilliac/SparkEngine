/**
 * @file TestAngleUtilsReal.cpp
 * @brief Real-class tests for Spark::AngleUtils (replacing fake-coverage TestAngleUtils.cpp)
 *
 * Phase JJ deep-wire — first batch of fake-coverage conversions.
 */

#include "TestFramework.h"
#include "Utils/AngleUtils.h"

#include <cmath>

TEST(AngleUtilsReal_ToRadiansAndToDegrees)
{
    EXPECT_NEAR(Spark::AngleUtils::ToRadians(180.0f), Spark::AngleUtils::PI, 0.001f);
    EXPECT_NEAR(Spark::AngleUtils::ToDegrees(Spark::AngleUtils::PI), 180.0f, 0.001f);
    EXPECT_NEAR(Spark::AngleUtils::ToRadians(0.0f), 0.0f, 0.001f);
    EXPECT_NEAR(Spark::AngleUtils::ToDegrees(0.0f), 0.0f, 0.001f);
}

TEST(AngleUtilsReal_NormalizeAnglePositive)
{
    // Positive over-range: 3*PI should normalize to PI (or -PI).
    const float normalized = Spark::AngleUtils::NormalizeAngle(3.0f * Spark::AngleUtils::PI);
    EXPECT_TRUE(normalized >= -Spark::AngleUtils::PI && normalized <= Spark::AngleUtils::PI);
}

TEST(AngleUtilsReal_NormalizeAngleNegative)
{
    const float normalized = Spark::AngleUtils::NormalizeAngle(-3.0f * Spark::AngleUtils::PI);
    EXPECT_TRUE(normalized >= -Spark::AngleUtils::PI && normalized <= Spark::AngleUtils::PI);
}

TEST(AngleUtilsReal_NormalizeAngleInRange)
{
    // A value already in [-PI, PI] should pass through.
    EXPECT_NEAR(Spark::AngleUtils::NormalizeAngle(0.5f), 0.5f, 0.001f);
    EXPECT_NEAR(Spark::AngleUtils::NormalizeAngle(-0.5f), -0.5f, 0.001f);
}

TEST(AngleUtilsReal_AngleDeltaShortestPath)
{
    // Turning from 170deg to -170deg should be +20deg (short way), not -340.
    const float from = Spark::AngleUtils::ToRadians(170.0f);
    const float to = Spark::AngleUtils::ToRadians(-170.0f);
    const float delta = Spark::AngleUtils::AngleDelta(from, to);
    EXPECT_NEAR(std::abs(Spark::AngleUtils::ToDegrees(delta)), 20.0f, 0.5f);
}

TEST(AngleUtilsReal_LerpAngleHalfway)
{
    const float mid = Spark::AngleUtils::LerpAngle(0.0f, Spark::AngleUtils::PI / 2.0f, 0.5f);
    EXPECT_NEAR(mid, Spark::AngleUtils::PI / 4.0f, 0.01f);
}

TEST(AngleUtilsReal_Constants)
{
    EXPECT_NEAR(Spark::AngleUtils::PI, 3.14159265f, 0.0001f);
    EXPECT_NEAR(Spark::AngleUtils::TWO_PI, 6.28318530f, 0.0001f);
    EXPECT_NEAR(Spark::AngleUtils::DEG_TO_RAD * 180.0f, Spark::AngleUtils::PI, 0.0001f);
    EXPECT_NEAR(Spark::AngleUtils::RAD_TO_DEG * Spark::AngleUtils::PI, 180.0f, 0.0001f);
}
