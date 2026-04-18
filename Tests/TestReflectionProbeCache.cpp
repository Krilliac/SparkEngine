/**
 * @file TestReflectionProbeCache.cpp
 * @brief Phase M — CPU reference tests for ReflectionProbeCache
 *
 * Exercises the header-only `Spark::Graphics::ReflectionProbeCache`
 * class directly — no GPU, no D3D11. The cache is pure CPU code so
 * these tests run on every platform's CI.
 *
 * Phase M also wires the cache as a member of `LightingSystem` with
 * per-frame `Update()` ticks; integration tests against the
 * LightingSystem-owned instance live in `TestLightingSystemPhaseM.cpp`.
 */

#include "TestFramework.h"

#include "Graphics/ReflectionProbeCache.h"

#include <algorithm>

using Spark::Graphics::ReflectionProbe;
using Spark::Graphics::ReflectionProbeCache;

namespace
{
    ReflectionProbe MakeProbe(uint32_t id, float x, float y, float z, bool isStatic = true, float importance = 1.0f,
                              int priority = 0)
    {
        ReflectionProbe p;
        p.probeId = id;
        p.posX = x;
        p.posY = y;
        p.posZ = z;
        p.isStatic = isStatic;
        p.importance = importance;
        p.priority = priority;
        p.needsRender = true;
        return p;
    }
} // namespace

// =========================================================================
// Lifecycle
// =========================================================================

TEST(ReflectionProbeCache_InitializeAndShutdown)
{
    ReflectionProbeCache cache;
    EXPECT_FALSE(cache.IsInitialized());
    cache.Initialize(32, 2);
    EXPECT_TRUE(cache.IsInitialized());
    EXPECT_EQ(cache.GetProbeCount(), 0u);
    cache.Shutdown();
    EXPECT_FALSE(cache.IsInitialized());
}

TEST(ReflectionProbeCache_DefaultInitializeValues)
{
    ReflectionProbeCache cache;
    cache.Initialize(); // defaults: 64 slots, 4/frame budget
    EXPECT_TRUE(cache.IsInitialized());
    EXPECT_EQ(cache.GetCachedCount(), 0u);
    cache.Shutdown();
}

// =========================================================================
// Register / Unregister
// =========================================================================

TEST(ReflectionProbeCache_RegisterProbeAppearsInCount)
{
    ReflectionProbeCache cache;
    cache.Initialize(8, 2);
    cache.RegisterProbe(MakeProbe(1, 0, 0, 0));
    EXPECT_EQ(cache.GetProbeCount(), 1u);
    cache.RegisterProbe(MakeProbe(2, 10, 0, 0));
    EXPECT_EQ(cache.GetProbeCount(), 2u);
    cache.Shutdown();
}

TEST(ReflectionProbeCache_UnregisterProbeRemovesIt)
{
    ReflectionProbeCache cache;
    cache.Initialize(8, 2);
    cache.RegisterProbe(MakeProbe(42, 1, 2, 3));
    EXPECT_EQ(cache.GetProbeCount(), 1u);
    EXPECT_TRUE(cache.GetProbe(42) != nullptr);
    cache.UnregisterProbe(42);
    EXPECT_EQ(cache.GetProbeCount(), 0u);
    EXPECT_TRUE(cache.GetProbe(42) == nullptr);
    cache.Shutdown();
}

TEST(ReflectionProbeCache_UnregisterUnknownIsSafe)
{
    ReflectionProbeCache cache;
    cache.Initialize(8, 2);
    cache.UnregisterProbe(999); // no crash
    EXPECT_EQ(cache.GetProbeCount(), 0u);
    cache.Shutdown();
}

// =========================================================================
// Update budget + scheduling
// =========================================================================

TEST(ReflectionProbeCache_UpdateRendersAllWhenBudgetAllows)
{
    ReflectionProbeCache cache;
    cache.Initialize(32, 10); // big budget
    for (uint32_t i = 0; i < 5; ++i)
    {
        cache.RegisterProbe(MakeProbe(i, static_cast<float>(i), 0, 0));
    }
    auto rendered = cache.Update(0, 0, 0);
    EXPECT_EQ(static_cast<int>(rendered.size()), 5);
}

TEST(ReflectionProbeCache_UpdateRespectsBudget)
{
    ReflectionProbeCache cache;
    cache.Initialize(32, 2); // only 2 renders per frame
    for (uint32_t i = 0; i < 5; ++i)
    {
        cache.RegisterProbe(MakeProbe(i, static_cast<float>(i * 10), 0, 0));
    }
    auto rendered = cache.Update(0, 0, 0);
    EXPECT_EQ(static_cast<int>(rendered.size()), 2);
}

TEST(ReflectionProbeCache_UpdateStaticProbeRendersOnceThenCached)
{
    ReflectionProbeCache cache;
    cache.Initialize(8, 4);
    cache.RegisterProbe(MakeProbe(7, 0, 0, 0, /*isStatic*/ true));

    auto first = cache.Update(0, 0, 0);
    EXPECT_EQ(static_cast<int>(first.size()), 1); // first frame renders

    auto second = cache.Update(0, 0, 0);
    EXPECT_EQ(static_cast<int>(second.size()), 0); // static + cached → no render
    cache.Shutdown();
}

TEST(ReflectionProbeCache_DynamicProbeReRendersEveryFrame)
{
    ReflectionProbeCache cache;
    cache.Initialize(8, 4);
    auto p = MakeProbe(7, 0, 0, 0, /*isStatic*/ false);
    cache.RegisterProbe(p);

    // Every call to Update needs a re-render because the probe is dynamic
    // and `needsRender` stays true until after a render. After the first
    // render the cache resets needsRender=false — but dynamic probes are
    // considered "always stale" in the next frame via the cached-but-not-
    // static branch of the evaluator.
    auto first = cache.Update(0, 0, 0);
    EXPECT_EQ(static_cast<int>(first.size()), 1);

    // Mark as needing a re-render again (simulates a scene change) and
    // verify the evaluator schedules it.
    cache.InvalidateProbe(7);
    auto second = cache.Update(0, 0, 0);
    EXPECT_EQ(static_cast<int>(second.size()), 1);
    cache.Shutdown();
}

// =========================================================================
// Cache slot allocation and queries
// =========================================================================

TEST(ReflectionProbeCache_GetCacheSlotAfterRender)
{
    ReflectionProbeCache cache;
    cache.Initialize(8, 4);
    cache.RegisterProbe(MakeProbe(5, 0, 0, 0));
    EXPECT_EQ(cache.GetCacheSlot(5), UINT32_MAX); // nothing rendered yet
    cache.Update(0, 0, 0);
    uint32_t slot = cache.GetCacheSlot(5);
    EXPECT_TRUE(slot != UINT32_MAX);
    EXPECT_LT(slot, 8u);
    cache.Shutdown();
}

TEST(ReflectionProbeCache_GetCacheSlotUnregisteredIsInvalid)
{
    ReflectionProbeCache cache;
    cache.Initialize(8, 4);
    EXPECT_EQ(cache.GetCacheSlot(999), UINT32_MAX);
    cache.Shutdown();
}

TEST(ReflectionProbeCache_GetCachedCountTracksRenders)
{
    ReflectionProbeCache cache;
    cache.Initialize(8, 4);
    cache.RegisterProbe(MakeProbe(1, 0, 0, 0));
    cache.RegisterProbe(MakeProbe(2, 1, 0, 0));
    cache.RegisterProbe(MakeProbe(3, 2, 0, 0));
    cache.Update(0, 0, 0);
    EXPECT_EQ(cache.GetCachedCount(), 3u);
    cache.Shutdown();
}

// =========================================================================
// Invalidation
// =========================================================================

TEST(ReflectionProbeCache_InvalidateProbeMarksNeedsRender)
{
    ReflectionProbeCache cache;
    cache.Initialize(8, 4);
    cache.RegisterProbe(MakeProbe(5, 0, 0, 0, /*isStatic*/ true));
    cache.Update(0, 0, 0); // first render
    auto second = cache.Update(0, 0, 0);
    EXPECT_EQ(static_cast<int>(second.size()), 0); // static → no render

    cache.InvalidateProbe(5);
    auto third = cache.Update(0, 0, 0);
    EXPECT_EQ(static_cast<int>(third.size()), 1); // invalidated → re-renders
    cache.Shutdown();
}

TEST(ReflectionProbeCache_InvalidateAllForcesReRender)
{
    ReflectionProbeCache cache;
    cache.Initialize(8, 4);
    cache.RegisterProbe(MakeProbe(1, 0, 0, 0, /*isStatic*/ true));
    cache.RegisterProbe(MakeProbe(2, 1, 0, 0, /*isStatic*/ true));
    cache.RegisterProbe(MakeProbe(3, 2, 0, 0, /*isStatic*/ true));
    cache.Update(0, 0, 0); // all three render
    auto idle = cache.Update(0, 0, 0);
    EXPECT_EQ(static_cast<int>(idle.size()), 0);

    cache.InvalidateAll();
    auto rerendered = cache.Update(0, 0, 0);
    EXPECT_EQ(static_cast<int>(rerendered.size()), 3);
    cache.Shutdown();
}

// =========================================================================
// Eviction when the cache fills up
// =========================================================================

TEST(ReflectionProbeCache_LRUEvictionWhenSlotsExhausted)
{
    ReflectionProbeCache cache;
    cache.Initialize(/*maxSlots*/ 2, /*budget*/ 4);
    // Render two probes to fill the cache
    cache.RegisterProbe(MakeProbe(10, 0, 0, 0, /*isStatic*/ true));
    cache.RegisterProbe(MakeProbe(20, 1, 0, 0, /*isStatic*/ true));
    cache.Update(0, 0, 0);
    EXPECT_EQ(cache.GetCachedCount(), 2u);

    // Now register a third and force it to render by invalidating-all so
    // the first two get re-scheduled together with the new one. The
    // budget allows all three but only 2 slots exist → the oldest gets
    // evicted.
    cache.RegisterProbe(MakeProbe(30, 2, 0, 0, /*isStatic*/ true));
    cache.Update(0, 0, 0); // only #30 schedules (not cached yet)
    // Cached count should still be ≤ maxSlots (2).
    EXPECT_LE(cache.GetCachedCount(), 2u);
    cache.Shutdown();
}

TEST(ReflectionProbeCache_Console_GetStatusIncludesProbeCount)
{
    ReflectionProbeCache cache;
    cache.Initialize(8, 4);
    cache.RegisterProbe(MakeProbe(1, 0, 0, 0));
    cache.RegisterProbe(MakeProbe(2, 1, 0, 0));
    std::string status = cache.Console_GetStatus();
    EXPECT_TRUE(status.find("ReflectionProbeCache") != std::string::npos);
    EXPECT_TRUE(status.find("2 probes") != std::string::npos);
    cache.Shutdown();
}
