/**
 * @file TestGraphicsEngine.cpp
 * @brief Unit tests for GraphicsEngine state management, draw list, and statistics
 *
 * Tests configuration, quality presets, draw list operations, and statistics
 * without requiring a GPU or D3D11 device.
 */

#include "TestFramework.h"
#include "Graphics/GraphicsEngineTypes.h"

// ============================================================================
// GraphicsSettings Defaults
// ============================================================================

TEST(GraphicsSettings_DefaultValues)
{
    GraphicsSettings settings;

    EXPECT_TRUE(settings.renderPath == RenderPath::Deferred);
    EXPECT_TRUE(settings.qualityPreset == QualityPreset::High);
    EXPECT_TRUE(settings.vsync);
    EXPECT_TRUE(settings.hdr);
    EXPECT_EQ(settings.msaaSamples, uint32_t(4));
    EXPECT_EQ(settings.maxTextureSize, uint32_t(2048));
    EXPECT_TRUE(settings.anisotropicFiltering);
    EXPECT_EQ(settings.anisotropyLevel, uint32_t(16));
    EXPECT_TRUE(settings.shadows);
    EXPECT_EQ(settings.shadowMapSize, uint32_t(2048));
    EXPECT_EQ(settings.cascadeCount, uint32_t(3));
    EXPECT_TRUE(settings.bloom);
    EXPECT_FALSE(settings.ssao);
    EXPECT_FALSE(settings.taa);
    EXPECT_FALSE(settings.motionBlur);
    EXPECT_TRUE(settings.frustumCulling);
    EXPECT_FALSE(settings.occlusionCulling);
    EXPECT_TRUE(settings.levelOfDetail);
    EXPECT_EQ(settings.maxDrawCalls, uint32_t(1000));
    EXPECT_FALSE(settings.wireframeMode);
    EXPECT_FALSE(settings.debugMode);
    EXPECT_FALSE(settings.showFPS);
    EXPECT_NEAR(settings.renderScale, 1.0f, 0.001f);
    EXPECT_FALSE(settings.enableGPUTiming);
}

TEST(GraphicsSettings_CustomValues)
{
    GraphicsSettings settings;
    settings.renderPath = RenderPath::Forward;
    settings.qualityPreset = QualityPreset::Low;
    settings.vsync = false;
    settings.hdr = false;
    settings.msaaSamples = 1;
    settings.shadowMapSize = 512;
    settings.bloom = false;
    settings.renderScale = 0.5f;

    EXPECT_TRUE(settings.renderPath == RenderPath::Forward);
    EXPECT_TRUE(settings.qualityPreset == QualityPreset::Low);
    EXPECT_FALSE(settings.vsync);
    EXPECT_FALSE(settings.hdr);
    EXPECT_EQ(settings.msaaSamples, uint32_t(1));
    EXPECT_EQ(settings.shadowMapSize, uint32_t(512));
    EXPECT_FALSE(settings.bloom);
    EXPECT_NEAR(settings.renderScale, 0.5f, 0.001f);
}

// ============================================================================
// RenderStatistics Defaults
// ============================================================================

TEST(RenderStatistics_DefaultZero)
{
    RenderStatistics stats;

    EXPECT_NEAR(stats.frameTime, 0.0f, 0.001f);
    EXPECT_NEAR(stats.cpuTime, 0.0f, 0.001f);
    EXPECT_NEAR(stats.gpuTime, 0.0f, 0.001f);
    EXPECT_EQ(stats.fps, uint32_t(0));
    EXPECT_EQ(stats.drawCalls, uint32_t(0));
    EXPECT_EQ(stats.triangles, uint32_t(0));
    EXPECT_EQ(stats.vertices, uint32_t(0));
    EXPECT_EQ(stats.textureBinds, uint32_t(0));
    EXPECT_EQ(stats.materialSwitches, uint32_t(0));
    EXPECT_EQ(stats.totalObjects, uint32_t(0));
    EXPECT_EQ(stats.visibleObjects, uint32_t(0));
    EXPECT_EQ(stats.culledObjects, uint32_t(0));
    EXPECT_EQ(stats.textureMemory, size_t(0));
    EXPECT_EQ(stats.meshMemory, size_t(0));
    EXPECT_EQ(stats.totalGPUMemory, size_t(0));
    EXPECT_EQ(stats.activeLights, uint32_t(0));
    EXPECT_EQ(stats.shadowUpdates, uint32_t(0));
}

TEST(RenderStatistics_CanSetFields)
{
    RenderStatistics stats;
    stats.drawCalls = 150;
    stats.triangles = 250000;
    stats.fps = 60;
    stats.frameTime = 16.67f;
    stats.totalObjects = 500;
    stats.visibleObjects = 200;
    stats.culledObjects = 300;

    EXPECT_EQ(stats.drawCalls, uint32_t(150));
    EXPECT_EQ(stats.triangles, uint32_t(250000));
    EXPECT_EQ(stats.fps, uint32_t(60));
    EXPECT_NEAR(stats.frameTime, 16.67f, 0.01f);
    EXPECT_EQ(stats.totalObjects, uint32_t(500));
    EXPECT_EQ(stats.visibleObjects, uint32_t(200));
    EXPECT_EQ(stats.culledObjects, uint32_t(300));
}

// ============================================================================
// SSAOSettings Defaults and Ranges
// ============================================================================

TEST(SSAOSettings_Defaults)
{
    SSAOSettings ssao;
    EXPECT_FALSE(ssao.enabled);
    EXPECT_NEAR(ssao.radius, 0.5f, 0.001f);
    EXPECT_NEAR(ssao.intensity, 1.0f, 0.001f);
    EXPECT_EQ(ssao.sampleCount, 16);
    EXPECT_NEAR(ssao.bias, 0.025f, 0.001f);
    EXPECT_TRUE(ssao.blur);
}

// ============================================================================
// SSRSettings Defaults
// ============================================================================

TEST(SSRSettings_Defaults)
{
    SSRSettings ssr;
    EXPECT_FALSE(ssr.enabled);
    EXPECT_NEAR(ssr.maxDistance, 100.0f, 0.01f);
    EXPECT_EQ(ssr.maxSteps, 32);
    EXPECT_NEAR(ssr.thickness, 0.5f, 0.001f);
    EXPECT_NEAR(ssr.fadeStart, 80.0f, 0.01f);
    EXPECT_NEAR(ssr.fadeEnd, 100.0f, 0.01f);
}

// ============================================================================
// VolumetricSettings Defaults
// ============================================================================

TEST(VolumetricSettings_Defaults)
{
    VolumetricSettings vol;
    EXPECT_FALSE(vol.enabled);
    EXPECT_EQ(vol.sampleCount, 32);
    EXPECT_NEAR(vol.scattering, 0.1f, 0.001f);
    EXPECT_NEAR(vol.extinction, 0.01f, 0.001f);
    EXPECT_NEAR(vol.anisotropy, 0.3f, 0.001f);
}

// ============================================================================
// QualityPreset Enum Values
// ============================================================================

TEST(QualityPreset_EnumValues)
{
    // Ensure distinct enum values
    EXPECT_NE(static_cast<int>(QualityPreset::Low), static_cast<int>(QualityPreset::Medium));
    EXPECT_NE(static_cast<int>(QualityPreset::Medium), static_cast<int>(QualityPreset::High));
    EXPECT_NE(static_cast<int>(QualityPreset::High), static_cast<int>(QualityPreset::Ultra));
    EXPECT_NE(static_cast<int>(QualityPreset::Ultra), static_cast<int>(QualityPreset::Custom));
}

TEST(MSAALevel_EnumValues)
{
    EXPECT_EQ(static_cast<int>(MSAALevel::None), 1);
    EXPECT_EQ(static_cast<int>(MSAALevel::MSAA2x), 2);
    EXPECT_EQ(static_cast<int>(MSAALevel::MSAA4x), 4);
    EXPECT_EQ(static_cast<int>(MSAALevel::MSAA8x), 8);
}

// ============================================================================
// GraphicsSettings Copy Semantics
// ============================================================================

TEST(GraphicsSettings_CopySemantic)
{
    GraphicsSettings original;
    original.renderPath = RenderPath::ForwardPlus;
    original.qualityPreset = QualityPreset::Ultra;
    original.vsync = false;
    original.shadowMapSize = 4096;
    original.bloom = false;

    GraphicsSettings copy = original;
    EXPECT_TRUE(copy.renderPath == RenderPath::ForwardPlus);
    EXPECT_TRUE(copy.qualityPreset == QualityPreset::Ultra);
    EXPECT_FALSE(copy.vsync);
    EXPECT_EQ(copy.shadowMapSize, uint32_t(4096));
    EXPECT_FALSE(copy.bloom);

    // Modify copy — original unchanged
    copy.vsync = true;
    EXPECT_FALSE(original.vsync);
    EXPECT_TRUE(copy.vsync);
}
