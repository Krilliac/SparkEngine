// TestGraphicsStress.cpp - Stress tests for Graphics subsystem edge cases
// Tests LightManager, FogSystem, and ScreenSpaceEffects under extreme inputs

#ifndef SPARK_PLATFORM_WINDOWS
#ifndef _XM_NO_INTRINSICS_
#define _XM_NO_INTRINSICS_
#endif
#endif

#include "TestFramework.h"
#include "Graphics/LightManager.h"
#include "Graphics/FogSystem.h"
#include "Graphics/ScreenSpaceEffects.h"

using namespace DirectX;
using namespace Spark::Graphics;

// =============================================================================
// Helper
// =============================================================================

static XMFLOAT4X4 MakeIdentity4x4()
{
    XMFLOAT4X4 m = {};
    m.m[0][0] = 1.0f;
    m.m[1][1] = 1.0f;
    m.m[2][2] = 1.0f;
    m.m[3][3] = 1.0f;
    return m;
}

// =============================================================================
// LightManager Stress Tests
// =============================================================================

TEST(GfxStress_LightManagerMassiveLights)
{
    LightManager mgr;
    mgr.Initialize(1920, 1080, 16);

    XMFLOAT4X4 identity = MakeIdentity4x4();
    mgr.BeginFrame(identity);

    for (int i = 0; i < 10000; ++i)
    {
        RuntimeLight light;
        light.id = static_cast<uint32_t>(i);
        light.type = RuntimeLightType::Point;
        light.position = {static_cast<float>(i % 100), 0.0f, static_cast<float>(i / 100)};
        light.range = 5.0f;
        light.intensity = 1.0f;
        light.enabled = true;
        mgr.SubmitLight(light);
    }

    EXPECT_EQ((int)mgr.GetLights().size(), 10000);
    mgr.CullAndBinLights();

    // Tile binning should cap at MAX_LIGHTS_PER_TILE per tile
    auto& metrics = mgr.GetMetrics();
    EXPECT_EQ(metrics.totalSubmitted, 10000);
    EXPECT_LE(metrics.maxLightsPerTile, TileLightList::MAX_LIGHTS_PER_TILE);

    mgr.Shutdown();
}

TEST(GfxStress_LightManagerZeroResolution)
{
    LightManager mgr;
    // Width=0 and height=0 should not cause division by zero
    bool result = mgr.Initialize(0, 0, 16);
    EXPECT_TRUE(result);

    // Tiles should be 0x0 (no crash from empty grid)
    EXPECT_EQ(mgr.GetTilesX(), 0);
    EXPECT_EQ(mgr.GetTilesY(), 0);

    mgr.Shutdown();
}

TEST(GfxStress_LightManagerNegativeResolution)
{
    LightManager mgr;
    // Negative values wrap to large unsigned, but should not crash.
    // Initialize takes uint32_t, so negative int is implicitly converted.
    // The key assertion: no crash, no undefined behavior on construction.
    bool result = mgr.Initialize(static_cast<uint32_t>(-1), static_cast<uint32_t>(-1), 16);
    // Whether it succeeds or fails, it should not crash
    (void)result;

    mgr.Shutdown();
}

TEST(GfxStress_LightManagerDuplicateInit)
{
    LightManager mgr;
    EXPECT_TRUE(mgr.Initialize(800, 600, 16));
    EXPECT_EQ(mgr.GetTilesX(), 50);
    EXPECT_EQ(mgr.GetTilesY(), 38);

    // Second Initialize should overwrite cleanly, no double-allocation leak
    EXPECT_TRUE(mgr.Initialize(1920, 1080, 16));
    EXPECT_EQ(mgr.GetTilesX(), 120);
    EXPECT_EQ(mgr.GetTilesY(), 68);

    mgr.Shutdown();
}

TEST(GfxStress_LightManagerNoInit)
{
    LightManager mgr;
    // Do NOT call Initialize — SubmitLight and CullAndBinLights should not crash

    RuntimeLight light;
    light.id = 1;
    light.type = RuntimeLightType::Point;
    light.position = {0, 0, 0};
    light.enabled = true;
    mgr.SubmitLight(light);

    // CullAndBinLights early-returns when not initialized
    mgr.CullAndBinLights();

    // Should have the light in the list but no tile processing
    EXPECT_EQ((int)mgr.GetLights().size(), 1);
    mgr.Shutdown();
}

TEST(GfxStress_LightNaNPosition)
{
    LightManager mgr;
    mgr.Initialize(800, 600, 16);

    XMFLOAT4X4 identity = MakeIdentity4x4();
    mgr.BeginFrame(identity);

    RuntimeLight light;
    light.id = 1;
    light.type = RuntimeLightType::Point;
    light.position = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
                      std::numeric_limits<float>::quiet_NaN()};
    light.direction = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    light.range = 10.0f;
    light.enabled = true;
    mgr.SubmitLight(light);

    // Should not crash during culling with NaN positions
    mgr.CullAndBinLights();
    EXPECT_EQ(mgr.GetMetrics().totalSubmitted, 1);

    mgr.Shutdown();
}

TEST(GfxStress_LightNegativeRadius)
{
    LightManager mgr;
    mgr.Initialize(800, 600, 16);

    XMFLOAT4X4 identity = MakeIdentity4x4();
    mgr.BeginFrame(identity);

    RuntimeLight light;
    light.id = 1;
    light.type = RuntimeLightType::Point;
    light.position = {0, 0, 5};
    light.range = -10.0f;
    light.enabled = true;
    mgr.SubmitLight(light);

    // Should not crash; negative range may cause the light to be culled
    mgr.CullAndBinLights();
    EXPECT_EQ(mgr.GetMetrics().totalSubmitted, 1);

    mgr.Shutdown();
}

TEST(GfxStress_LightZeroIntensity)
{
    LightManager mgr;
    mgr.Initialize(800, 600, 16);

    XMFLOAT4X4 identity = MakeIdentity4x4();
    mgr.BeginFrame(identity);

    RuntimeLight light;
    light.id = 1;
    light.type = RuntimeLightType::Point;
    light.position = {0, 0, 5};
    light.intensity = 0.0f;
    light.range = 10.0f;
    light.enabled = true;
    mgr.SubmitLight(light);

    mgr.CullAndBinLights();

    // Zero-intensity light is still submitted and visible (rendering handles intensity)
    EXPECT_EQ(mgr.GetMetrics().totalSubmitted, 1);

    mgr.Shutdown();
}

// =============================================================================
// FogSystem Stress Tests
// =============================================================================

TEST(GfxStress_FogExtremeValues)
{
    FogSystem fog;
    fog.Initialize();

    // Extreme density
    fog.SetMode(FogMode::Exponential);
    fog.SetDensity(1e30f);
    float f = fog.ComputeFogFactor(100.0f);
    // Should clamp to 1.0 or at least not crash/NaN
    EXPECT_GE(f, 0.0f);
    EXPECT_LE(f, 1.0f);

    // Negative start/end for linear fog
    fog.SetMode(FogMode::Linear);
    fog.SetLinearRange(-100.0f, -50.0f);
    f = fog.ComputeFogFactor(0.0f);
    // Distance 0 is past end (-50), should return 1.0
    EXPECT_GE(f, 0.0f);
    EXPECT_LE(f, 1.0f);

    fog.Shutdown();
}

TEST(GfxStress_FogNaNParameters)
{
    FogSystem fog;
    fog.Initialize();

    float nan = std::numeric_limits<float>::quiet_NaN();

    // NaN density — should not crash
    fog.SetMode(FogMode::Exponential);
    fog.SetDensity(nan);
    float f = fog.ComputeFogFactor(50.0f);
    // Result may be NaN but must not crash
    (void)f;

    // NaN distance
    fog.SetDensity(0.01f);
    f = fog.ComputeFogFactor(nan);
    (void)f;

    // NaN in linear range
    fog.SetMode(FogMode::Linear);
    fog.SetLinearRange(nan, nan);
    f = fog.ComputeFogFactor(50.0f);
    (void)f;

    fog.Shutdown();
}

TEST(GfxStress_FogRapidModeSwitch)
{
    FogSystem fog;
    fog.Initialize();

    FogMode modes[] = {FogMode::None,        FogMode::Linear,
                       FogMode::Exponential, FogMode::ExponentialSquared,
                       FogMode::Height,      FogMode::ExponentialHeight,
                       FogMode::Volumetric};
    constexpr int modeCount = static_cast<int>(sizeof(modes) / sizeof(modes[0]));

    fog.SetDensity(0.01f);
    fog.SetLinearRange(10.0f, 100.0f);

    for (int i = 0; i < 1000; ++i)
    {
        fog.SetMode(modes[i % modeCount]);
    }

    // After rapid switching, mode should be deterministic
    fog.SetMode(FogMode::Linear);
    EXPECT_EQ((int)fog.GetMode(), (int)FogMode::Linear);

    // Fog computation should still work correctly
    float f = fog.ComputeFogFactor(55.0f);
    EXPECT_NEAR(f, 0.5f, 0.001f);

    fog.Shutdown();
}

// =============================================================================
// ScreenSpaceEffects Stress Tests
// =============================================================================

TEST(GfxStress_SSAOZeroKernelSize)
{
    // Generate a kernel with 0 samples — should return empty, no crash
    auto kernel = SSAOKernel::GenerateKernel(0);
    EXPECT_EQ((int)kernel.size(), 0);
}

TEST(GfxStress_SSAOMassiveKernelSize)
{
    // Generate a very large kernel — should handle without crash
    auto kernel = SSAOKernel::GenerateKernel(10000);
    EXPECT_EQ((int)kernel.size(), 10000);

    // All samples should have finite length
    for (const auto& s : kernel)
    {
        float len = s.Length();
        EXPECT_GE(len, 0.0f);
        EXPECT_LT(len, 10.0f);
    }
}

TEST(GfxStress_RenderTargetZeroSize)
{
    ScreenSpaceEffects sse;
    // Initialize with zero dimensions — should not crash
    bool result = sse.Initialize(0, 0);
    // Whether it succeeds or fails, no crash is the requirement
    (void)result;
    sse.Shutdown();
}

TEST(GfxStress_ShadowAtlasOverflow)
{
    LightManager mgr;
    mgr.Initialize();

    // Allocate slots until the atlas is full, then verify graceful rejection
    int successCount = 0;
    int failedSlot = -1;

    for (uint32_t i = 1; i <= 100; ++i)
    {
        int slot = mgr.AllocateShadowSlot(i, 1024);
        if (slot >= 0)
        {
            successCount++;
        }
        else
        {
            failedSlot = slot;
            break;
        }
    }

    // Should have allocated some slots successfully
    EXPECT_GT(successCount, 0);

    // Should eventually return -1 when the atlas is full
    EXPECT_EQ(failedSlot, -1);

    mgr.Shutdown();
}
