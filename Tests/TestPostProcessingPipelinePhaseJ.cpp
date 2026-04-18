/**
 * @file TestPostProcessingPipelinePhaseJ.cpp
 * @brief Phase J — integration tests for the Tier 2 orphan activations
 *
 * Phase J wires four Tier 2 graphics orphans into the real PostProcessingPipeline:
 *
 *   1. `SSAOTemporalFilter` — activated as the 16th post-process pass and
 *      kept alive on both Windows and Linux (CPU-only reference code).
 *   2. `RenderTargetPool`   — owned per-pipeline, ticked each frame. The pool
 *      is Windows-only; headless builds return a stub status string.
 *   3. `GPUDebugMarkers`    — wraps PIX/RenderDoc annotations. Marker depth
 *      is always zero at rest; Process() pushes and pops an outer "PostProcessingPipeline"
 *      event region.
 *   4. `GPUTimestampQuery`  — per-pass scoped timestamps. Readings lag behind
 *      the CPU-side timings by two frames, so on warm-up the pipeline reports
 *      the CPU value. Headless builds never allocate the query pool.
 *
 * These tests cover the portable accessor surface exposed by
 * `PostProcessingPipeline` so they execute on Windows, Linux, and macOS CI.
 * The pass-enum ordering, metric array alignment, and graceful-fallback
 * behaviour are all pinned here — a regression in any of them fails loud.
 *
 * Direct CPU-oracle tests for `SSAOTemporalFilter` live in
 * `TestSSAOTemporalFilter.cpp`.
 */

#include "TestFramework.h"

#include "Graphics/PostProcessingPipeline.h"

#include <algorithm>
#include <string>
#include <vector>

using Spark::Graphics::PassMetrics;
using Spark::Graphics::PostProcessingPipeline;
using Spark::Graphics::PostProcessPass;

// =========================================================================
// Enum ordering and metrics — the Phase J slot layout is load-bearing.
// =========================================================================

TEST(PhaseJ_EnumOrdering_GTAOFirst)
{
    EXPECT_EQ(static_cast<int>(PostProcessPass::GTAO), 0);
}

TEST(PhaseJ_EnumOrdering_SSAOTemporalSecond)
{
    // Must sit immediately after GTAO so the temporal denoise reads the
    // GTAO-modulated scene before Bloom extracts highlights.
    EXPECT_EQ(static_cast<int>(PostProcessPass::SSAOTemporal), 1);
}

TEST(PhaseJ_EnumOrdering_BloomBumpedToThird)
{
    EXPECT_EQ(static_cast<int>(PostProcessPass::Bloom), 2);
}

TEST(PhaseJ_EnumOrdering_CountIsSixteen)
{
    EXPECT_EQ(static_cast<int>(PostProcessPass::Count), 16);
}

TEST(PhaseJ_EnumOrdering_SharpenIsLast)
{
    // Sharpen stays the last user-facing pass — it runs after LDR output.
    EXPECT_EQ(static_cast<int>(PostProcessPass::Sharpen) + 1, static_cast<int>(PostProcessPass::Count));
}

// =========================================================================
// Pipeline lifecycle, headless / no-device path
// =========================================================================

TEST(PhaseJ_HeadlessInitializeAndShutdown)
{
    PostProcessingPipeline pp;
    EXPECT_TRUE(pp.Initialize(640, 480));

    // The SSAOTemporalFilter runs on both Windows and Linux, and the pipeline
    // must have initialised its CPU-side history buffer already.
    EXPECT_TRUE(pp.GetSSAOTemporalFilter().IsInitialized());

    pp.Shutdown();
    EXPECT_FALSE(pp.GetSSAOTemporalFilter().IsInitialized());
}

TEST(PhaseJ_ResizePropagatesToSSAOTemporalHistory)
{
    PostProcessingPipeline pp;
    pp.Initialize(640, 480);
    pp.Resize(1280, 720);
    // Resize is a no-op when dimensions match the existing size, so a second
    // identical call must leave the filter in the same initialised state.
    EXPECT_TRUE(pp.GetSSAOTemporalFilter().IsInitialized());
    pp.Resize(1280, 720);
    EXPECT_TRUE(pp.GetSSAOTemporalFilter().IsInitialized());
    pp.Shutdown();
}

TEST(PhaseJ_ProcessIsSafeWithoutDevice)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    // No D3D11 device was set — Process() must return without crashing and
    // without incrementing the active pass count (BeginPass bails early when
    // m_context is null).
    pp.Process(0.016f);
    EXPECT_EQ(pp.GetActivePassCount(), 0);
    pp.Shutdown();
}

// =========================================================================
// Metrics array
// =========================================================================

TEST(PhaseJ_GetPassMetrics_HasSixteenEntries)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    auto metrics = pp.GetPassMetrics();
    EXPECT_EQ(static_cast<int>(metrics.size()), 16);
    pp.Shutdown();
}

TEST(PhaseJ_GetPassMetrics_SSAOTemporalNameAtSlotOne)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    auto metrics = pp.GetPassMetrics();
    EXPECT_EQ(metrics[0].name, std::string("GTAO"));
    EXPECT_EQ(metrics[1].name, std::string("SSAOTemporal"));
    EXPECT_EQ(metrics[2].name, std::string("Bloom"));
    EXPECT_EQ(metrics[metrics.size() - 1].name, std::string("Sharpen"));
    pp.Shutdown();
}

TEST(PhaseJ_Console_ListEffects_IncludesSSAOTemporal)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    std::string listing = pp.Console_ListEffects();
    EXPECT_TRUE(listing.find("SSAOTemporal") != std::string::npos);
    EXPECT_TRUE(listing.find("GTAO") != std::string::npos);
    pp.Shutdown();
}

// =========================================================================
// Phase J accessor surface — the four new public methods must be safe
// to call in all lifecycle states and return fallback values on Linux.
// =========================================================================

TEST(PhaseJ_GetRenderTargetPoolSize_HeadlessIsZero)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    // No device attached — the pool is never instantiated. On Windows it
    // would still be zero because Acquire() has not been called. Either
    // way, the return value must be zero.
    EXPECT_EQ(pp.GetRenderTargetPoolSize(), 0u);
    pp.Shutdown();
}

TEST(PhaseJ_Console_RenderTargetPoolStatus_ReturnsNonEmpty)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    std::string s = pp.Console_RenderTargetPoolStatus();
    // On Linux this returns the "(not compiled on this platform)" stub;
    // on Windows it returns the pool's own status line. Both paths must
    // be non-empty so UI panels never show a blank line.
    EXPECT_TRUE(!s.empty());
    pp.Shutdown();
}

TEST(PhaseJ_GetGPUMarkerDepth_AtRestIsZero)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    EXPECT_EQ(pp.GetGPUMarkerDepth(), 0u);
    // Process() brackets the chain with Begin/EndEvent. Outside of a
    // Process() call the depth must always return to zero.
    pp.Process(0.016f);
    EXPECT_EQ(pp.GetGPUMarkerDepth(), 0u);
    pp.Shutdown();
}

TEST(PhaseJ_GetPassTimeMs_IsNonNegative)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    for (int i = 0; i < static_cast<int>(PostProcessPass::Count); ++i)
    {
        EXPECT_GE(pp.GetPassTimeMs(static_cast<PostProcessPass>(i)), 0.0f);
    }
    // Out-of-range index must not crash and must return 0.
    EXPECT_NEAR(pp.GetPassTimeMs(static_cast<PostProcessPass>(999)), 0.0f, 1e-6f);
    pp.Shutdown();
}

// =========================================================================
// Settings accessors for the newly activated pass
// =========================================================================

TEST(PhaseJ_GetSSAOTemporalSettings_Mutable)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    pp.GetSSAOTemporalSettings().blendFactor = 0.5f;
    pp.GetSSAOTemporalSettings().useVarianceClipping = false;
    EXPECT_NEAR(pp.GetSSAOTemporalSettings().blendFactor, 0.5f, 1e-6f);
    EXPECT_FALSE(pp.GetSSAOTemporalSettings().useVarianceClipping);
    pp.Shutdown();
}

TEST(PhaseJ_SetEffectEnabled_SSAOTemporalToggles)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    EXPECT_FALSE(pp.IsEffectEnabled(PostProcessPass::SSAOTemporal));
    pp.SetEffectEnabled(PostProcessPass::SSAOTemporal, true);
    EXPECT_TRUE(pp.IsEffectEnabled(PostProcessPass::SSAOTemporal));
    pp.SetEffectEnabled(PostProcessPass::SSAOTemporal, false);
    EXPECT_FALSE(pp.IsEffectEnabled(PostProcessPass::SSAOTemporal));
    pp.Shutdown();
}
