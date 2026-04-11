/**
 * @file TestColorUtilsReal.cpp
 * @brief Real-class tests for Spark::ColorUtils namespace
 */

#include "TestFramework.h"
#include "Utils/ColorUtils.h"

#include <tuple>

TEST(ColorUtilsReal_RGBToHSVBlackWhite)
{
    auto [h1, s1, v1] = Spark::ColorUtils::RGBToHSV(0.0f, 0.0f, 0.0f);
    EXPECT_NEAR(v1, 0.0f, 0.001f);
    EXPECT_NEAR(s1, 0.0f, 0.001f);

    auto [h2, s2, v2] = Spark::ColorUtils::RGBToHSV(1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(v2, 1.0f, 0.001f);
    EXPECT_NEAR(s2, 0.0f, 0.001f);
}

TEST(ColorUtilsReal_RGBToHSVPrimaryColors)
{
    // Red
    auto [h_r, s_r, v_r] = Spark::ColorUtils::RGBToHSV(1.0f, 0.0f, 0.0f);
    EXPECT_NEAR(h_r, 0.0f, 0.5f);
    EXPECT_NEAR(s_r, 1.0f, 0.01f);
    EXPECT_NEAR(v_r, 1.0f, 0.01f);

    // Green
    auto [h_g, s_g, v_g] = Spark::ColorUtils::RGBToHSV(0.0f, 1.0f, 0.0f);
    EXPECT_NEAR(h_g, 120.0f, 0.5f);
    EXPECT_NEAR(s_g, 1.0f, 0.01f);

    // Blue
    auto [h_b, s_b, v_b] = Spark::ColorUtils::RGBToHSV(0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(h_b, 240.0f, 0.5f);
    EXPECT_NEAR(s_b, 1.0f, 0.01f);
}

TEST(ColorUtilsReal_RGBToHSVRoundTrip)
{
    // RGB -> HSV -> RGB should preserve the original color.
    const float origR = 0.6f;
    const float origG = 0.3f;
    const float origB = 0.8f;
    auto [h, s, v] = Spark::ColorUtils::RGBToHSV(origR, origG, origB);
    auto [r, g, b] = Spark::ColorUtils::HSVToRGB(h, s, v);
    EXPECT_NEAR(r, origR, 0.01f);
    EXPECT_NEAR(g, origG, 0.01f);
    EXPECT_NEAR(b, origB, 0.01f);
}

TEST(ColorUtilsReal_HSVToRGBBlack)
{
    auto [r, g, b] = Spark::ColorUtils::HSVToRGB(0.0f, 0.0f, 0.0f);
    EXPECT_NEAR(r, 0.0f, 0.001f);
    EXPECT_NEAR(g, 0.0f, 0.001f);
    EXPECT_NEAR(b, 0.0f, 0.001f);
}
