// TestScreenSpaceEffects.cpp - Tests for SSAO, SSR, and contact shadows
// Uses the actual ScreenSpaceEffects.h header

#include "TestFramework.h"
#include "Graphics/ScreenSpaceEffects.h"

using namespace Spark::Graphics;

// =============================================================================
// Tests
// =============================================================================

TEST(SSE_Initialize)
{
    ScreenSpaceEffects sse;
    EXPECT_TRUE(sse.Initialize(1920, 1080));
    sse.Shutdown();
}

TEST(SSE_SSAODefaults)
{
    ScreenSpaceEffects sse;
    sse.Initialize();

    EXPECT_TRUE(sse.IsSSAOEnabled());
    auto& settings = sse.GetSSAOSettings();
    EXPECT_EQ(settings.kernelSize, 32);
    EXPECT_GT(settings.radius, 0.0f);
    EXPECT_GT(settings.intensity, 0.0f);

    sse.Shutdown();
}

TEST(SSE_SSAOKernelGeneration)
{
    ScreenSpaceEffects sse;
    sse.Initialize();

    auto kernel = sse.GenerateSSAOKernel(64);
    EXPECT_EQ((int)kernel.size(), 64);

    // All samples should have finite length
    for (const auto& s : kernel)
    {
        float len = s.Length();
        EXPECT_GT(len, 0.0f);
        EXPECT_LT(len, 2.0f);
    }

    sse.Shutdown();
}

TEST(SSE_SSAOKernelDeterministic)
{
    auto k1 = SSAOKernel::GenerateKernel(16, 42);
    auto k2 = SSAOKernel::GenerateKernel(16, 42);
    EXPECT_EQ((int)k1.size(), (int)k2.size());

    // Same seed should produce same results
    for (int i = 0; i < (int)k1.size(); ++i)
    {
        EXPECT_NEAR(k1[i].x, k2[i].x, 0.0001f);
        EXPECT_NEAR(k1[i].y, k2[i].y, 0.0001f);
        EXPECT_NEAR(k1[i].z, k2[i].z, 0.0001f);
    }
}

TEST(SSE_NoiseTextureGeneration)
{
    ScreenSpaceEffects sse;
    sse.Initialize();

    auto noise = sse.GenerateNoiseTexture(4);
    EXPECT_EQ((int)noise.size(), 16); // 4x4

    // All noise vectors should be unit length in XY
    for (const auto& n : noise)
    {
        float len = std::sqrt(n.x * n.x + n.y * n.y);
        EXPECT_NEAR(len, 1.0f, 0.01f);
        EXPECT_NEAR(n.z, 0.0f, 0.001f);
    }

    sse.Shutdown();
}

TEST(SSE_SSAOQualitySettings)
{
    SSAOSettings s;

    s.quality = SSAOQuality::Low;
    EXPECT_EQ(s.GetSampleCount(), 16);
    EXPECT_TRUE(s.IsHalfRes());

    s.quality = SSAOQuality::Medium;
    EXPECT_EQ(s.GetSampleCount(), 32);
    EXPECT_TRUE(s.IsHalfRes());

    s.quality = SSAOQuality::High;
    EXPECT_EQ(s.GetSampleCount(), 64);
    EXPECT_FALSE(s.IsHalfRes());

    s.quality = SSAOQuality::Ultra;
    EXPECT_EQ(s.GetSampleCount(), 128);
    EXPECT_FALSE(s.IsHalfRes());
}

TEST(SSE_SSRDefaults)
{
    ScreenSpaceEffects sse;
    sse.Initialize();

    EXPECT_FALSE(sse.IsSSREnabled());
    auto& settings = sse.GetSSRSettings();
    EXPECT_GT(settings.maxDistance, 0.0f);
    EXPECT_GT(settings.maxSteps, 0);

    sse.Shutdown();
}

TEST(SSE_SSRQualitySettings)
{
    SSRSettings s;

    s.quality = SSRQuality::Low;
    EXPECT_EQ(s.GetStepCount(), 16);

    s.quality = SSRQuality::Medium;
    EXPECT_EQ(s.GetStepCount(), 32);

    s.quality = SSRQuality::High;
    EXPECT_EQ(s.GetStepCount(), 64);

    s.quality = SSRQuality::Ultra;
    EXPECT_EQ(s.GetStepCount(), 128);
}

TEST(SSE_ContactShadowsDefault)
{
    ScreenSpaceEffects sse;
    sse.Initialize();

    EXPECT_FALSE(sse.IsContactShadowsEnabled());
    sse.SetContactShadowsEnabled(true);
    EXPECT_TRUE(sse.IsContactShadowsEnabled());

    auto& settings = sse.GetContactShadowSettings();
    EXPECT_GT(settings.stepCount, 0);
    EXPECT_GT(settings.intensity, 0.0f);

    sse.Shutdown();
}

TEST(SSE_EnableDisable)
{
    ScreenSpaceEffects sse;
    sse.Initialize();

    sse.SetSSAOEnabled(false);
    EXPECT_FALSE(sse.IsSSAOEnabled());

    sse.SetSSREnabled(true);
    EXPECT_TRUE(sse.IsSSREnabled());

    sse.SetContactShadowsEnabled(true);
    EXPECT_TRUE(sse.IsContactShadowsEnabled());

    sse.Shutdown();
}

TEST(SSE_SSAORadiusClamp)
{
    ScreenSpaceEffects sse;
    sse.Initialize();

    sse.SetSSAORadius(2.0f);
    EXPECT_NEAR(sse.GetSSAOSettings().radius, 2.0f, 0.001f);

    sse.SetSSAORadius(-1.0f);
    EXPECT_GT(sse.GetSSAOSettings().radius, 0.0f); // Should clamp

    sse.Shutdown();
}

TEST(SSE_QualityChange)
{
    ScreenSpaceEffects sse;
    sse.Initialize();

    sse.SetSSAOQuality(SSAOQuality::Ultra);
    EXPECT_EQ(sse.GetSSAOSettings().kernelSize, 128);

    const auto& kernel = sse.GetSSAOKernel();
    EXPECT_EQ((int)kernel.size(), 128);

    sse.Shutdown();
}

TEST(SSE_Metrics)
{
    ScreenSpaceEffects sse;
    sse.Initialize();

    sse.SetSSAOEnabled(true);
    sse.SetSSREnabled(true);
    sse.SetContactShadowsEnabled(false);

    auto m = sse.GetMetrics();
    EXPECT_TRUE(m.ssaoActive);
    EXPECT_TRUE(m.ssrActive);
    EXPECT_FALSE(m.contactShadowsActive);
    EXPECT_EQ(m.totalEffects, 2);
    EXPECT_GT(m.ssaoSamples, 0);
    EXPECT_GT(m.ssrSteps, 0);

    sse.Shutdown();
}

TEST(SSE_ConsoleStatus)
{
    ScreenSpaceEffects sse;
    sse.Initialize();

    std::string status = sse.Console_GetStatus();
    EXPECT_TRUE(status.find("Screen-Space Effects") != std::string::npos);
    EXPECT_TRUE(status.find("SSAO") != std::string::npos);
    EXPECT_TRUE(status.find("SSR") != std::string::npos);

    sse.Shutdown();
}

TEST(SSE_SamplePointNormalize)
{
    SamplePoint p = {3.0f, 4.0f, 0.0f};
    auto n = p.Normalized();
    EXPECT_NEAR(n.Length(), 1.0f, 0.001f);
    EXPECT_NEAR(n.x, 0.6f, 0.001f);
    EXPECT_NEAR(n.y, 0.8f, 0.001f);
}

TEST(SSE_Resize)
{
    ScreenSpaceEffects sse;
    sse.Initialize(1920, 1080);
    sse.Resize(1280, 720);
    // Should not crash, kernel should still be valid
    EXPECT_GT((int)sse.GetSSAOKernel().size(), 0);
    sse.Shutdown();
}
