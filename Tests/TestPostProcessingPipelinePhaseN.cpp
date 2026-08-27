/**
 * @file TestPostProcessingPipelinePhaseN.cpp
 * @brief Phase N — integration tests for RTHandleSystem + ConstantBufferRing activations
 *
 * Phase N wires two more Tier 2 graphics orphans into the real
 * `PostProcessingPipeline`:
 *
 *   1. `RTHandleSystem` — portable, per-pipeline member. `Initialize`
 *      follows the pipeline's reference viewport, `Resize()` forwards
 *      the new size via `SetReferenceSize`, `Shutdown()` clears all
 *      allocated handles.
 *   2. `ConstantBufferRing` — Windows-only, per-pipeline member.
 *      `Initialize(device, 2MB)` at pipeline startup when a device is
 *      attached; `BeginFrame` / `EndFrame` bracket `Process()`;
 *      `Shutdown()` releases the D3D11 buffer.
 *
 * These tests cover the portable accessor surface so they execute on
 * every platform's CI. CPU-oracle tests for `RTHandleSystem` itself
 * live in `TestRTHandleSystem.cpp`; Windows-only contract tests for
 * `ConstantBufferRing` live in `TestConstantBufferRing.cpp`.
 */

#include "TestFramework.h"

#include "Graphics/PostProcessingPipeline.h"

using Spark::Graphics::PostProcessingPipeline;

// =========================================================================
// RTHandleSystem — lifecycle follows the pipeline
// =========================================================================

TEST(PhaseN_RTHandleSystem_InitializedWithPipeline)
{
    PostProcessingPipeline pp;
    EXPECT_FALSE(pp.GetRTHandleSystem().IsInitialized());
    pp.Initialize(1920, 1080);
    EXPECT_TRUE(pp.GetRTHandleSystem().IsInitialized());
    EXPECT_EQ(pp.GetRTHandleSystem().GetReferenceWidth(), 1920u);
    EXPECT_EQ(pp.GetRTHandleSystem().GetReferenceHeight(), 1080u);
    pp.Shutdown();
    EXPECT_FALSE(pp.GetRTHandleSystem().IsInitialized());
}

TEST(PhaseN_RTHandleSystem_AllocateViaPipeline)
{
    PostProcessingPipeline pp;
    pp.Initialize(1920, 1080);
    uint32_t id = pp.GetRTHandleSystem().Allocate(0.5f, 0.5f);
    EXPECT_TRUE(id != 0);
    const auto* h = pp.GetRTHandleSystem().GetHandle(id);
    ASSERT_TRUE(h != nullptr);
    EXPECT_EQ(h->currentWidth, 960u);
    EXPECT_EQ(h->currentHeight, 540u);
    pp.Shutdown();
}

TEST(PhaseN_RTHandleSystem_ResizeForwardsReferenceSize)
{
    PostProcessingPipeline pp;
    pp.Initialize(1920, 1080);
    uint32_t id = pp.GetRTHandleSystem().Allocate(1.0f, 1.0f);

    pp.Resize(1280, 720);
    const auto* h = pp.GetRTHandleSystem().GetHandle(id);
    ASSERT_TRUE(h != nullptr);
    EXPECT_EQ(h->currentWidth, 1280u);
    EXPECT_EQ(h->currentHeight, 720u);
    EXPECT_EQ(pp.GetRTHandleSystem().GetReferenceWidth(), 1280u);
    EXPECT_EQ(pp.GetRTHandleSystem().GetReferenceHeight(), 720u);
    pp.Shutdown();
}

TEST(PhaseN_RTHandleSystem_SurvivesHeadlessProcess)
{
    PostProcessingPipeline pp;
    pp.Initialize(640, 480);
    // No D3D11 device — Process() early-outs for GPU work but the
    // RTHandleSystem tick is a no-op that must not crash.
    pp.Process(0.016f);
    EXPECT_TRUE(pp.GetRTHandleSystem().IsInitialized());
    pp.Shutdown();
}

// =========================================================================
// ConstantBufferRing — Windows-only accessor surface + headless fallback
// =========================================================================

TEST(PhaseN_ConstantBufferRing_HeadlessCapacityIsZero)
{
    PostProcessingPipeline pp;
    pp.Initialize(640, 480);
    // No device attached — the ring never initialises. Capacity reads
    // back as zero both on non-Windows (the accessor returns 0 at
    // compile time) and on Windows without a device (the metrics
    // struct is zeroed at construction).
    EXPECT_EQ(pp.GetConstantBufferRingCapacity(), 0u);
    EXPECT_EQ(pp.GetConstantBufferRingPeakUsage(), 0u);
    pp.Shutdown();
}

TEST(PhaseN_ConstantBufferRing_AccessorsSurviveShutdown)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    pp.Shutdown();
    // Shutdown resets the metrics. Calling the accessors after
    // Shutdown must still return zero without crashing.
    EXPECT_EQ(pp.GetConstantBufferRingCapacity(), 0u);
    EXPECT_EQ(pp.GetConstantBufferRingPeakUsage(), 0u);
}

// =========================================================================
// Process() is safe with the new per-frame hooks
// =========================================================================

TEST(PhaseN_ProcessRunsWithoutDevice)
{
    PostProcessingPipeline pp;
    pp.Initialize(320, 240);
    // Process() now calls ConstantBufferRing::BeginFrame/EndFrame in the
    // Windows path. Without a device the ring's BeginFrame guards on
    // m_initialized and returns false — EndFrame is a no-op. This test
    // pins the "no crash on headless Process" contract that Phase I/J/K
    // established and Phase N must preserve.
    pp.Process(0.016f);
    pp.Process(0.016f);
    pp.Process(0.016f);
    EXPECT_EQ(pp.GetActivePassCount(), 0);
    pp.Shutdown();
}
