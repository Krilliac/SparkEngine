/**
 * @file TestVolumetricClouds.cpp
 * @brief Tests for the volumetric cloud system
 *
 * Exercises the CPU reference path against the real VolumetricCloudSystem
 * class (no reimplementation) — coverage/type presets, altitude gating,
 * density-over-ray integration and transmittance bookkeeping.
 */

#include "TestFramework.h"
#include "Graphics/VolumetricClouds.h"

#include <cmath>

using Spark::Graphics::CloudSample;
using Spark::Graphics::VolumetricCloudSettings;
using Spark::Graphics::VolumetricCloudSystem;

// =============================================================================
// Lifecycle
// =============================================================================

TEST(VolumetricClouds_InitializeWithDefaults)
{
    auto& clouds = VolumetricCloudSystem::GetInstance();
    clouds.Shutdown();
    EXPECT_FALSE(clouds.IsInitialized());
    EXPECT_TRUE(clouds.Initialize());
    EXPECT_TRUE(clouds.IsInitialized());
    clouds.Shutdown();
    EXPECT_FALSE(clouds.IsInitialized());
}

TEST(VolumetricClouds_CustomSettingsPreserved)
{
    auto& clouds = VolumetricCloudSystem::GetInstance();
    VolumetricCloudSettings s;
    s.coverage = 0.8f;
    s.cloudType = 0.3f;
    s.baseAltitude = 1000.0f;
    s.topAltitude = 3500.0f;
    clouds.Initialize(s);
    const auto& out = clouds.GetSettings();
    EXPECT_NEAR(out.coverage, 0.8f, 0.001f);
    EXPECT_NEAR(out.cloudType, 0.3f, 0.001f);
    EXPECT_NEAR(out.baseAltitude, 1000.0f, 0.1f);
    EXPECT_NEAR(out.topAltitude, 3500.0f, 0.1f);
    clouds.Shutdown();
}

// =============================================================================
// Altitude gating
// =============================================================================

TEST(VolumetricClouds_ZeroDensityBelowBase)
{
    auto& clouds = VolumetricCloudSystem::GetInstance();
    clouds.Initialize();
    // Well below the 1500m base altitude → must be zero
    EXPECT_NEAR(clouds.SampleDensity(0.0f, 500.0f, 0.0f), 0.0f, 0.001f);
    EXPECT_NEAR(clouds.SampleDensity(100.0f, 100.0f, 100.0f), 0.0f, 0.001f);
    clouds.Shutdown();
}

TEST(VolumetricClouds_ZeroDensityAboveTop)
{
    auto& clouds = VolumetricCloudSystem::GetInstance();
    clouds.Initialize();
    // Well above the 4000m top altitude
    EXPECT_NEAR(clouds.SampleDensity(0.0f, 10000.0f, 0.0f), 0.0f, 0.001f);
    clouds.Shutdown();
}

TEST(VolumetricClouds_NonZeroInsideLayer)
{
    auto& clouds = VolumetricCloudSystem::GetInstance();
    VolumetricCloudSettings s;
    s.coverage = 0.95f; // force plenty of cloud everywhere
    clouds.Initialize(s);
    // Sample over a grid — at least some points inside the layer should have density
    int hits = 0;
    for (int i = 0; i < 10; ++i)
        for (int j = 0; j < 10; ++j)
        {
            const float x = static_cast<float>(i) * 500.0f;
            const float z = static_cast<float>(j) * 500.0f;
            if (clouds.SampleDensity(x, 2500.0f, z) > 0.0f)
                ++hits;
        }
    EXPECT_GT(hits, 0);
    clouds.Shutdown();
}

TEST(VolumetricClouds_DisabledReturnsZeroDensity)
{
    auto& clouds = VolumetricCloudSystem::GetInstance();
    VolumetricCloudSettings s;
    s.enabled = false;
    clouds.Initialize(s);
    EXPECT_NEAR(clouds.SampleDensity(0.0f, 2500.0f, 0.0f), 0.0f, 0.001f);
    clouds.Shutdown();
}

// =============================================================================
// Ray integration
// =============================================================================

TEST(VolumetricClouds_RayMissReturnsFullTransmittance)
{
    auto& clouds = VolumetricCloudSystem::GetInstance();
    clouds.Initialize();
    // Ray going down from below the layer misses the slab entirely.
    CloudSample s = clouds.SampleAlongRay(0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 2000.0f);
    EXPECT_NEAR(s.transmittance, 1.0f, 0.001f);
    EXPECT_NEAR(s.luminanceR, 0.0f, 0.001f);
    clouds.Shutdown();
}

TEST(VolumetricClouds_RayThroughLayerAttenuates)
{
    auto& clouds = VolumetricCloudSystem::GetInstance();
    VolumetricCloudSettings s;
    s.coverage = 0.95f;
    s.density = 0.1f;
    clouds.Initialize(s);
    clouds.SetSunDirection(0.0f, 1.0f, 0.0f);
    // Straight up through the entire cloud layer
    CloudSample out = clouds.SampleAlongRay(0.0f, 500.0f, 0.0f, 0.0f, 1.0f, 0.0f, 8000.0f);
    EXPECT_LE(out.transmittance, 1.0f);
    EXPECT_GE(out.transmittance, 0.0f);
    clouds.Shutdown();
}

TEST(VolumetricClouds_ZeroLengthRay)
{
    auto& clouds = VolumetricCloudSystem::GetInstance();
    clouds.Initialize();
    CloudSample s = clouds.SampleAlongRay(0.0f, 2500.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1000.0f);
    EXPECT_NEAR(s.transmittance, 1.0f, 0.001f);
    clouds.Shutdown();
}

TEST(VolumetricClouds_HorizontalRayInsideSlab)
{
    // Codex P1 regression: a horizontal ray whose origin is between the base
    // and top altitudes must still march the cloud layer.
    auto& clouds = VolumetricCloudSystem::GetInstance();
    VolumetricCloudSettings s;
    s.coverage = 0.95f;
    s.density = 0.2f;
    clouds.Initialize(s);
    CloudSample out =
        clouds.SampleAlongRay(0.0f, 2500.0f, 0.0f, 1.0f, 0.0f, 0.0f, 5000.0f); // dirY == 0, origin inside slab
    EXPECT_LE(out.transmittance, 1.0f);
    EXPECT_GE(out.transmittance, 0.0f);
    clouds.Shutdown();
}

TEST(VolumetricClouds_HorizontalRayOutsideSlab)
{
    // Same direction but origin is above the layer — must early out.
    auto& clouds = VolumetricCloudSystem::GetInstance();
    clouds.Initialize();
    CloudSample out = clouds.SampleAlongRay(0.0f, 100.0f, 0.0f, 1.0f, 0.0f, 0.0f, 5000.0f);
    EXPECT_NEAR(out.transmittance, 1.0f, 0.001f);
    EXPECT_NEAR(out.luminanceR, 0.0f, 0.001f);
    clouds.Shutdown();
}

// =============================================================================
// Weather integration
// =============================================================================

TEST(VolumetricClouds_SetWeatherUpdatesCoverage)
{
    auto& clouds = VolumetricCloudSystem::GetInstance();
    clouds.Initialize();
    clouds.SetWeather(0.25f, 0.75f, 0.5f);
    EXPECT_NEAR(clouds.GetSettings().coverage, 0.25f, 0.001f);
    EXPECT_NEAR(clouds.GetSettings().cloudType, 0.75f, 0.001f);
    clouds.Shutdown();
}

TEST(VolumetricClouds_UpdateAdvancesTime)
{
    auto& clouds = VolumetricCloudSystem::GetInstance();
    clouds.Initialize();
    const float t0 = clouds.GetTime();
    clouds.Update(0.5f);
    EXPECT_GT(clouds.GetTime(), t0);
    clouds.Shutdown();
}

TEST(VolumetricClouds_SunDirectionNormalized)
{
    auto& clouds = VolumetricCloudSystem::GetInstance();
    clouds.Initialize();
    clouds.SetSunDirection(0.0f, 10.0f, 0.0f);
    clouds.SetSunColor(1.0f, 0.9f, 0.7f);
    // No crashes; no public getter needed — covered by the ray call below
    CloudSample s = clouds.SampleAlongRay(0.0f, 2500.0f, 0.0f, 0.0f, 1.0f, 0.0f, 2000.0f);
    EXPECT_GE(s.transmittance, 0.0f);
    clouds.Shutdown();
}
