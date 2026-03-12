/**
 * @file TestUpscalingSystem.cpp
 * @brief Unit tests for the DLSS/FSR/XeSS upscaling system
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Graphics/UpscalingSystem.h"

TEST(Upscaling_QualityRenderScale)
{
    // Ultra Performance should render at roughly 33% resolution
    float scale = UpscalingSettings::GetRenderScale(UpscalingQuality::UltraPerformance);
    EXPECT_GT(scale, 0.3f);
    EXPECT_LT(scale, 0.4f);

    // Performance should render at roughly 50% resolution
    scale = UpscalingSettings::GetRenderScale(UpscalingQuality::Performance);
    EXPECT_GT(scale, 0.45f);
    EXPECT_LT(scale, 0.55f);

    // Quality should render at roughly 67% resolution
    scale = UpscalingSettings::GetRenderScale(UpscalingQuality::Quality);
    EXPECT_GT(scale, 0.6f);
    EXPECT_LT(scale, 0.7f);

    // Native should be 1.0
    scale = UpscalingSettings::GetRenderScale(UpscalingQuality::Native);
    EXPECT_NEAR(scale, 1.0f, 0.01f);
}

TEST(Upscaling_RenderResolutionCalculation)
{
    UpscalingSettings settings;
    settings.mode = UpscalingMode::FSR1;
    settings.quality = UpscalingQuality::Performance;

    uint32_t w = 0, h = 0;
    settings.CalculateRenderResolution(1920, 1080, w, h);

    // At Performance quality (~50%), 1920x1080 -> ~960x540
    EXPECT_GT(w, 900u);
    EXPECT_LT(w, 1000u);
    EXPECT_GT(h, 500u);
    EXPECT_LT(h, 560u);

    // 4K at Balanced quality (~58%)
    settings.quality = UpscalingQuality::Balanced;
    settings.CalculateRenderResolution(3840, 2160, w, h);
    EXPECT_GT(w, 2100u);
    EXPECT_LT(w, 2400u);
    EXPECT_GT(h, 1100u);
    EXPECT_LT(h, 1350u);
}

TEST(Upscaling_ModeInputRequirements)
{
    // FSR 1.0 only needs color
    auto fsr1Reqs = UpscalingInputRequirements::ForMode(UpscalingMode::FSR1);
    EXPECT_TRUE(fsr1Reqs.needsColor);
    EXPECT_FALSE(fsr1Reqs.needsMotionVectors);
    EXPECT_FALSE(fsr1Reqs.needsDepth);

    // FSR 2.0 needs color, depth, motion vectors, and jitter
    auto fsr2Reqs = UpscalingInputRequirements::ForMode(UpscalingMode::FSR2);
    EXPECT_TRUE(fsr2Reqs.needsColor);
    EXPECT_TRUE(fsr2Reqs.needsMotionVectors);
    EXPECT_TRUE(fsr2Reqs.needsDepth);
    EXPECT_TRUE(fsr2Reqs.needsJitterOffset);

    // DLSS needs similar to FSR 2.0 plus exposure
    auto dlssReqs = UpscalingInputRequirements::ForMode(UpscalingMode::DLSS);
    EXPECT_TRUE(dlssReqs.needsColor);
    EXPECT_TRUE(dlssReqs.needsMotionVectors);
    EXPECT_TRUE(dlssReqs.needsExposure);
}

TEST(Upscaling_FSR1Constants)
{
    // FSR1 constant buffers use XMFLOAT4 packed fields
    FSR1EASUConstants easu{};
    easu.const0 = {960.0f, 540.0f, 1920.0f, 1080.0f};
    EXPECT_NEAR(easu.const0.x, 960.0f, 0.01f);
    EXPECT_NEAR(easu.const0.z, 1920.0f, 0.01f);

    FSR1RCASConstants rcas{};
    rcas.const0 = {0.5f, 0.0f, 0.0f, 0.0f};
    EXPECT_GE(rcas.const0.x, 0.0f);
    EXPECT_LE(rcas.const0.x, 2.0f);
}

TEST(Upscaling_SettingsDefaults)
{
    UpscalingSettings settings;

    EXPECT_TRUE(settings.mode == UpscalingMode::None);
    EXPECT_TRUE(settings.quality == UpscalingQuality::Quality);
    EXPECT_GE(settings.sharpness, 0.0f);

    // When mode is None, CalculateRenderResolution returns display res
    uint32_t w = 0, h = 0;
    settings.CalculateRenderResolution(1920, 1080, w, h);
    EXPECT_EQ(w, 1920u);
    EXPECT_EQ(h, 1080u);
}
