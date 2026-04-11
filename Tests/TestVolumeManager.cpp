/**
 * @file TestVolumeManager.cpp
 * @brief Phase K — CPU reference tests for the VolumeManager orphan
 *
 * Exercises the header-only `Spark::Graphics::VolumeManager` directly —
 * no pipeline, no GPU, no editor. `VolumeManager` is a pure CPU orphan
 * documented in `stub-and-abandoned-features-2026-04-10.md` as an
 * "intentional reusable utility"; Phase K activates it by wiring it
 * into `PostProcessingPipeline::Process()`, and these tests pin its
 * blend math so the pipeline binding stays well-defined.
 */

#include "TestFramework.h"

#include "../SparkEngine/Source/Graphics/VolumeSystem.h"

#include <cmath>

using Spark::Graphics::BloomVolumeComponent;
using Spark::Graphics::ColorGradingVolumeComponent;
using Spark::Graphics::ExposureVolumeComponent;
using Spark::Graphics::FogVolumeComponent;
using Spark::Graphics::Volume;
using Spark::Graphics::VolumeManager;
using Spark::Graphics::VolumeParameterType;

TEST(VolumeManager_InitializeAndShutdown)
{
    VolumeManager mgr;
    EXPECT_FALSE(mgr.IsInitialized());
    mgr.Initialize();
    EXPECT_TRUE(mgr.IsInitialized());
    EXPECT_EQ(static_cast<int>(mgr.GetVolumeCount()), 0);
    mgr.Shutdown();
    EXPECT_FALSE(mgr.IsInitialized());
}

TEST(VolumeManager_CreateAndRemoveVolume)
{
    VolumeManager mgr;
    mgr.Initialize();
    Volume* v = mgr.CreateVolume("test");
    EXPECT_TRUE(v != nullptr);
    EXPECT_EQ(static_cast<int>(mgr.GetVolumeCount()), 1);
    EXPECT_EQ(mgr.FindVolume("test"), v);
    mgr.RemoveVolume("test");
    EXPECT_EQ(static_cast<int>(mgr.GetVolumeCount()), 0);
    EXPECT_TRUE(mgr.FindVolume("test") == nullptr);
}

TEST(VolumeManager_UpdateBeforeInitializeIsSafe)
{
    VolumeManager mgr;
    // Update() early-outs when !IsInitialized — it must not crash.
    mgr.Update({0.0f, 0.0f, 0.0f});
    EXPECT_FALSE(mgr.IsInitialized());
}

TEST(VolumeManager_GlobalVolumeAlwaysContributes)
{
    VolumeManager mgr;
    mgr.Initialize();
    Volume* global = mgr.CreateVolume("global");
    global->isGlobal = true;
    global->weight = 1.0f;
    auto* bloom = global->AddComponent<BloomVolumeComponent>();
    bloom->intensity = {3.14f, true};

    // Camera far from any bounds — a global volume must still blend.
    mgr.Update({1000.0f, 1000.0f, 1000.0f});

    const auto& stack = mgr.GetStack();
    EXPECT_NEAR(stack.bloom.intensity.value, 3.14f, 1e-4f);
}

TEST(VolumeManager_LocalVolumeInsideBoundsApplies)
{
    VolumeManager mgr;
    mgr.Initialize();
    Volume* v = mgr.CreateVolume("local");
    v->isGlobal = false;
    v->boundsMin = {-5.0f, -5.0f, -5.0f};
    v->boundsMax = {5.0f, 5.0f, 5.0f};
    v->weight = 1.0f;
    v->blendDistance = 2.0f;
    auto* bloom = v->AddComponent<BloomVolumeComponent>();
    bloom->intensity = {2.0f, true};

    // Camera at origin — fully inside the AABB.
    mgr.Update({0.0f, 0.0f, 0.0f});
    EXPECT_NEAR(mgr.GetStack().bloom.intensity.value, 2.0f, 1e-4f);
}

TEST(VolumeManager_LocalVolumeOutsideBoundsDoesNotApply)
{
    VolumeManager mgr;
    mgr.Initialize();
    Volume* v = mgr.CreateVolume("local");
    v->isGlobal = false;
    v->boundsMin = {-1.0f, -1.0f, -1.0f};
    v->boundsMax = {1.0f, 1.0f, 1.0f};
    v->weight = 1.0f;
    v->blendDistance = 0.5f; // tiny fade-in so 5 units out is fully outside
    auto* bloom = v->AddComponent<BloomVolumeComponent>();
    bloom->intensity = {42.0f, true};

    // Stack defaults: bloom.intensity defaults to 1.0f from BloomVolumeComponent.
    mgr.Update({100.0f, 100.0f, 100.0f});
    EXPECT_NEAR(mgr.GetStack().bloom.intensity.value, 1.0f, 1e-4f);
}

TEST(VolumeManager_LocalVolumeBlendDistanceSmoothsEdges)
{
    VolumeManager mgr;
    mgr.Initialize();
    Volume* v = mgr.CreateVolume("fade");
    v->isGlobal = false;
    v->boundsMin = {-1.0f, -1.0f, -1.0f};
    v->boundsMax = {1.0f, 1.0f, 1.0f};
    v->weight = 1.0f;
    v->blendDistance = 10.0f; // very wide blend zone
    auto* bloom = v->AddComponent<BloomVolumeComponent>();
    bloom->intensity = {0.0f, true};

    // Half-way through the blend distance — the smoothstep must produce
    // a value strictly between 0 and 1.
    mgr.Update({6.0f, 0.0f, 0.0f}); // 5 units outside AABB, halfway through 10-unit blend
    float val = mgr.GetStack().bloom.intensity.value;
    EXPECT_LT(val, 1.0f);
    EXPECT_GT(val, 0.0f);
}

TEST(VolumeManager_HigherPriorityOverridesLower)
{
    VolumeManager mgr;
    mgr.Initialize();
    Volume* low = mgr.CreateVolume("low");
    low->isGlobal = true;
    low->priority = 0;
    low->weight = 1.0f;
    auto* lowExp = low->AddComponent<ExposureVolumeComponent>();
    lowExp->compensationEV = {0.5f, true};

    Volume* high = mgr.CreateVolume("high");
    high->isGlobal = true;
    high->priority = 10;
    high->weight = 1.0f;
    auto* highExp = high->AddComponent<ExposureVolumeComponent>();
    highExp->compensationEV = {-2.0f, true};

    mgr.Update({0.0f, 0.0f, 0.0f});
    // Both volumes are global with weight=1, so the higher priority
    // volume's Interpolate(t=1) fully lands on top of the lower one's.
    EXPECT_NEAR(mgr.GetStack().exposure.compensationEV.value, -2.0f, 1e-4f);
}

TEST(VolumeManager_NonOverriddenFieldsPassThrough)
{
    VolumeManager mgr;
    mgr.Initialize();
    Volume* v = mgr.CreateVolume("partial");
    v->isGlobal = true;
    v->weight = 1.0f;
    auto* color = v->AddComponent<ColorGradingVolumeComponent>();
    // Only override saturation — everything else must keep its default.
    color->saturation = {0.25f, true};

    mgr.Update({0.0f, 0.0f, 0.0f});
    const auto& stack = mgr.GetStack();
    EXPECT_NEAR(stack.colorGrading.saturation.value, 0.25f, 1e-4f);
    // Contrast was never flagged as an override — stay at the default (1.0).
    EXPECT_NEAR(stack.colorGrading.contrast.value, 1.0f, 1e-4f);
}

TEST(VolumeManager_FogComponentBlends)
{
    VolumeManager mgr;
    mgr.Initialize();
    Volume* v = mgr.CreateVolume("fogzone");
    v->isGlobal = true;
    v->weight = 1.0f;
    auto* fog = v->AddComponent<FogVolumeComponent>();
    fog->density = {0.7f, true};
    fog->heightFalloff = {0.4f, true};

    mgr.Update({0.0f, 0.0f, 0.0f});
    const auto& stack = mgr.GetStack();
    EXPECT_NEAR(stack.fog.density.value, 0.7f, 1e-4f);
    EXPECT_NEAR(stack.fog.heightFalloff.value, 0.4f, 1e-4f);
}

TEST(VolumeManager_Console_GetStatusListsAllVolumes)
{
    VolumeManager mgr;
    mgr.Initialize();
    Volume* a = mgr.CreateVolume("sky");
    a->isGlobal = true;
    a->AddComponent<ExposureVolumeComponent>();
    Volume* b = mgr.CreateVolume("cave");
    b->AddComponent<FogVolumeComponent>();

    std::string status = mgr.Console_GetStatus();
    EXPECT_TRUE(status.find("sky") != std::string::npos);
    EXPECT_TRUE(status.find("cave") != std::string::npos);
    EXPECT_TRUE(status.find("[GLOBAL]") != std::string::npos);
    EXPECT_TRUE(status.find("[LOCAL]") != std::string::npos);
}
