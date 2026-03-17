/**
 * @file TestShadowAtlas.cpp
 * @brief Unit tests for the ShadowAtlas tile allocation system
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Graphics/ShadowAtlas.h"

// ============================================================================
// Initialization
// ============================================================================

TEST(ShadowAtlas_Initialize)
{
    Spark::Graphics::ShadowAtlas atlas;
    EXPECT_TRUE(atlas.Initialize(4096, 256));
    EXPECT_TRUE(atlas.IsInitialized());
    EXPECT_EQ(atlas.GetAtlasSize(), 4096u);
    atlas.Shutdown();
    EXPECT_FALSE(atlas.IsInitialized());
}

// ============================================================================
// Tile Allocation
// ============================================================================

TEST(ShadowAtlas_AllocateTile)
{
    Spark::Graphics::ShadowAtlas atlas;
    atlas.Initialize(4096, 256);
    atlas.BeginFrame();

    EXPECT_TRUE(atlas.RequestTile(1, 1.0f, 1024));
    const auto* tile = atlas.GetTile(1);
    EXPECT_TRUE(tile != nullptr);
    EXPECT_EQ(tile->size, 1024u);
    EXPECT_TRUE(tile->active);

    atlas.EndFrame();
    atlas.Shutdown();
}

TEST(ShadowAtlas_MultipleTiles)
{
    Spark::Graphics::ShadowAtlas atlas;
    atlas.Initialize(4096, 256);
    atlas.BeginFrame();

    EXPECT_TRUE(atlas.RequestTile(1, 1.0f, 1024));
    EXPECT_TRUE(atlas.RequestTile(2, 0.8f, 512));
    EXPECT_TRUE(atlas.RequestTile(3, 0.5f, 256));

    EXPECT_TRUE(atlas.GetTile(1) != nullptr);
    EXPECT_TRUE(atlas.GetTile(2) != nullptr);
    EXPECT_TRUE(atlas.GetTile(3) != nullptr);

    // Tiles should not overlap
    const auto* t1 = atlas.GetTile(1);
    const auto* t2 = atlas.GetTile(2);
    bool overlaps = (t1->x < t2->x + t2->size && t1->x + t1->size > t2->x && t1->y < t2->y + t2->size &&
                     t1->y + t1->size > t2->y);
    EXPECT_FALSE(overlaps);

    atlas.EndFrame();
    atlas.Shutdown();
}

TEST(ShadowAtlas_ReactivateTile)
{
    Spark::Graphics::ShadowAtlas atlas;
    atlas.Initialize(4096, 256);
    atlas.BeginFrame();

    EXPECT_TRUE(atlas.RequestTile(1, 1.0f, 1024));
    atlas.EndFrame();

    // Next frame: re-request same tile
    atlas.BeginFrame();
    EXPECT_TRUE(atlas.RequestTile(1, 1.0f, 1024));
    const auto* tile = atlas.GetTile(1);
    EXPECT_TRUE(tile->active);

    atlas.EndFrame();
    atlas.Shutdown();
}

// ============================================================================
// Metrics
// ============================================================================

TEST(ShadowAtlas_Metrics)
{
    Spark::Graphics::ShadowAtlas atlas;
    atlas.Initialize(2048, 256);
    atlas.BeginFrame();

    atlas.RequestTile(1, 1.0f, 512);
    atlas.RequestTile(2, 0.8f, 512);

    auto metrics = atlas.GetMetrics();
    EXPECT_EQ(metrics.atlasSize, 2048u);
    EXPECT_EQ(metrics.activeTiles, 2u);
    EXPECT_TRUE(metrics.utilization > 0.0f);

    atlas.EndFrame();
    atlas.Shutdown();
}

TEST(ShadowAtlas_ConsoleStatus)
{
    Spark::Graphics::ShadowAtlas atlas;
    atlas.Initialize(4096, 256);
    atlas.BeginFrame();
    atlas.RequestTile(1, 1.0f, 1024);

    std::string status = atlas.Console_GetStatus();
    EXPECT_TRUE(status.find("ShadowAtlas") != std::string::npos);
    EXPECT_TRUE(status.find("4096") != std::string::npos);

    atlas.EndFrame();
    atlas.Shutdown();
}

TEST(ShadowAtlas_NonexistentTile)
{
    Spark::Graphics::ShadowAtlas atlas;
    atlas.Initialize(4096, 256);
    EXPECT_TRUE(atlas.GetTile(999) == nullptr);
    atlas.Shutdown();
}
