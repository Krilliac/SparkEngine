// TestProceduralGenerator.cpp - Tests for Spark::Procedural::ProceduralGenerator
#include "TestFramework.h"
#include "Engine/Procedural/ProceduralGenerator.h"

// ============================================================================
// Initialization
// ============================================================================

TEST(ProceduralGenerator_Initialize)
{
    auto& gen = Spark::Procedural::ProceduralGenerator::GetInstance();
    gen.Initialize(42);
    EXPECT_EQ(gen.GetSeed(), 42u);
    gen.Shutdown();
}

TEST(ProceduralGenerator_SetSeed)
{
    auto& gen = Spark::Procedural::ProceduralGenerator::GetInstance();
    gen.Initialize(0);
    gen.SetSeed(12345);
    EXPECT_EQ(gen.GetSeed(), 12345u);
    gen.Shutdown();
}

// ============================================================================
// Heightmap generation
// ============================================================================

TEST(ProceduralGenerator_Heightmap_CorrectSize)
{
    auto& gen = Spark::Procedural::ProceduralGenerator::GetInstance();
    gen.Initialize(42);

    auto heightmap = gen.GenerateHeightmap(64, 32);
    EXPECT_EQ(heightmap.size(), static_cast<size_t>(64 * 32));
    gen.Shutdown();
}

TEST(ProceduralGenerator_Heightmap_NormalizedRange)
{
    auto& gen = Spark::Procedural::ProceduralGenerator::GetInstance();
    gen.Initialize(42);

    auto heightmap = gen.GenerateHeightmap(32, 32);
    float minVal = *std::min_element(heightmap.begin(), heightmap.end());
    float maxVal = *std::max_element(heightmap.begin(), heightmap.end());

    EXPECT_GE(minVal, 0.0f);
    EXPECT_LE(maxVal, 1.0f);
    // Should have some variation (not all same value)
    EXPECT_GT(maxVal - minVal, 0.01f);
    gen.Shutdown();
}

TEST(ProceduralGenerator_Heightmap_Deterministic)
{
    auto& gen = Spark::Procedural::ProceduralGenerator::GetInstance();

    gen.Initialize(42);
    auto h1 = gen.GenerateHeightmap(16, 16);

    gen.Initialize(42);
    auto h2 = gen.GenerateHeightmap(16, 16);

    EXPECT_EQ(h1.size(), h2.size());
    for (size_t i = 0; i < h1.size(); ++i)
        EXPECT_NEAR(h1[i], h2[i], 1e-6f);

    gen.Shutdown();
}

TEST(ProceduralGenerator_Heightmap_DifferentSeeds)
{
    auto& gen = Spark::Procedural::ProceduralGenerator::GetInstance();

    gen.Initialize(42);
    auto h1 = gen.GenerateHeightmap(16, 16);

    gen.Initialize(99);
    auto h2 = gen.GenerateHeightmap(16, 16);

    // At least some values should differ
    bool anyDiff = false;
    for (size_t i = 0; i < h1.size(); ++i)
    {
        if (std::abs(h1[i] - h2[i]) > 0.01f)
        {
            anyDiff = true;
            break;
        }
    }
    EXPECT_TRUE(anyDiff);
    gen.Shutdown();
}

// ============================================================================
// Dungeon generation
// ============================================================================

TEST(ProceduralGenerator_Dungeon_CorrectSize)
{
    auto& gen = Spark::Procedural::ProceduralGenerator::GetInstance();
    gen.Initialize(42);

    auto dungeon = gen.GenerateDungeon(50, 50);
    EXPECT_EQ(dungeon.width, 50);
    EXPECT_EQ(dungeon.height, 50);
    EXPECT_EQ(dungeon.tiles.size(), static_cast<size_t>(50 * 50));
    gen.Shutdown();
}

TEST(ProceduralGenerator_Dungeon_HasRooms)
{
    auto& gen = Spark::Procedural::ProceduralGenerator::GetInstance();
    gen.Initialize(42);

    Spark::Procedural::DungeonParams params;
    params.maxRooms = 5;
    auto dungeon = gen.GenerateDungeon(50, 50, params);

    EXPECT_GT(dungeon.rooms.size(), static_cast<size_t>(0));
    EXPECT_LE(dungeon.rooms.size(), static_cast<size_t>(5));
    gen.Shutdown();
}

TEST(ProceduralGenerator_Dungeon_HasFloorTiles)
{
    auto& gen = Spark::Procedural::ProceduralGenerator::GetInstance();
    gen.Initialize(42);

    auto dungeon = gen.GenerateDungeon(50, 50);

    int floorCount = 0;
    for (auto tile : dungeon.tiles)
    {
        if (tile == Spark::Procedural::DungeonTile::Floor || tile == Spark::Procedural::DungeonTile::Corridor)
            ++floorCount;
    }
    EXPECT_GT(floorCount, 0);
    gen.Shutdown();
}

TEST(ProceduralGenerator_Dungeon_RoomsDoNotOverlap)
{
    auto& gen = Spark::Procedural::ProceduralGenerator::GetInstance();
    gen.Initialize(42);

    auto dungeon = gen.GenerateDungeon(80, 80);

    for (size_t i = 0; i < dungeon.rooms.size(); ++i)
    {
        for (size_t j = i + 1; j < dungeon.rooms.size(); ++j)
        {
            EXPECT_FALSE(dungeon.rooms[i].Overlaps(dungeon.rooms[j], 2));
        }
    }
    gen.Shutdown();
}

// ============================================================================
// Wave Function Collapse
// ============================================================================

TEST(ProceduralGenerator_WFC_EmptyRules)
{
    auto& gen = Spark::Procedural::ProceduralGenerator::GetInstance();
    gen.Initialize(42);

    std::vector<Spark::Procedural::WFCTileRule> rules;
    auto result = gen.GenerateWFC(4, 4, rules);
    EXPECT_EQ(result.width, 4);
    EXPECT_EQ(result.height, 4);
    // All tiles should be -1 (unresolved) with no rules
    for (auto t : result.tiles)
        EXPECT_EQ(t, -1);
    gen.Shutdown();
}

TEST(ProceduralGenerator_WFC_SingleTile)
{
    auto& gen = Spark::Procedural::ProceduralGenerator::GetInstance();
    gen.Initialize(42);

    // One tile type that allows itself everywhere
    Spark::Procedural::WFCTileRule rule;
    rule.tileId = 1;
    rule.weight = 1.0f;
    rule.allowedRight = {1};
    rule.allowedLeft = {1};
    rule.allowedUp = {1};
    rule.allowedDown = {1};

    auto result = gen.GenerateWFC(4, 4, {rule});
    EXPECT_TRUE(result.success);
    for (auto t : result.tiles)
        EXPECT_EQ(t, 1);
    gen.Shutdown();
}

TEST(ProceduralGenerator_ConsoleStatus)
{
    auto& gen = Spark::Procedural::ProceduralGenerator::GetInstance();
    gen.Initialize(42);
    std::string status = gen.Console_GetStatus();
    EXPECT_TRUE(status.find("seed=42") != std::string::npos);
    gen.Shutdown();
}
