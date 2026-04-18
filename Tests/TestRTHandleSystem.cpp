/**
 * @file TestRTHandleSystem.cpp
 * @brief Phase N — portable CPU tests for RTHandleSystem
 *
 * Exercises the header-only `Spark::Graphics::RTHandleSystem` class
 * directly. Despite living in the Graphics folder, `RTHandleSystem`
 * is pure CPU — it tracks allocation metadata (reference size, per-
 * handle scale factors, max-history bookkeeping) without binding to
 * any GPU texture. The format field is a raw `uint32_t` (DXGI_FORMAT
 * value), so the header compiles on every platform.
 *
 * Phase N also wires `RTHandleSystem` as a per-pipeline member of
 * `PostProcessingPipeline` with Initialize/Shutdown/Resize hooks;
 * integration tests against the pipeline-owned instance live in
 * `TestPostProcessingPipelinePhaseN.cpp`.
 */

#include "TestFramework.h"

#include "Graphics/RTHandleSystem.h"

using Spark::Graphics::BufferedRTHandle;
using Spark::Graphics::RTHandle;
using Spark::Graphics::RTHandleSystem;

// =========================================================================
// Lifecycle
// =========================================================================

TEST(RTHandleSystem_InitializeAndShutdown)
{
    RTHandleSystem sys;
    EXPECT_FALSE(sys.IsInitialized());
    sys.Initialize(1920, 1080);
    EXPECT_TRUE(sys.IsInitialized());
    EXPECT_EQ(sys.GetReferenceWidth(), 1920u);
    EXPECT_EQ(sys.GetReferenceHeight(), 1080u);
    EXPECT_EQ(static_cast<int>(sys.GetHandleCount()), 0);
    sys.Shutdown();
    EXPECT_FALSE(sys.IsInitialized());
}

TEST(RTHandleSystem_AllocateBeforeInitializeReturnsZero)
{
    RTHandleSystem sys;
    uint32_t id = sys.Allocate(1.0f, 1.0f);
    EXPECT_EQ(id, 0u);
}

// =========================================================================
// Allocation
// =========================================================================

TEST(RTHandleSystem_AllocateFullResolutionHandle)
{
    RTHandleSystem sys;
    sys.Initialize(1920, 1080);

    uint32_t id = sys.Allocate(1.0f, 1.0f);
    EXPECT_TRUE(id != 0);
    const RTHandle* h = sys.GetHandle(id);
    EXPECT_TRUE(h != nullptr);
    EXPECT_EQ(h->currentWidth, 1920u);
    EXPECT_EQ(h->currentHeight, 1080u);
    EXPECT_EQ(h->allocatedWidth, 1920u);
    EXPECT_EQ(h->allocatedHeight, 1080u);
    sys.Shutdown();
}

TEST(RTHandleSystem_AllocateHalfResolutionHandle)
{
    RTHandleSystem sys;
    sys.Initialize(1920, 1080);
    uint32_t id = sys.Allocate(0.5f, 0.5f);
    const RTHandle* h = sys.GetHandle(id);
    EXPECT_TRUE(h != nullptr);
    EXPECT_EQ(h->currentWidth, 960u);
    EXPECT_EQ(h->currentHeight, 540u);
    sys.Shutdown();
}

TEST(RTHandleSystem_MultipleHandlesGetDistinctIds)
{
    RTHandleSystem sys;
    sys.Initialize(800, 600);
    uint32_t a = sys.Allocate(1.0f, 1.0f);
    uint32_t b = sys.Allocate(0.5f, 0.5f);
    uint32_t c = sys.Allocate(0.25f, 0.25f);
    EXPECT_TRUE(a != b);
    EXPECT_TRUE(b != c);
    EXPECT_TRUE(a != c);
    EXPECT_EQ(static_cast<int>(sys.GetHandleCount()), 3);
    sys.Shutdown();
}

TEST(RTHandleSystem_ReleaseRemovesHandle)
{
    RTHandleSystem sys;
    sys.Initialize(800, 600);
    uint32_t id = sys.Allocate(1.0f, 1.0f);
    EXPECT_TRUE(sys.GetHandle(id) != nullptr);
    sys.Release(id);
    EXPECT_TRUE(sys.GetHandle(id) == nullptr);
    EXPECT_EQ(static_cast<int>(sys.GetHandleCount()), 0);
    sys.Shutdown();
}

// =========================================================================
// Reference size changes
// =========================================================================

TEST(RTHandleSystem_SetReferenceSize_GrowsHandles)
{
    RTHandleSystem sys;
    sys.Initialize(1920, 1080);
    uint32_t id = sys.Allocate(1.0f, 1.0f);

    sys.SetReferenceSize(2560, 1440);
    const RTHandle* h = sys.GetHandle(id);
    EXPECT_TRUE(h != nullptr);
    EXPECT_EQ(h->currentWidth, 2560u);
    EXPECT_EQ(h->currentHeight, 1440u);
    // Growing past the max-history triggers an allocation bump.
    EXPECT_GE(h->allocatedWidth, 2560u);
    EXPECT_GE(h->allocatedHeight, 1440u);
    sys.Shutdown();
}

TEST(RTHandleSystem_SetReferenceSize_ShrinkDoesNotReallocate)
{
    RTHandleSystem sys;
    sys.Initialize(1920, 1080);
    uint32_t id = sys.Allocate(1.0f, 1.0f);
    uint32_t originalAllocW = sys.GetHandle(id)->allocatedWidth;
    uint32_t originalAllocH = sys.GetHandle(id)->allocatedHeight;

    sys.SetReferenceSize(1280, 720);
    const RTHandle* h = sys.GetHandle(id);
    // Allocated sizes stay at the max historical values; only current sizes shrink.
    EXPECT_EQ(h->allocatedWidth, originalAllocW);
    EXPECT_EQ(h->allocatedHeight, originalAllocH);
    EXPECT_EQ(h->currentWidth, 1280u);
    EXPECT_EQ(h->currentHeight, 720u);
    sys.Shutdown();
}

TEST(RTHandleSystem_SetReferenceSize_HalfScaleHandleTracksReference)
{
    RTHandleSystem sys;
    sys.Initialize(1920, 1080);
    uint32_t id = sys.Allocate(0.5f, 0.5f);

    sys.SetReferenceSize(1280, 720);
    const RTHandle* h = sys.GetHandle(id);
    EXPECT_EQ(h->currentWidth, 640u);
    EXPECT_EQ(h->currentHeight, 360u);
    sys.Shutdown();
}

// =========================================================================
// Dynamic resolution
// =========================================================================

TEST(RTHandleSystem_SetDynamicResolutionScale_ShrinksCurrent)
{
    RTHandleSystem sys;
    sys.Initialize(1920, 1080);
    uint32_t id = sys.Allocate(1.0f, 1.0f);

    sys.SetDynamicResolutionScale(0.5f);
    EXPECT_NEAR(sys.GetDynamicScale(), 0.5f, 1e-6f);
    const RTHandle* h = sys.GetHandle(id);
    EXPECT_EQ(h->currentWidth, 960u);
    EXPECT_EQ(h->currentHeight, 540u);
    // Allocation stays at the full reference resolution.
    EXPECT_EQ(h->allocatedWidth, 1920u);
    EXPECT_EQ(h->allocatedHeight, 1080u);
    sys.Shutdown();
}

TEST(RTHandleSystem_SetDynamicResolutionScale_ClampsToRange)
{
    RTHandleSystem sys;
    sys.Initialize(1920, 1080);
    sys.SetDynamicResolutionScale(2.0f);
    EXPECT_NEAR(sys.GetDynamicScale(), 1.0f, 1e-6f); // clamped to 1.0

    sys.SetDynamicResolutionScale(0.0f);
    EXPECT_NEAR(sys.GetDynamicScale(), 0.25f, 1e-6f); // clamped to 0.25
    sys.Shutdown();
}

// =========================================================================
// UV scale
// =========================================================================

TEST(RTHandleSystem_GetUVScaleTracksAllocatedVsCurrent)
{
    RTHandleSystem sys;
    sys.Initialize(1920, 1080);
    uint32_t id = sys.Allocate(1.0f, 1.0f);

    // Without dynamic scale, current == allocated → UV scale = 1.0.
    const RTHandle* h = sys.GetHandle(id);
    EXPECT_NEAR(h->GetUVScaleX(), 1.0f, 1e-6f);
    EXPECT_NEAR(h->GetUVScaleY(), 1.0f, 1e-6f);

    // At half dynamic resolution: current = 960, allocated = 1920 → UV = 0.5.
    sys.SetDynamicResolutionScale(0.5f);
    const RTHandle* h2 = sys.GetHandle(id);
    EXPECT_NEAR(h2->GetUVScaleX(), 0.5f, 1e-6f);
    EXPECT_NEAR(h2->GetUVScaleY(), 0.5f, 1e-6f);
    sys.Shutdown();
}

// =========================================================================
// Console status
// =========================================================================

TEST(RTHandleSystem_Console_GetStatusFormatting)
{
    RTHandleSystem sys;
    sys.Initialize(1920, 1080);
    sys.Allocate(1.0f, 1.0f);
    sys.Allocate(0.5f, 0.5f);
    std::string status = sys.Console_GetStatus();
    EXPECT_TRUE(status.find("RTHandleSystem") != std::string::npos);
    EXPECT_TRUE(status.find("1920") != std::string::npos);
    EXPECT_TRUE(status.find("1080") != std::string::npos);
    EXPECT_TRUE(status.find("handles=2") != std::string::npos);
    sys.Shutdown();
}

// =========================================================================
// Buffered RT handle (double-buffered helper)
// =========================================================================

TEST(BufferedRTHandle_InitializeAllocatesTwoHandles)
{
    RTHandleSystem sys;
    sys.Initialize(1024, 768);

    BufferedRTHandle buf;
    buf.Initialize(sys, 1.0f, 1.0f);
    EXPECT_EQ(static_cast<int>(sys.GetHandleCount()), 2);

    EXPECT_TRUE(buf.Current() != nullptr);
    EXPECT_TRUE(buf.Previous() != nullptr);
    // Distinct handles on each side.
    EXPECT_TRUE(buf.Current() != buf.Previous());
    sys.Shutdown();
}

TEST(BufferedRTHandle_SwapAlternatesCurrentAndPrevious)
{
    RTHandleSystem sys;
    sys.Initialize(1024, 768);

    BufferedRTHandle buf;
    buf.Initialize(sys, 1.0f, 1.0f);

    const RTHandle* firstCurrent = buf.Current();
    const RTHandle* firstPrevious = buf.Previous();

    buf.Swap();
    EXPECT_EQ(buf.Current(), firstPrevious);
    EXPECT_EQ(buf.Previous(), firstCurrent);

    buf.Swap();
    EXPECT_EQ(buf.Current(), firstCurrent);
    EXPECT_EQ(buf.Previous(), firstPrevious);
    sys.Shutdown();
}
