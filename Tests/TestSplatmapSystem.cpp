/**
 * @file TestSplatmapSystem.cpp
 * @brief Tests for Spark::Procedural::SplatmapSystem
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Engine/Procedural/SplatmapSystem.h"

using namespace Spark::Procedural;

TEST(Splatmap_Creation)
{
    Splatmap splatmap(64, 64, 4);
    EXPECT_EQ(splatmap.GetWidth(), 64u);
    EXPECT_EQ(splatmap.GetHeight(), 64u);
    EXPECT_EQ(splatmap.GetLayerCount(), 4);
}

TEST(Splatmap_SetGetWeight)
{
    Splatmap splatmap(32, 32, 3);

    splatmap.SetWeight(10, 15, 0, 0.7f);
    splatmap.SetWeight(10, 15, 1, 0.2f);
    splatmap.SetWeight(10, 15, 2, 0.1f);

    EXPECT_NEAR(splatmap.GetWeight(10, 15, 0), 0.7f, 0.001f);
    EXPECT_NEAR(splatmap.GetWeight(10, 15, 1), 0.2f, 0.001f);
    EXPECT_NEAR(splatmap.GetWeight(10, 15, 2), 0.1f, 0.001f);
}

TEST(Splatmap_OutOfBoundsReturnsZero)
{
    Splatmap splatmap(16, 16, 2);
    EXPECT_NEAR(splatmap.GetWeight(100, 100, 0), 0.0f, 0.001f);
    EXPECT_NEAR(splatmap.GetWeight(0, 0, 5), 0.0f, 0.001f); // Invalid layer
}

TEST(Splatmap_Normalize)
{
    Splatmap splatmap(4, 4, 3);

    // Set weights that don't sum to 1
    splatmap.SetWeight(0, 0, 0, 0.5f);
    splatmap.SetWeight(0, 0, 1, 0.3f);
    splatmap.SetWeight(0, 0, 2, 0.7f);

    splatmap.Normalize();

    float sum = splatmap.GetWeight(0, 0, 0) + splatmap.GetWeight(0, 0, 1) + splatmap.GetWeight(0, 0, 2);
    EXPECT_NEAR(sum, 1.0f, 0.001f);
}

TEST(Splatmap_NormalizeZeroWeights)
{
    Splatmap splatmap(4, 4, 2);

    // All weights are 0 — should assign to first layer
    splatmap.Normalize();
    EXPECT_NEAR(splatmap.GetWeight(0, 0, 0), 1.0f, 0.001f);
}

TEST(Splatmap_PaintBrush)
{
    Splatmap splatmap(64, 64, 3);

    // Initialize all to layer 0
    for (uint32_t y = 0; y < 64; ++y)
    {
        for (uint32_t x = 0; x < 64; ++x)
        {
            splatmap.SetWeight(x, y, 0, 1.0f);
        }
    }

    // Paint layer 1 at center
    splatmap.PaintBrush(32.0f, 32.0f, 8.0f, 1, 0.8f, 0.5f);

    // Center should have significant layer 1 weight
    float centerWeight = splatmap.GetWeight(32, 32, 1);
    EXPECT_GT(centerWeight, 0.3f);

    // Far corner should be unaffected
    float cornerWeight = splatmap.GetWeight(0, 0, 1);
    EXPECT_NEAR(cornerWeight, 0.0f, 0.001f);

    // Weights at center should still sum to ~1
    float sum = splatmap.GetWeight(32, 32, 0) + splatmap.GetWeight(32, 32, 1) + splatmap.GetWeight(32, 32, 2);
    EXPECT_NEAR(sum, 1.0f, 0.02f);
}

TEST(Splatmap_MaxLayers)
{
    Splatmap splatmap(8, 8, Splatmap::MaxLayers + 5); // Request more than max
    EXPECT_LE(splatmap.GetLayerCount(), Splatmap::MaxLayers);
}

TEST(SplatmapGen_BasicGeneration)
{
    // Create a simple heightmap — ramp from 0 to 1
    const uint32_t size = 32;
    std::vector<float> heightmap(size * size);
    for (uint32_t y = 0; y < size; ++y)
    {
        for (uint32_t x = 0; x < size; ++x)
        {
            heightmap[y * size + x] = static_cast<float>(y) / static_cast<float>(size - 1);
        }
    }

    SplatmapGenerator gen;
    gen.SetResolution(32, 32);
    gen.AddLayer({"Grass", "", "", 10.0f, 0.0f, 0.4f, 0.0f, 30.0f});
    gen.AddLayer({"Rock", "", "", 10.0f, 0.3f, 0.8f, 20.0f, 90.0f});
    gen.AddLayer({"Snow", "", "", 10.0f, 0.7f, 1.0f, 0.0f, 45.0f});

    Splatmap result = gen.Generate(heightmap.data(), size, size);

    EXPECT_EQ(result.GetWidth(), 32u);
    EXPECT_EQ(result.GetHeight(), 32u);
    EXPECT_EQ(result.GetLayerCount(), 3);

    // Bottom rows (low height) should have more grass
    float grassBottom = result.GetWeight(16, 2, 0);
    EXPECT_GT(grassBottom, 0.0f);

    // Top rows (high height) should have some snow
    float snowTop = result.GetWeight(16, 30, 2);
    EXPECT_GT(snowTop, 0.0f);
}

TEST(SplatmapGen_EmptyInput)
{
    SplatmapGenerator gen;
    gen.AddLayer({"Grass"});

    Splatmap result = gen.Generate(nullptr, 0, 0);
    EXPECT_EQ(result.GetWidth(), 0u);
}

TEST(Foliage_PlacerBasic)
{
    // Simple heightmap
    const uint32_t size = 32;
    std::vector<float> heightmap(size * size, 0.5f); // Flat terrain at 0.5

    // Simple splatmap — all grass
    Splatmap splatmap(size, size, 1);
    for (uint32_t y = 0; y < size; ++y)
    {
        for (uint32_t x = 0; x < size; ++x)
        {
            splatmap.SetWeight(x, y, 0, 1.0f);
        }
    }

    FoliagePlacer placer;
    FoliageType grass;
    grass.name = "Grass";
    grass.splatmapLayer = 0;
    grass.minWeight = 0.5f;
    grass.density = 0.5f;
    grass.minScale = 0.8f;
    grass.maxScale = 1.2f;
    placer.AddFoliageType(grass);

    auto instances = placer.GenerateInstances(splatmap, heightmap.data(), size, size, 100.0f, 10.0f);

    EXPECT_GT(static_cast<int>(instances.size()), 0);

    // All instances should be within terrain bounds (with jitter tolerance)
    for (const auto& inst : instances)
    {
        EXPECT_GE(inst.position.x, -2.0f);
        EXPECT_LE(inst.position.x, 102.0f);
        EXPECT_GE(inst.scale, 0.8f);
        EXPECT_LE(inst.scale, 1.2f);
    }
}
