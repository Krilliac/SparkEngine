/**
 * @file TestUpscalingSystem.cpp
 * @brief Unit tests for the DLSS/FSR/XeSS/SparkSR upscaling system
 */

#include "TestFramework.h"
#include "Graphics/UpscalingSystem.h"

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

    auto [w, h] = settings.CalculateRenderResolution(1920, 1080);

    // At Performance quality (~50%), 1920x1080 -> ~960x540
    EXPECT_GT(w, 900u);
    EXPECT_LT(w, 1000u);
    EXPECT_GT(h, 500u);
    EXPECT_LT(h, 560u);

    // 4K at Balanced quality (~58%)
    settings.quality = UpscalingQuality::Balanced;
    auto [w2, h2] = settings.CalculateRenderResolution(3840, 2160);
    EXPECT_GT(w2, 2100u);
    EXPECT_LT(w2, 2400u);
    EXPECT_GT(h2, 1100u);
    EXPECT_LT(h2, 1350u);
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
    auto [w, h] = settings.CalculateRenderResolution(1920, 1080);
    EXPECT_EQ(w, 1920u);
    EXPECT_EQ(h, 1080u);
}

// =============================================================================
// SparkSR Tests
// =============================================================================

TEST(SparkSR_InputRequirements)
{
    // SparkSR is a temporal upscaler — needs all temporal inputs
    auto reqs = UpscalingInputRequirements::ForMode(UpscalingMode::SparkSR);
    EXPECT_TRUE(reqs.needsColor);
    EXPECT_TRUE(reqs.needsDepth);
    EXPECT_TRUE(reqs.needsMotionVectors);
    EXPECT_TRUE(reqs.needsExposure);
    EXPECT_TRUE(reqs.needsReactiveMask);
    EXPECT_TRUE(reqs.needsJitterOffset);
}

TEST(SparkSR_RenderResolutionCalculation)
{
    // SparkSR uses the same quality presets as other temporal modes
    UpscalingSettings settings;
    settings.mode = UpscalingMode::SparkSR;
    settings.quality = UpscalingQuality::Quality;

    auto [w, h] = settings.CalculateRenderResolution(1920, 1080);

    // Quality (~67%): 1920x1080 -> ~1286x724
    EXPECT_GT(w, 1200u);
    EXPECT_LT(w, 1400u);
    EXPECT_GT(h, 680u);
    EXPECT_LT(h, 780u);

    // Performance (~50%): 3840x2160 -> ~1920x1080
    settings.quality = UpscalingQuality::Performance;
    auto [w2, h2] = settings.CalculateRenderResolution(3840, 2160);
    EXPECT_GT(w2, 1800u);
    EXPECT_LT(w2, 2000u);
    EXPECT_GT(h2, 1000u);
    EXPECT_LT(h2, 1150u);
}

TEST(SparkSR_ConstantsAlignment)
{
    // SparkSRConstants must be 16-byte aligned for GPU constant buffers
    static_assert(alignof(SparkSRConstants) == 16, "SparkSRConstants must be 16-byte aligned");
    static_assert(sizeof(SparkSRConstants) % 16 == 0, "SparkSRConstants size must be multiple of 16");

    // Verify struct can be populated correctly
    SparkSRConstants constants{};
    constants.renderSize = {960.0f, 540.0f, 1.0f / 960.0f, 1.0f / 540.0f};
    constants.displaySize = {1920.0f, 1080.0f, 1.0f / 1920.0f, 1.0f / 1080.0f};
    constants.jitterOffset = {0.25f, -0.125f, 0.0f, 0.0f};
    constants.temporalParams = {0.0f, 0.0f, 0.5f, 0.03f};
    constants.motionParams = {1.0f, 1.0f, 0.05f, 1.0f};

    EXPECT_NEAR(constants.renderSize.x, 960.0f, 0.01f);
    EXPECT_NEAR(constants.displaySize.z, 1.0f / 1920.0f, 0.0001f);
    EXPECT_NEAR(constants.motionParams.w, 1.0f, 0.01f); // varianceGamma
}

TEST(SparkSR_ModeEnumExists)
{
    // SparkSR should be a valid UpscalingMode value distinct from other modes
    UpscalingSettings settings;
    settings.mode = UpscalingMode::SparkSR;

    EXPECT_TRUE(settings.mode == UpscalingMode::SparkSR);
    EXPECT_FALSE(settings.mode == UpscalingMode::None);
    EXPECT_FALSE(settings.mode == UpscalingMode::FSR1);
    EXPECT_FALSE(settings.mode == UpscalingMode::FSR2);
    EXPECT_FALSE(settings.mode == UpscalingMode::DLSS);
    EXPECT_FALSE(settings.mode == UpscalingMode::XeSS);
}

TEST(SparkSR_ModeIsTemporalUpscaler)
{
    // SparkSR should use the same render scale as other modes
    UpscalingSettings settings;
    settings.mode = UpscalingMode::SparkSR;
    settings.quality = UpscalingQuality::UltraPerformance;

    auto [w, h] = settings.CalculateRenderResolution(1920, 1080);

    // Ultra Performance (~33%): should render significantly below native
    EXPECT_LT(w, 700u);
    EXPECT_LT(h, 400u);
    EXPECT_GT(w, 500u);
    EXPECT_GT(h, 300u);
}
