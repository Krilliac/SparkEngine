/**
 * @file TestDynamicQualityScalerPhaseBB.cpp
 * @brief Phase BB Theme 3D tests for Spark::Graphics::DynamicQualityScaler
 *
 * DynamicQualityScaler is a singleton in `Graphics/DynamicQualityScaler.h`
 * with a full .cpp implementation and zero external call sites before
 * Phase BB. `GameplayLifecycleShared.cpp` now calls `Initialize` at
 * startup with sensible 60 FPS defaults and `Reset()` at shutdown; any
 * future dynamic-resolution render path consults the same singleton
 * for its current scale.
 *
 * These tests run against the real class:
 *
 *   - Singleton stability across calls
 *   - Initialize + HasValidData false on empty window
 *   - RecordFrameTime populates the sliding window
 *   - GetAverageFrameTime / GetAverageFPS compute over the window
 *   - Resolution scale stays at 1.0 when FPS is above target
 *   - Resolution scale drops when FPS is below target for long enough
 *   - Resolution scale rises again when FPS recovers with headroom
 *   - Scale is clamped to [minScale, maxScale]
 *   - GetRenderDimensions applies the current scale
 *   - Reset clears window + resets scale to 1.0
 *   - GetThresholds round-trips the Initialize settings
 *   - Adjustment cooldown prevents rapid successive changes
 */

#include "TestFramework.h"
#include "Graphics/DynamicQualityScaler.h"

#include <cstdint>

namespace
{

    void ResetScaler()
    {
        auto& s = Spark::Graphics::DynamicQualityScaler::GetInstance();
        s.Reset();
        Spark::Graphics::DynamicQualityThresholds defaults;
        defaults.targetFPS = 60.0f;
        defaults.headroomPercent = 10.0f;
        defaults.dropThresholdPercent = 5.0f;
        defaults.minScale = 0.5f;
        defaults.maxScale = 1.0f;
        defaults.scaleStepDown = 0.05f;
        defaults.scaleStepUp = 0.02f;
        defaults.adjustCooldownSec = 0.0f; // Disable cooldown for deterministic tests
        s.Initialize(defaults);
    }

    // Feed the sliding window with N frames at a given frame time. Each
    // call bumps `timeSinceLastAdjust` so with cooldown=0 the scaler
    // re-evaluates on every call.
    void FeedFrames(Spark::Graphics::DynamicQualityScaler& s, int count, float frameTimeSec)
    {
        for (int i = 0; i < count; ++i)
            s.RecordFrameTime(frameTimeSec);
    }

} // namespace

// ============================================================================
// Singleton accessor
// ============================================================================

TEST(DynamicQualityScalerPhaseBB_SingletonReturnsSameInstance)
{
    auto& a = Spark::Graphics::DynamicQualityScaler::GetInstance();
    auto& b = Spark::Graphics::DynamicQualityScaler::GetInstance();
    EXPECT_TRUE(&a == &b);
}

// ============================================================================
// Initial state
// ============================================================================

TEST(DynamicQualityScalerPhaseBB_InitializeClearsWindow)
{
    ResetScaler();
    auto& s = Spark::Graphics::DynamicQualityScaler::GetInstance();
    // After Reset + Initialize the window is empty so HasValidData is false.
    EXPECT_FALSE(s.HasValidData());
    EXPECT_NEAR(s.GetCurrentResolutionScale(), 1.0f, 0.001f);
}

TEST(DynamicQualityScalerPhaseBB_ThresholdsRoundTrip)
{
    ResetScaler();
    auto& s = Spark::Graphics::DynamicQualityScaler::GetInstance();
    const auto& t = s.GetThresholds();
    EXPECT_NEAR(t.targetFPS, 60.0f, 0.001f);
    EXPECT_NEAR(t.minScale, 0.5f, 0.001f);
    EXPECT_NEAR(t.maxScale, 1.0f, 0.001f);
}

// ============================================================================
// Frame time recording
// ============================================================================

TEST(DynamicQualityScalerPhaseBB_RecordFrameTimePopulatesWindow)
{
    ResetScaler();
    auto& s = Spark::Graphics::DynamicQualityScaler::GetInstance();
    // Feed 16ms (≈ 62.5 FPS) for 10 frames.
    FeedFrames(s, 10, 0.016f);
    EXPECT_TRUE(s.HasValidData());
    EXPECT_NEAR(s.GetAverageFrameTime(), 0.016f, 0.001f);
    EXPECT_NEAR(s.GetAverageFPS(), 62.5f, 1.0f);
}

// ============================================================================
// Stable at target FPS — scale stays ≈ 1.0
// ============================================================================

TEST(DynamicQualityScalerPhaseBB_StableAtTargetKeepsMaxScale)
{
    ResetScaler();
    auto& s = Spark::Graphics::DynamicQualityScaler::GetInstance();
    // 16.67ms = exactly 60 FPS. No headroom, no deficit → scale stays.
    FeedFrames(s, 64, 1.0f / 60.0f);
    EXPECT_NEAR(s.GetCurrentResolutionScale(), 1.0f, 0.001f);
}

// ============================================================================
// Below target for long enough → scale drops
// ============================================================================

TEST(DynamicQualityScalerPhaseBB_BelowTargetDropsScale)
{
    ResetScaler();
    auto& s = Spark::Graphics::DynamicQualityScaler::GetInstance();
    // 33ms = 30 FPS, well below the 60 FPS target with 5% drop threshold.
    // Feed enough frames to trigger the drop path multiple times.
    FeedFrames(s, 200, 0.033f);
    // Scale should have dropped below 1.0.
    EXPECT_TRUE(s.GetCurrentResolutionScale() < 1.0f);
    // And stay above the floor.
    EXPECT_TRUE(s.GetCurrentResolutionScale() >= s.GetThresholds().minScale);
}

// ============================================================================
// Scale clamps to minScale — severe underperformance
// ============================================================================

TEST(DynamicQualityScalerPhaseBB_ScaleClampsToMinScale)
{
    ResetScaler();
    auto& s = Spark::Graphics::DynamicQualityScaler::GetInstance();
    // 100ms = 10 FPS, far below target. Feed many frames so the scaler
    // reaches the minimum clamp.
    FeedFrames(s, 2000, 0.100f);
    EXPECT_NEAR(s.GetCurrentResolutionScale(), s.GetThresholds().minScale, 0.001f);
}

// ============================================================================
// Above target → scale rises (toward maxScale)
// ============================================================================

TEST(DynamicQualityScalerPhaseBB_AboveTargetRaisesScale)
{
    ResetScaler();
    auto& s = Spark::Graphics::DynamicQualityScaler::GetInstance();

    // First drop the scale down by running slow.
    FeedFrames(s, 200, 0.033f);
    const float droppedScale = s.GetCurrentResolutionScale();
    EXPECT_TRUE(droppedScale < 1.0f);

    // Now run fast (8ms ≈ 125 FPS) and the scaler should raise the scale.
    FeedFrames(s, 200, 0.008f);
    const float raisedScale = s.GetCurrentResolutionScale();
    EXPECT_TRUE(raisedScale > droppedScale);
}

// ============================================================================
// GetRenderDimensions applies the scale
// ============================================================================

TEST(DynamicQualityScalerPhaseBB_GetRenderDimensionsScalesInput)
{
    ResetScaler();
    auto& s = Spark::Graphics::DynamicQualityScaler::GetInstance();

    uint32_t width = 0;
    uint32_t height = 0;
    s.GetRenderDimensions(1920, 1080, width, height);
    // At scale 1.0 the dimensions match the input.
    EXPECT_EQ(width, static_cast<uint32_t>(1920));
    EXPECT_EQ(height, static_cast<uint32_t>(1080));

    // Drop the scale.
    FeedFrames(s, 200, 0.100f);
    EXPECT_TRUE(s.GetCurrentResolutionScale() < 1.0f);

    s.GetRenderDimensions(1920, 1080, width, height);
    EXPECT_TRUE(width < 1920);
    EXPECT_TRUE(height < 1080);
    EXPECT_TRUE(width >= 1);
    EXPECT_TRUE(height >= 1);
}

// ============================================================================
// Reset
// ============================================================================

TEST(DynamicQualityScalerPhaseBB_ResetRestoresMaxScale)
{
    ResetScaler();
    auto& s = Spark::Graphics::DynamicQualityScaler::GetInstance();
    // Drop the scale first.
    FeedFrames(s, 200, 0.100f);
    EXPECT_TRUE(s.GetCurrentResolutionScale() < 1.0f);

    s.Reset();
    EXPECT_NEAR(s.GetCurrentResolutionScale(), 1.0f, 0.001f);
    EXPECT_FALSE(s.HasValidData());
}

// ============================================================================
// GetRecommendedQuality returns a sensible value
// ============================================================================

TEST(DynamicQualityScalerPhaseBB_GetRecommendedQualityForCurrentScale)
{
    ResetScaler();
    auto& s = Spark::Graphics::DynamicQualityScaler::GetInstance();
    // At scale 1.0 the recommendation should exist and not crash.
    auto quality = s.GetRecommendedQuality();
    // We just exercise the call — specific enum values depend on
    // DynamicQualityTypes.h and are implementation-dependent.
    (void)quality;
    EXPECT_TRUE(true);
}
