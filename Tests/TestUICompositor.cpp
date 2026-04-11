/**
 * @file TestUICompositor.cpp
 * @brief Phase R — CPU reference tests for UICompositor
 *
 * Exercises the header-only `Spark::Graphics::UICompositor` class
 * directly. Despite living in the Graphics folder, the compositor
 * is pure CPU — it tracks a pool of `PooledRenderTarget` metadata
 * descriptors and a composition stack without any GPU resources.
 * Every test runs on every platform's CI.
 *
 * Phase R also wires the compositor as a per-instance member of
 * `Spark::UI::UISystem` with `Initialize` / `OnResize` / `Render`
 * lifecycle hooks; integration tests against the UISystem-owned
 * instance live in `TestUISystemPhaseR.cpp`.
 */

#include "TestFramework.h"

#include "../SparkEngine/Source/Graphics/UICompositor.h"

using Spark::Graphics::CompositeOp;
using Spark::Graphics::CompositeRequest;
using Spark::Graphics::UICompositor;

namespace
{
    CompositeRequest MakeRequest(CompositeOp op = CompositeOp::AlphaBlend, uint32_t w = 512, uint32_t h = 512,
                                 float opacity = 1.0f)
    {
        CompositeRequest r;
        r.operation = op;
        r.width = w;
        r.height = h;
        r.opacity = opacity;
        r.blurRadius = 0.0f;
        return r;
    }
} // namespace

// =========================================================================
// Lifecycle
// =========================================================================

TEST(UICompositor_InitializeAndShutdown)
{
    UICompositor c;
    EXPECT_FALSE(c.IsInitialized());
    c.Initialize(1920, 1080);
    EXPECT_TRUE(c.IsInitialized());
    EXPECT_EQ(static_cast<int>(c.GetPoolSize()), 0);
    EXPECT_EQ(static_cast<int>(c.GetStackDepth()), 0);
    c.Shutdown();
    EXPECT_FALSE(c.IsInitialized());
}

TEST(UICompositor_BeginCompositeBeforeInitializeFails)
{
    UICompositor c;
    int idx = c.BeginComposite(MakeRequest());
    EXPECT_EQ(idx, -1);
}

TEST(UICompositor_EndCompositeOnEmptyStackIsSafe)
{
    UICompositor c;
    c.Initialize(1024, 768);
    c.EndComposite(); // no crash
    EXPECT_EQ(static_cast<int>(c.GetStackDepth()), 0);
    c.Shutdown();
}

// =========================================================================
// Push / pop composition levels
// =========================================================================

TEST(UICompositor_BeginCompositeAllocatesPoolEntry)
{
    UICompositor c;
    c.Initialize(1024, 768);
    int idx = c.BeginComposite(MakeRequest(CompositeOp::AlphaBlend, 512, 512));
    EXPECT_GE(idx, 0);
    EXPECT_EQ(static_cast<int>(c.GetStackDepth()), 1);
    EXPECT_EQ(static_cast<int>(c.GetPoolSize()), 1);
    EXPECT_EQ(c.GetActiveTargetCount(), 1u);
    c.EndComposite();
    EXPECT_EQ(static_cast<int>(c.GetStackDepth()), 0);
    EXPECT_EQ(c.GetActiveTargetCount(), 0u);
    c.Shutdown();
}

TEST(UICompositor_BeginCompositeUsesScreenSizeWhenZero)
{
    UICompositor c;
    c.Initialize(1920, 1080);
    // Request with width=0 / height=0 falls back to the screen size.
    CompositeRequest req;
    req.operation = CompositeOp::GaussianBlur;
    req.blurRadius = 4.0f;
    int idx = c.BeginComposite(req);
    EXPECT_GE(idx, 0);
    EXPECT_EQ(static_cast<int>(c.GetPoolSize()), 1);
    c.EndComposite();
    c.Shutdown();
}

TEST(UICompositor_NestedCompositeLevelsStack)
{
    UICompositor c;
    c.Initialize(1024, 768);
    int a = c.BeginComposite(MakeRequest(CompositeOp::AlphaBlend, 1024, 768));
    int b = c.BeginComposite(MakeRequest(CompositeOp::GaussianBlur, 512, 512));
    int d = c.BeginComposite(MakeRequest(CompositeOp::Mask, 256, 256));

    EXPECT_GE(a, 0);
    EXPECT_GE(b, 0);
    EXPECT_GE(d, 0);
    EXPECT_EQ(static_cast<int>(c.GetStackDepth()), 3);
    EXPECT_EQ(c.GetActiveTargetCount(), 3u);

    c.EndComposite();
    c.EndComposite();
    c.EndComposite();
    EXPECT_EQ(static_cast<int>(c.GetStackDepth()), 0);
    EXPECT_EQ(c.GetActiveTargetCount(), 0u);
    c.Shutdown();
}

TEST(UICompositor_MaxStackDepthLimit)
{
    UICompositor c;
    c.Initialize(1024, 768);
    // Push up to the kMaxStackDepth = 8 limit.
    for (int i = 0; i < 8; ++i)
    {
        int idx = c.BeginComposite(MakeRequest(CompositeOp::AlphaBlend, 64, 64));
        EXPECT_GE(idx, 0);
    }
    EXPECT_EQ(static_cast<int>(c.GetStackDepth()), 8);

    // The 9th must fail without affecting stack depth.
    int overflow = c.BeginComposite(MakeRequest(CompositeOp::AlphaBlend, 64, 64));
    EXPECT_EQ(overflow, -1);
    EXPECT_EQ(static_cast<int>(c.GetStackDepth()), 8);

    // Drain back to zero.
    for (int i = 0; i < 8; ++i)
    {
        c.EndComposite();
    }
    EXPECT_EQ(static_cast<int>(c.GetStackDepth()), 0);
    c.Shutdown();
}

// =========================================================================
// Pool reuse
// =========================================================================

TEST(UICompositor_PoolReusesReleasedTargets)
{
    UICompositor c;
    c.Initialize(1024, 768);

    // First push + pop creates one pool entry.
    int a = c.BeginComposite(MakeRequest(CompositeOp::AlphaBlend, 512, 512));
    EXPECT_EQ(static_cast<int>(c.GetPoolSize()), 1);
    c.EndComposite();

    // Second push with the same dimensions must reuse the released
    // entry — pool size stays at 1.
    int b = c.BeginComposite(MakeRequest(CompositeOp::AlphaBlend, 512, 512));
    EXPECT_EQ(static_cast<int>(c.GetPoolSize()), 1);
    EXPECT_EQ(a, b); // same index → same pool slot
    c.EndComposite();
    c.Shutdown();
}

TEST(UICompositor_PoolAllocatesNewSlotForDifferentSize)
{
    UICompositor c;
    c.Initialize(1024, 768);
    c.BeginComposite(MakeRequest(CompositeOp::AlphaBlend, 512, 512));
    c.EndComposite();

    // Different dimensions → different pool entry.
    c.BeginComposite(MakeRequest(CompositeOp::AlphaBlend, 256, 256));
    EXPECT_EQ(static_cast<int>(c.GetPoolSize()), 2);
    c.EndComposite();
    c.Shutdown();
}

TEST(UICompositor_MaxPoolSizeLimit)
{
    UICompositor c;
    c.Initialize(4096, 4096);
    // Push 16 distinct-sized composites to fill the pool. Each must
    // be released before the next Begin so we only grow the pool,
    // not the stack.
    for (int i = 0; i < 16; ++i)
    {
        int idx = c.BeginComposite(MakeRequest(CompositeOp::AlphaBlend, 32u + i, 32u + i));
        EXPECT_GE(idx, 0);
        c.EndComposite();
    }
    EXPECT_EQ(static_cast<int>(c.GetPoolSize()), 16);

    // The 17th distinct-size request has no free matching slot and
    // the pool is at kMaxPoolSize — must fail.
    int overflow = c.BeginComposite(MakeRequest(CompositeOp::AlphaBlend, 999, 999));
    EXPECT_EQ(overflow, -1);
    EXPECT_EQ(static_cast<int>(c.GetStackDepth()), 0);
    c.Shutdown();
}

// =========================================================================
// Frame reclaim
// =========================================================================

TEST(UICompositor_BeginFrameReclaimsStaleEntries)
{
    UICompositor c;
    c.Initialize(1024, 768);

    // Create a pool entry and release it.
    c.BeginComposite(MakeRequest(CompositeOp::AlphaBlend, 512, 512));
    c.EndComposite();
    EXPECT_EQ(static_cast<int>(c.GetPoolSize()), 1);

    // Advance 15 frames with no activity — the 10-frame idle
    // threshold should reclaim the released entry.
    for (int i = 0; i < 15; ++i)
    {
        c.BeginFrame();
    }
    EXPECT_EQ(static_cast<int>(c.GetPoolSize()), 0);
    c.Shutdown();
}

TEST(UICompositor_BeginFrameClearsStack)
{
    UICompositor c;
    c.Initialize(1024, 768);
    // Push some composites and "forget" to pop them (simulating a
    // mid-frame bail-out). BeginFrame is documented to clear the
    // stack so the next frame starts fresh.
    c.BeginComposite(MakeRequest(CompositeOp::AlphaBlend, 512, 512));
    c.BeginComposite(MakeRequest(CompositeOp::GaussianBlur, 256, 256));
    EXPECT_EQ(static_cast<int>(c.GetStackDepth()), 2);

    c.BeginFrame();
    EXPECT_EQ(static_cast<int>(c.GetStackDepth()), 0);
    c.Shutdown();
}

// =========================================================================
// Console status
// =========================================================================

TEST(UICompositor_Console_GetStatusFormatting)
{
    UICompositor c;
    c.Initialize(1024, 768);
    c.BeginComposite(MakeRequest(CompositeOp::AlphaBlend, 256, 256));
    c.BeginComposite(MakeRequest(CompositeOp::GaussianBlur, 128, 128));

    std::string status = c.Console_GetStatus();
    EXPECT_TRUE(status.find("UICompositor") != std::string::npos);
    EXPECT_TRUE(status.find("pool=2") != std::string::npos);
    EXPECT_TRUE(status.find("active=2") != std::string::npos);
    EXPECT_TRUE(status.find("depth=2") != std::string::npos);

    c.EndComposite();
    c.EndComposite();
    c.Shutdown();
}
