/**
 * @file TestCachedShadowAtlas.cpp
 * @brief Phase M — CPU reference tests for CachedShadowAtlas
 *
 * Exercises the header-only `Spark::Graphics::CachedShadowAtlas` class
 * directly. The atlas owns two `ShadowAtlas` sub-atlases (dynamic +
 * cached) and decides which lights need re-rendering each frame based
 * on a state hash. Tests cover the lifecycle, request routing,
 * cache-hit detection, invalidation, and the per-frame render list.
 *
 * The underlying `ShadowAtlas` depends only on portable types, so
 * these tests run on every platform's CI.
 */

#include "TestFramework.h"

#include "../SparkEngine/Source/Graphics/CachedShadowAtlas.h"

using Spark::Graphics::CachedShadowAtlas;
using Spark::Graphics::ShadowUpdateRequest;

namespace
{
    ShadowUpdateRequest MakeRequest(uint32_t lightId, bool isStatic = true, float priority = 0.5f, float x = 0,
                                    float y = 0, float z = 0)
    {
        ShadowUpdateRequest r;
        r.lightId = lightId;
        r.isStatic = isStatic;
        r.priority = priority;
        r.posX = x;
        r.posY = y;
        r.posZ = z;
        r.dirX = 0;
        r.dirY = -1;
        r.dirZ = 0;
        r.range = 20.0f;
        r.spotAngle = 0.0f;
        r.forceUpdate = false;
        return r;
    }
} // namespace

// =========================================================================
// Lifecycle
// =========================================================================

TEST(CachedShadowAtlas_InitializeAndShutdown)
{
    CachedShadowAtlas atlas;
    EXPECT_FALSE(atlas.IsInitialized());
    EXPECT_TRUE(atlas.Initialize(1024, 2048, 128));
    EXPECT_TRUE(atlas.IsInitialized());
    atlas.Shutdown();
    EXPECT_FALSE(atlas.IsInitialized());
}

TEST(CachedShadowAtlas_DefaultInitializeValues)
{
    CachedShadowAtlas atlas;
    EXPECT_TRUE(atlas.Initialize());
    EXPECT_TRUE(atlas.IsInitialized());
    EXPECT_EQ(atlas.GetCachedRendersAvoided(), 0u);
    atlas.Shutdown();
}

TEST(CachedShadowAtlas_RequestShadowBeforeInitializeIsRejected)
{
    CachedShadowAtlas atlas;
    auto req = MakeRequest(1);
    EXPECT_FALSE(atlas.RequestShadow(req));
}

// =========================================================================
// Request routing
// =========================================================================

TEST(CachedShadowAtlas_FirstRequestSchedulesRender)
{
    CachedShadowAtlas atlas;
    atlas.Initialize(1024, 2048, 128);
    atlas.BeginFrame();
    EXPECT_TRUE(atlas.RequestShadow(MakeRequest(1, /*isStatic*/ true)));
    EXPECT_EQ(static_cast<int>(atlas.GetShadowsToRender().size()), 1);
    EXPECT_EQ(atlas.GetShadowsToRender()[0], 1u);
    atlas.EndFrame();
    atlas.Shutdown();
}

TEST(CachedShadowAtlas_MultipleLightsAllRenderOnFirstFrame)
{
    CachedShadowAtlas atlas;
    atlas.Initialize(1024, 2048, 128);
    atlas.BeginFrame();
    EXPECT_TRUE(atlas.RequestShadow(MakeRequest(1)));
    EXPECT_TRUE(atlas.RequestShadow(MakeRequest(2)));
    EXPECT_TRUE(atlas.RequestShadow(MakeRequest(3)));
    EXPECT_EQ(static_cast<int>(atlas.GetShadowsToRender().size()), 3);
    atlas.EndFrame();
    atlas.Shutdown();
}

// =========================================================================
// Caching behaviour
// =========================================================================

TEST(CachedShadowAtlas_StaticLightCachesAfterFirstRender)
{
    CachedShadowAtlas atlas;
    atlas.Initialize(1024, 2048, 128);

    // Frame 1: request + render.
    atlas.BeginFrame();
    atlas.RequestShadow(MakeRequest(1, /*isStatic*/ true));
    EXPECT_EQ(static_cast<int>(atlas.GetShadowsToRender().size()), 1);
    atlas.MarkRendered(1);
    atlas.EndFrame();

    // Frame 2: same request with unchanged state — must be a cache hit.
    atlas.BeginFrame();
    atlas.RequestShadow(MakeRequest(1, /*isStatic*/ true));
    EXPECT_EQ(static_cast<int>(atlas.GetShadowsToRender().size()), 0);
    EXPECT_EQ(atlas.GetCachedRendersAvoided(), 1u);
    atlas.EndFrame();
    atlas.Shutdown();
}

TEST(CachedShadowAtlas_CacheMissWhenLightMoves)
{
    CachedShadowAtlas atlas;
    atlas.Initialize(1024, 2048, 128);

    // Frame 1: register + render at the origin.
    atlas.BeginFrame();
    atlas.RequestShadow(MakeRequest(5, /*isStatic*/ true, 0.5f, 0, 0, 0));
    atlas.MarkRendered(5);
    atlas.EndFrame();

    // Frame 2: same light, different position → hash mismatch → re-render.
    atlas.BeginFrame();
    atlas.RequestShadow(MakeRequest(5, /*isStatic*/ true, 0.5f, 10, 0, 0));
    EXPECT_EQ(static_cast<int>(atlas.GetShadowsToRender().size()), 1);
    atlas.EndFrame();
    atlas.Shutdown();
}

TEST(CachedShadowAtlas_ForceUpdateBypassesCache)
{
    CachedShadowAtlas atlas;
    atlas.Initialize(1024, 2048, 128);

    // Render once.
    atlas.BeginFrame();
    atlas.RequestShadow(MakeRequest(3, true));
    atlas.MarkRendered(3);
    atlas.EndFrame();

    // Force-update request → no cache reuse.
    atlas.BeginFrame();
    auto req = MakeRequest(3, true);
    req.forceUpdate = true;
    atlas.RequestShadow(req);
    EXPECT_EQ(static_cast<int>(atlas.GetShadowsToRender().size()), 1);
    atlas.EndFrame();
    atlas.Shutdown();
}

TEST(CachedShadowAtlas_DynamicLightAlwaysRendersOnStateChange)
{
    CachedShadowAtlas atlas;
    atlas.Initialize(1024, 2048, 128);

    atlas.BeginFrame();
    atlas.RequestShadow(MakeRequest(8, /*isStatic*/ false, 0.5f, 0, 0, 0));
    EXPECT_EQ(static_cast<int>(atlas.GetShadowsToRender().size()), 1);
    atlas.MarkRendered(8);
    atlas.EndFrame();

    // Dynamic light moves.
    atlas.BeginFrame();
    atlas.RequestShadow(MakeRequest(8, /*isStatic*/ false, 0.5f, 5, 0, 0));
    EXPECT_EQ(static_cast<int>(atlas.GetShadowsToRender().size()), 1);
    atlas.EndFrame();
    atlas.Shutdown();
}

// =========================================================================
// Invalidation
// =========================================================================

TEST(CachedShadowAtlas_InvalidateShadowForcesReRender)
{
    CachedShadowAtlas atlas;
    atlas.Initialize(1024, 2048, 128);

    atlas.BeginFrame();
    atlas.RequestShadow(MakeRequest(11, true));
    atlas.MarkRendered(11);
    atlas.EndFrame();

    // Still static + unchanged → cache hit on frame 2.
    atlas.BeginFrame();
    atlas.RequestShadow(MakeRequest(11, true));
    EXPECT_EQ(atlas.GetCachedRendersAvoided(), 1u);
    atlas.EndFrame();

    // Invalidate → re-render on frame 3.
    atlas.InvalidateShadow(11);
    atlas.BeginFrame();
    atlas.RequestShadow(MakeRequest(11, true));
    EXPECT_EQ(static_cast<int>(atlas.GetShadowsToRender().size()), 1);
    atlas.EndFrame();
    atlas.Shutdown();
}

TEST(CachedShadowAtlas_InvalidateAllMarksEveryLight)
{
    CachedShadowAtlas atlas;
    atlas.Initialize(1024, 2048, 128);

    atlas.BeginFrame();
    atlas.RequestShadow(MakeRequest(1, true));
    atlas.RequestShadow(MakeRequest(2, true));
    atlas.RequestShadow(MakeRequest(3, true));
    atlas.MarkRendered(1);
    atlas.MarkRendered(2);
    atlas.MarkRendered(3);
    atlas.EndFrame();

    atlas.InvalidateAll();

    atlas.BeginFrame();
    atlas.RequestShadow(MakeRequest(1, true));
    atlas.RequestShadow(MakeRequest(2, true));
    atlas.RequestShadow(MakeRequest(3, true));
    EXPECT_EQ(static_cast<int>(atlas.GetShadowsToRender().size()), 3);
    atlas.EndFrame();
    atlas.Shutdown();
}

// =========================================================================
// Metrics
// =========================================================================

TEST(CachedShadowAtlas_Console_GetStatusFormatting)
{
    CachedShadowAtlas atlas;
    atlas.Initialize(1024, 2048, 128);
    atlas.BeginFrame();
    atlas.RequestShadow(MakeRequest(1, true));
    atlas.EndFrame();

    std::string status = atlas.Console_GetStatus();
    EXPECT_TRUE(status.find("CachedShadowAtlas") != std::string::npos);
    EXPECT_TRUE(status.find("Dynamic") != std::string::npos);
    EXPECT_TRUE(status.find("Cached") != std::string::npos);
    atlas.Shutdown();
}

TEST(CachedShadowAtlas_GetTileRoutingByStaticFlag)
{
    CachedShadowAtlas atlas;
    atlas.Initialize(1024, 2048, 128);

    atlas.BeginFrame();
    atlas.RequestShadow(MakeRequest(1, /*isStatic*/ true));  // → cached atlas
    atlas.RequestShadow(MakeRequest(2, /*isStatic*/ false)); // → dynamic atlas
    atlas.EndFrame();

    // Both lookups must return non-null tiles because GetTile falls
    // through to either sub-atlas.
    EXPECT_TRUE(atlas.GetTile(1) != nullptr);
    EXPECT_TRUE(atlas.GetTile(2) != nullptr);
    atlas.Shutdown();
}
