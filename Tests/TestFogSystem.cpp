// TestFogSystem.cpp - Tests for distance fog, height fog, and volumetric fog
// Uses the actual FogSystem.h header

#include "TestFramework.h"
#include "Graphics/FogSystem.h"

using namespace Spark::Graphics;

// =============================================================================
// Tests
// =============================================================================

TEST(Fog_LinearFogFactor)
{
    EXPECT_NEAR(FogMath::LinearFog(0.0f, 10.0f, 100.0f), 0.0f, 0.001f);
    EXPECT_NEAR(FogMath::LinearFog(10.0f, 10.0f, 100.0f), 0.0f, 0.001f);
    EXPECT_NEAR(FogMath::LinearFog(55.0f, 10.0f, 100.0f), 0.5f, 0.001f);
    EXPECT_NEAR(FogMath::LinearFog(100.0f, 10.0f, 100.0f), 1.0f, 0.001f);
    EXPECT_NEAR(FogMath::LinearFog(200.0f, 10.0f, 100.0f), 1.0f, 0.001f);
}

TEST(Fog_ExponentialFogFactor)
{
    float f = FogMath::ExponentialFog(0.0f, 0.01f);
    EXPECT_NEAR(f, 0.0f, 0.001f);

    f = FogMath::ExponentialFog(100.0f, 0.01f);
    EXPECT_GT(f, 0.0f);
    EXPECT_LT(f, 1.0f);

    // Higher density = more fog
    float f1 = FogMath::ExponentialFog(50.0f, 0.01f);
    float f2 = FogMath::ExponentialFog(50.0f, 0.05f);
    EXPECT_LT(f1, f2);
}

TEST(Fog_ExponentialSquaredFogFactor)
{
    float f = FogMath::ExponentialSquaredFog(0.0f, 0.01f);
    EXPECT_NEAR(f, 0.0f, 0.001f);

    f = FogMath::ExponentialSquaredFog(100.0f, 0.01f);
    EXPECT_GT(f, 0.0f);
    EXPECT_LT(f, 1.0f);

    // Exp squared falls off faster than exp at large distances
    float fExp = FogMath::ExponentialFog(50.0f, 0.02f);
    float fSq = FogMath::ExponentialSquaredFog(50.0f, 0.02f);
    // For d*density=1, exp gives ~0.632, exp-sq gives ~0.632 too,
    // but for other values they differ
    float fExp2 = FogMath::ExponentialFog(100.0f, 0.02f);
    float fSq2 = FogMath::ExponentialSquaredFog(100.0f, 0.02f);
    // At larger distances, squared should produce stronger fog
    EXPECT_GT(fSq2, fExp2);
}

TEST(Fog_HeightDensity)
{
    float d = FogMath::HeightDensity(0.0f, 0.0f, 0.5f, 0.02f);
    EXPECT_NEAR(d, 0.02f, 0.001f); // At base height = base density

    // Higher altitude = less fog
    float d1 = FogMath::HeightDensity(0.0f, 0.0f, 0.5f, 0.02f);
    float d2 = FogMath::HeightDensity(10.0f, 0.0f, 0.5f, 0.02f);
    EXPECT_GT(d1, d2);

    // Below base = base density
    float d3 = FogMath::HeightDensity(-5.0f, 0.0f, 0.5f, 0.02f);
    EXPECT_NEAR(d3, 0.02f, 0.001f);
}

TEST(Fog_IntegratedHeightFog)
{
    float f = FogMath::IntegratedHeightFog(0.0f, 0.0f, 50.0f, 0.0f, 0.5f, 0.02f);
    EXPECT_GT(f, 0.0f);
    EXPECT_LT(f, 1.0f);

    // Higher camera = less fog
    float fLow = FogMath::IntegratedHeightFog(0.0f, 0.0f, 50.0f, 0.0f, 0.5f, 0.02f);
    float fHigh = FogMath::IntegratedHeightFog(50.0f, 50.0f, 50.0f, 0.0f, 0.5f, 0.02f);
    EXPECT_GT(fLow, fHigh);
}

TEST(Fog_HenyeyGreenstein)
{
    // Forward scattering (g > 0): more light in forward direction
    float forward = FogMath::HenyeyGreenstein(1.0f, 0.5f);
    float backward = FogMath::HenyeyGreenstein(-1.0f, 0.5f);
    EXPECT_GT(forward, backward);

    // Isotropic (g = 0): uniform scattering
    float iso = FogMath::HenyeyGreenstein(0.0f, 0.0f);
    EXPECT_GT(iso, 0.0f);
}

TEST(Fog_BeerLambert)
{
    EXPECT_NEAR(FogMath::BeerLambert(0.0f), 1.0f, 0.001f);
    EXPECT_LT(FogMath::BeerLambert(1.0f), 1.0f);
    EXPECT_GT(FogMath::BeerLambert(1.0f), 0.0f);
}

TEST(Fog_SystemModes)
{
    FogSystem fog;
    fog.Initialize();

    fog.SetMode(FogMode::Linear);
    EXPECT_EQ((int)fog.GetMode(), (int)FogMode::Linear);

    fog.SetMode(FogMode::Exponential);
    EXPECT_EQ((int)fog.GetMode(), (int)FogMode::Exponential);

    fog.SetMode(FogMode::None);
    EXPECT_NEAR(fog.ComputeFogFactor(100.0f), 0.0f, 0.001f);

    fog.Shutdown();
}

TEST(Fog_LinearMode)
{
    FogSystem fog;
    fog.Initialize();
    fog.SetMode(FogMode::Linear);
    fog.SetLinearRange(10.0f, 100.0f);

    EXPECT_NEAR(fog.ComputeFogFactor(0.0f), 0.0f, 0.001f);
    EXPECT_NEAR(fog.ComputeFogFactor(55.0f), 0.5f, 0.001f);
    EXPECT_NEAR(fog.ComputeFogFactor(100.0f), 1.0f, 0.001f);

    fog.Shutdown();
}

TEST(Fog_ExponentialMode)
{
    FogSystem fog;
    fog.Initialize();
    fog.SetMode(FogMode::Exponential);
    fog.SetDensity(0.02f);

    float f = fog.ComputeFogFactor(50.0f);
    EXPECT_GT(f, 0.0f);
    EXPECT_LT(f, 1.0f);

    fog.Shutdown();
}

TEST(Fog_HeightMode)
{
    FogSystem fog;
    fog.Initialize();
    fog.SetMode(FogMode::Height);
    fog.SetDensity(0.02f);
    fog.SetBaseHeight(0.0f);
    fog.SetHeightFalloff(0.5f);

    float f = fog.ComputeHeightFogFactor(0.0f, 0.0f, 50.0f);
    EXPECT_GT(f, 0.0f);

    fog.Shutdown();
}

TEST(Fog_DisabledReturnsZero)
{
    FogSystem fog;
    fog.Initialize();
    fog.SetMode(FogMode::Linear);
    fog.SetLinearRange(0.0f, 100.0f);
    fog.SetEnabled(false);

    EXPECT_NEAR(fog.ComputeFogFactor(50.0f), 0.0f, 0.001f);

    fog.Shutdown();
}

TEST(Fog_ApplyFog)
{
    FogSystem fog;
    fog.Initialize();
    fog.SetColor({0.5f, 0.5f, 0.5f, 1.0f});

    FogColor scene = {1.0f, 1.0f, 1.0f, 1.0f};
    FogColor result = fog.ApplyFog(scene, 0.5f);

    EXPECT_NEAR(result.r, 0.75f, 0.01f);
    EXPECT_NEAR(result.g, 0.75f, 0.01f);
    EXPECT_NEAR(result.b, 0.75f, 0.01f);

    fog.Shutdown();
}

TEST(Fog_ColorLerp)
{
    FogColor a = {0.0f, 0.0f, 0.0f, 1.0f};
    FogColor b = {1.0f, 1.0f, 1.0f, 1.0f};
    FogColor mid = a.Lerp(b, 0.5f);
    EXPECT_NEAR(mid.r, 0.5f, 0.001f);
    EXPECT_NEAR(mid.g, 0.5f, 0.001f);
}

TEST(Fog_ColorPresets)
{
    auto white = FogColor::White();
    EXPECT_NEAR(white.r, 1.0f, 0.001f);

    auto gray = FogColor::Gray();
    EXPECT_NEAR(gray.r, 0.5f, 0.001f);

    auto warm = FogColor::Warm();
    EXPECT_GT(warm.r, warm.b);
}

TEST(Fog_Metrics)
{
    FogSystem fog;
    fog.Initialize();
    fog.SetMode(FogMode::Linear);
    fog.SetDensity(0.05f);

    auto m = fog.GetMetrics();
    EXPECT_EQ(m.activeMode, (int)FogMode::Linear);
    EXPECT_NEAR(m.currentDensity, 0.05f, 0.001f);

    fog.Shutdown();
}

TEST(Fog_ConsoleStatus)
{
    FogSystem fog;
    fog.Initialize();
    fog.SetMode(FogMode::Exponential);
    std::string status = fog.Console_GetStatus();
    EXPECT_TRUE(status.find("Fog System") != std::string::npos);
    EXPECT_TRUE(status.find("Exponential") != std::string::npos);
    fog.Shutdown();
}
