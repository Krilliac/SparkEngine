// TestColorUtils.cpp - Tests for Spark::Color and Spark::ColorUtils
#include "TestFramework.h"
#include "Utils/ColorUtils.h"
#include <cmath>

TEST(Color_DefaultIsBlack)
{
    Spark::Color c;
    EXPECT_NEAR(c.r, 0.0f, 0.001f);
    EXPECT_NEAR(c.g, 0.0f, 0.001f);
    EXPECT_NEAR(c.b, 0.0f, 0.001f);
    EXPECT_NEAR(c.a, 1.0f, 0.001f);
}

TEST(Color_Presets)
{
    auto red = Spark::Color::Red();
    EXPECT_NEAR(red.r, 1.0f, 0.001f);
    EXPECT_NEAR(red.g, 0.0f, 0.001f);
    EXPECT_NEAR(red.b, 0.0f, 0.001f);

    auto clear = Spark::Color::Clear();
    EXPECT_NEAR(clear.a, 0.0f, 0.001f);
}

TEST(Color_ToRGBA32)
{
    Spark::Color white(1.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_EQ(white.ToRGBA32(), 0xFFFFFFFFu);

    Spark::Color black(0.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_EQ(black.ToRGBA32(), 0x000000FFu);
}

TEST(Color_FromRGBA32)
{
    auto c = Spark::Color::FromRGBA32(0xFF0000FF);
    EXPECT_NEAR(c.r, 1.0f, 0.01f);
    EXPECT_NEAR(c.g, 0.0f, 0.01f);
    EXPECT_NEAR(c.b, 0.0f, 0.01f);
    EXPECT_NEAR(c.a, 1.0f, 0.01f);
}

TEST(Color_FromHex6)
{
    auto c = Spark::Color::FromHex("#FF8000");
    EXPECT_NEAR(c.r, 1.0f, 0.01f);
    EXPECT_GT(c.g, 0.4f);
    EXPECT_NEAR(c.b, 0.0f, 0.01f);
    EXPECT_NEAR(c.a, 1.0f, 0.01f);
}

TEST(Color_FromHex8)
{
    auto c = Spark::Color::FromHex("#FF000080");
    EXPECT_NEAR(c.r, 1.0f, 0.01f);
    EXPECT_NEAR(c.a, 0.502f, 0.01f);
}

TEST(Color_FromHex3)
{
    auto c = Spark::Color::FromHex("#F00");
    EXPECT_NEAR(c.r, 1.0f, 0.01f);
    EXPECT_NEAR(c.g, 0.0f, 0.01f);
    EXPECT_NEAR(c.b, 0.0f, 0.01f);
}

TEST(Color_ToHex)
{
    auto hex = Spark::Color::Red().ToHex();
    EXPECT_TRUE(hex == "#FF0000FF");
}

TEST(Color_WithAlpha)
{
    auto c = Spark::Color::Red().WithAlpha(0.5f);
    EXPECT_NEAR(c.r, 1.0f, 0.001f);
    EXPECT_NEAR(c.a, 0.5f, 0.001f);
}

TEST(Color_Lerp)
{
    auto result = Spark::Color::Lerp(Spark::Color::Black(), Spark::Color::White(), 0.5f);
    EXPECT_NEAR(result.r, 0.5f, 0.001f);
    EXPECT_NEAR(result.g, 0.5f, 0.001f);
    EXPECT_NEAR(result.b, 0.5f, 0.001f);
}

TEST(Color_AdjustBrightness)
{
    auto c = Spark::Color(0.5f, 0.5f, 0.5f).AdjustBrightness(2.0f);
    EXPECT_NEAR(c.r, 1.0f, 0.001f);
    EXPECT_NEAR(c.g, 1.0f, 0.001f);
}

TEST(ColorUtils_RGBToHSV_Red)
{
    auto [h, s, v] = Spark::ColorUtils::RGBToHSV(1.0f, 0.0f, 0.0f);
    EXPECT_NEAR(h, 0.0f, 1.0f);
    EXPECT_NEAR(s, 1.0f, 0.01f);
    EXPECT_NEAR(v, 1.0f, 0.01f);
}

TEST(ColorUtils_RGBToHSV_Green)
{
    auto [h, s, v] = Spark::ColorUtils::RGBToHSV(0.0f, 1.0f, 0.0f);
    EXPECT_NEAR(h, 120.0f, 1.0f);
    EXPECT_NEAR(s, 1.0f, 0.01f);
    EXPECT_NEAR(v, 1.0f, 0.01f);
}

TEST(ColorUtils_HSVToRGB_Red)
{
    auto [r, g, b] = Spark::ColorUtils::HSVToRGB(0.0f, 1.0f, 1.0f);
    EXPECT_NEAR(r, 1.0f, 0.01f);
    EXPECT_NEAR(g, 0.0f, 0.01f);
    EXPECT_NEAR(b, 0.0f, 0.01f);
}

TEST(ColorUtils_Roundtrip)
{
    auto [h, s, v] = Spark::ColorUtils::RGBToHSV(0.3f, 0.6f, 0.9f);
    auto [r, g, b] = Spark::ColorUtils::HSVToRGB(h, s, v);
    EXPECT_NEAR(r, 0.3f, 0.02f);
    EXPECT_NEAR(g, 0.6f, 0.02f);
    EXPECT_NEAR(b, 0.9f, 0.02f);
}

TEST(ColorUtils_Luminance)
{
    float lum = Spark::ColorUtils::Luminance(Spark::Color::White());
    EXPECT_NEAR(lum, 1.0f, 0.01f);

    float lumBlack = Spark::ColorUtils::Luminance(Spark::Color::Black());
    EXPECT_NEAR(lumBlack, 0.0f, 0.01f);
}

TEST(ColorUtils_FromHSV)
{
    auto c = Spark::ColorUtils::FromHSV(240.0f, 1.0f, 1.0f, 0.5f);
    EXPECT_NEAR(c.b, 1.0f, 0.01f);
    EXPECT_NEAR(c.a, 0.5f, 0.001f);
}

TEST(Color_Equality)
{
    EXPECT_TRUE(Spark::Color::Red() == Spark::Color::Red());
    EXPECT_TRUE(Spark::Color::Red() != Spark::Color::Blue());
}
