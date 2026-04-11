/**
 * @file TestPostProcessingPipelinePhaseK.cpp
 * @brief Phase K — integration tests for VolumeManager activation
 *
 * Phase K wires `Spark::Graphics::VolumeManager` into the real
 * `PostProcessingPipeline`. The pipeline now owns one manager instance,
 * drives `Update()` once per `Process()` call, and pushes the blended
 * stack into the existing effect settings structs via `ApplyVolumeStack()`
 * unless the caller explicitly disables volume blending.
 *
 * These tests cover the portable integration surface so they run on
 * Windows, Linux, and macOS CI. CPU-oracle tests for `VolumeManager`
 * itself live in `TestVolumeManager.cpp`.
 */

#include "TestFramework.h"

#include "../SparkEngine/Source/Graphics/PostProcessingPipeline.h"

using Spark::Graphics::BloomVolumeComponent;
using Spark::Graphics::ColorGradingVolumeComponent;
using Spark::Graphics::ExposureVolumeComponent;
using Spark::Graphics::FogVolumeComponent;
using Spark::Graphics::PostProcessingPipeline;
using Spark::Graphics::PostProcessPass;
using Spark::Graphics::Volume;

// =========================================================================
// Lifecycle: manager follows the pipeline
// =========================================================================

TEST(PhaseK_VolumeManager_InitializedWithPipeline)
{
    PostProcessingPipeline pp;
    EXPECT_FALSE(pp.GetVolumeManager().IsInitialized());
    EXPECT_TRUE(pp.Initialize(320, 240));
    EXPECT_TRUE(pp.GetVolumeManager().IsInitialized());
    pp.Shutdown();
    EXPECT_FALSE(pp.GetVolumeManager().IsInitialized());
}

TEST(PhaseK_CameraPosition_Defaults)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    const auto& pos = pp.GetCameraPosition();
    EXPECT_NEAR(pos.x, 0.0f, 1e-6f);
    EXPECT_NEAR(pos.y, 0.0f, 1e-6f);
    EXPECT_NEAR(pos.z, 0.0f, 1e-6f);
    pp.Shutdown();
}

TEST(PhaseK_SetCameraPosition_IsReadBack)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    pp.SetCameraPosition({1.0f, 2.0f, 3.0f});
    EXPECT_NEAR(pp.GetCameraPosition().x, 1.0f, 1e-6f);
    EXPECT_NEAR(pp.GetCameraPosition().y, 2.0f, 1e-6f);
    EXPECT_NEAR(pp.GetCameraPosition().z, 3.0f, 1e-6f);
    pp.Shutdown();
}

TEST(PhaseK_VolumeBlendEnabled_DefaultIsTrue)
{
    PostProcessingPipeline pp;
    EXPECT_TRUE(pp.IsVolumeBlendEnabled());
    pp.SetVolumeBlendEnabled(false);
    EXPECT_FALSE(pp.IsVolumeBlendEnabled());
    pp.SetVolumeBlendEnabled(true);
    EXPECT_TRUE(pp.IsVolumeBlendEnabled());
}

// =========================================================================
// ApplyVolumeStack pushes volume values into effect settings
// =========================================================================

TEST(PhaseK_ApplyVolumeStack_BloomIntensityLandsOnSettings)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);

    Volume* v = pp.GetVolumeManager().CreateVolume("bloom");
    v->isGlobal = true;
    v->weight = 1.0f;
    auto* bloom = v->AddComponent<BloomVolumeComponent>();
    bloom->intensity = {3.5f, true};
    bloom->threshold = {0.25f, true};
    bloom->scatter = {0.9f, true};

    pp.GetVolumeManager().Update(pp.GetCameraPosition());
    pp.ApplyVolumeStack();

    EXPECT_NEAR(pp.GetBloomSettings().intensity, 3.5f, 1e-4f);
    EXPECT_NEAR(pp.GetBloomSettings().threshold, 0.25f, 1e-4f);
    EXPECT_NEAR(pp.GetBloomSettings().scatter, 0.9f, 1e-4f);
    pp.Shutdown();
}

TEST(PhaseK_ApplyVolumeStack_ColorGradingLiftLandsOnSettings)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);

    Volume* v = pp.GetVolumeManager().CreateVolume("grade");
    v->isGlobal = true;
    v->weight = 1.0f;
    auto* color = v->AddComponent<ColorGradingVolumeComponent>();
    color->liftR = {0.1f, true};
    color->liftG = {0.2f, true};
    color->liftB = {0.3f, true};
    color->saturation = {1.5f, true};
    color->contrast = {1.25f, true};
    color->temperature = {0.15f, true};
    color->tint = {-0.05f, true};

    pp.GetVolumeManager().Update(pp.GetCameraPosition());
    pp.ApplyVolumeStack();

    const auto& cg = pp.GetColorGradingSettings();
    EXPECT_NEAR(cg.lift.x, 0.1f, 1e-4f);
    EXPECT_NEAR(cg.lift.y, 0.2f, 1e-4f);
    EXPECT_NEAR(cg.lift.z, 0.3f, 1e-4f);
    EXPECT_NEAR(cg.saturation, 1.5f, 1e-4f);
    EXPECT_NEAR(cg.contrast, 1.25f, 1e-4f);
    EXPECT_NEAR(cg.temperature, 0.15f, 1e-4f);
    EXPECT_NEAR(cg.tint, -0.05f, 1e-4f);
    pp.Shutdown();
}

TEST(PhaseK_ApplyVolumeStack_ExposureCompensationLandsOnSettings)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);

    Volume* v = pp.GetVolumeManager().CreateVolume("exp");
    v->isGlobal = true;
    v->weight = 1.0f;
    auto* exp = v->AddComponent<ExposureVolumeComponent>();
    exp->compensationEV = {1.75f, true};

    pp.GetVolumeManager().Update(pp.GetCameraPosition());
    pp.ApplyVolumeStack();

    EXPECT_NEAR(pp.GetAutoExposureSettings().compensationEV, 1.75f, 1e-4f);
    pp.Shutdown();
}

// =========================================================================
// Process() drives the blend once per frame
// =========================================================================

TEST(PhaseK_ProcessDrivesVolumeBlend)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    Volume* v = pp.GetVolumeManager().CreateVolume("proc");
    v->isGlobal = true;
    v->weight = 1.0f;
    auto* bloom = v->AddComponent<BloomVolumeComponent>();
    bloom->intensity = {2.2f, true};

    pp.Process(0.016f);
    // Process() calls Update() + ApplyVolumeStack() before any pass runs.
    EXPECT_NEAR(pp.GetBloomSettings().intensity, 2.2f, 1e-4f);
    pp.Shutdown();
}

TEST(PhaseK_ProcessRespectsVolumeBlendEnabled)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    pp.GetBloomSettings().intensity = 0.42f;

    Volume* v = pp.GetVolumeManager().CreateVolume("nope");
    v->isGlobal = true;
    v->weight = 1.0f;
    auto* bloom = v->AddComponent<BloomVolumeComponent>();
    bloom->intensity = {99.0f, true};

    pp.SetVolumeBlendEnabled(false);
    pp.Process(0.016f);
    // The volume stack was still evaluated (tests below verify that) but
    // the apply step was skipped — bloom intensity stays where we left it.
    EXPECT_NEAR(pp.GetBloomSettings().intensity, 0.42f, 1e-4f);
    pp.Shutdown();
}

TEST(PhaseK_VolumeStackEvaluatedEvenWhenBlendDisabled)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    Volume* v = pp.GetVolumeManager().CreateVolume("eval-only");
    v->isGlobal = true;
    v->weight = 1.0f;
    auto* bloom = v->AddComponent<BloomVolumeComponent>();
    bloom->intensity = {5.0f, true};

    pp.SetVolumeBlendEnabled(false);
    pp.Process(0.016f);
    // Stack still has the blended value — query-only callers (debug panels)
    // see the live stack without the pipeline settings being overwritten.
    EXPECT_NEAR(pp.GetVolumeManager().GetStack().bloom.intensity.value, 5.0f, 1e-4f);
    pp.Shutdown();
}

// =========================================================================
// Spatial behaviour: camera position is honoured
// =========================================================================

TEST(PhaseK_LocalVolumeBindsOnlyNearCamera)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);

    // Remember the initial bloom intensity default (pipeline-owned).
    const float initialIntensity = pp.GetBloomSettings().intensity;

    Volume* v = pp.GetVolumeManager().CreateVolume("cave");
    v->isGlobal = false;
    v->boundsMin = {-1.0f, -1.0f, -1.0f};
    v->boundsMax = {1.0f, 1.0f, 1.0f};
    v->weight = 1.0f;
    v->blendDistance = 0.25f; // very narrow fade
    auto* bloom = v->AddComponent<BloomVolumeComponent>();
    bloom->intensity = {initialIntensity + 10.0f, true}; // big delta

    // Camera far away — volume does not apply, initial intensity preserved.
    pp.SetCameraPosition({500.0f, 500.0f, 500.0f});
    pp.Process(0.016f);
    EXPECT_NEAR(pp.GetBloomSettings().intensity, initialIntensity, 1e-4f);

    // Move the camera inside the AABB — volume now fully applies.
    pp.SetCameraPosition({0.0f, 0.0f, 0.0f});
    pp.Process(0.016f);
    EXPECT_NEAR(pp.GetBloomSettings().intensity, initialIntensity + 10.0f, 1e-4f);

    pp.Shutdown();
}
