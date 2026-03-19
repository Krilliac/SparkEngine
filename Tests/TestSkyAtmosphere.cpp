// TestSkyAtmosphere.cpp - Tests for sky atmosphere initialization, sun direction, and color computation
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <algorithm>
#include <cmath>

namespace TestSkyAtmo
{

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        float Length() const { return std::sqrt(x * x + y * y + z * z); }

        Vec3 Normalized() const
        {
            float len = Length();
            if (len < 0.0001f)
                return {0, 1, 0};
            return {x / len, y / len, z / len};
        }

        float Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    };

    struct Color3
    {
        float r = 0.0f, g = 0.0f, b = 0.0f;
    };

    struct SkyAtmosphere
    {
        Vec3 sunDirection = {0, 1, 0};
        float rayleighScale = 1.0f;
        float mieScale = 0.005f;
        float sunIntensity = 22.0f;
        bool initialized = false;

        // Precomputed colors
        Color3 zenithColor;
        Color3 horizonColor;

        bool Initialize()
        {
            initialized = true;
            ComputeColors();
            return true;
        }

        void Shutdown() { initialized = false; }

        void SetSunDirection(Vec3 dir)
        {
            sunDirection = dir.Normalized();
            if (initialized)
                ComputeColors();
        }

        Color3 ComputeSkyColor(Vec3 viewDir) const
        {
            if (!initialized)
                return {0, 0, 0};

            float sunDot = std::max(0.0f, viewDir.Dot(sunDirection));
            float zenithFactor = std::max(0.0f, viewDir.y);

            // Simplified Rayleigh: blue at zenith, orange at horizon
            float r = horizonColor.r * (1.0f - zenithFactor) + zenithColor.r * zenithFactor;
            float g = horizonColor.g * (1.0f - zenithFactor) + zenithColor.g * zenithFactor;
            float b = horizonColor.b * (1.0f - zenithFactor) + zenithColor.b * zenithFactor;

            // Sun glow
            float sunGlow = std::pow(sunDot, 64.0f) * sunIntensity * 0.01f;
            r += sunGlow;
            g += sunGlow;
            b += sunGlow;

            return {std::min(r, 1.0f), std::min(g, 1.0f), std::min(b, 1.0f)};
        }

        Color3 GetZenithColor() const { return zenithColor; }
        Color3 GetHorizonColor() const { return horizonColor; }

      private:
        void ComputeColors()
        {
            float sunHeight = std::max(0.0f, sunDirection.y);
            // Zenith: deep blue when sun is high, dark when sun is low
            zenithColor = {0.1f * sunHeight, 0.2f * sunHeight, 0.8f * sunHeight};
            // Horizon: warm orange at sunset, lighter blue at noon
            float sunset = 1.0f - sunHeight;
            horizonColor = {0.8f * sunset + 0.4f * sunHeight, 0.3f * sunset + 0.5f * sunHeight,
                            0.1f * sunset + 0.7f * sunHeight};
        }
    };

} // namespace TestSkyAtmo

// ============================================================================
// Initialization
// ============================================================================

TEST(SkyAtmosphere_InitializeAndShutdown)
{
    TestSkyAtmo::SkyAtmosphere sky;
    EXPECT_FALSE(sky.initialized);
    EXPECT_TRUE(sky.Initialize());
    EXPECT_TRUE(sky.initialized);
    sky.Shutdown();
    EXPECT_FALSE(sky.initialized);
}

// ============================================================================
// Sun Direction
// ============================================================================

TEST(SkyAtmosphere_SetSunDirection)
{
    TestSkyAtmo::SkyAtmosphere sky;
    sky.Initialize();

    sky.SetSunDirection({0, 1, 0}); // Noon
    auto zenith = sky.GetZenithColor();
    EXPECT_GT(zenith.b, 0.5f); // Blue sky at noon

    sky.SetSunDirection({1, 0, 0}); // Sunset
    auto horizon = sky.GetHorizonColor();
    EXPECT_GT(horizon.r, 0.5f); // Orange horizon at sunset

    sky.Shutdown();
}

// ============================================================================
// Sky Color Computation
// ============================================================================

TEST(SkyAtmosphere_ComputeSkyColor)
{
    TestSkyAtmo::SkyAtmosphere sky;
    sky.Initialize();
    sky.SetSunDirection({0, 1, 0});

    // Looking straight up should be close to zenith color
    auto upColor = sky.ComputeSkyColor({0, 1, 0});
    auto zenith = sky.GetZenithColor();
    EXPECT_NEAR(upColor.b, zenith.b, 0.25f);

    // Looking at horizon should be close to horizon color
    auto horizonViewColor = sky.ComputeSkyColor({1, 0, 0});
    auto horizonColor = sky.GetHorizonColor();
    EXPECT_NEAR(horizonViewColor.r, horizonColor.r, 0.15f);

    sky.Shutdown();
}

TEST(SkyAtmosphere_ComputeSkyColor_NotInitialized)
{
    TestSkyAtmo::SkyAtmosphere sky;
    auto color = sky.ComputeSkyColor({0, 1, 0});
    EXPECT_NEAR(color.r, 0.0f, 0.001f);
    EXPECT_NEAR(color.g, 0.0f, 0.001f);
    EXPECT_NEAR(color.b, 0.0f, 0.001f);
}

TEST(SkyAtmosphere_GetZenithAndHorizonColor)
{
    TestSkyAtmo::SkyAtmosphere sky;
    sky.Initialize();
    sky.SetSunDirection({0, 1, 0});

    auto zenith = sky.GetZenithColor();
    auto horizon = sky.GetHorizonColor();

    // At noon, zenith should be bluer than horizon
    EXPECT_GT(zenith.b, horizon.r);

    sky.Shutdown();
}
