/**
 * @file TestOpenWorldModule.cpp
 * @brief Tests for SparkGameOpenWorld module data structures and enums
 *
 * Note: Full method tests require the OpenWorld .cpp files which depend on ImGui.
 * These tests verify the data structures, default values, and enum coverage.
 */

#include "TestFramework.h"

#include "../GameModules/SparkGameOpenWorld/Source/Player/OWPlayerSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/Exploration/OWExplorationSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/Gathering/OWGatheringSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/Wildlife/OWWildlifeSystem.h"

using namespace OpenWorld;

// =============================================================================
// SurvivalState
// =============================================================================

TEST(OW_SurvivalState_Defaults)
{
    SurvivalState s;
    EXPECT_NEAR(s.health, 100.0f, 0.1f);
    EXPECT_NEAR(s.maxHealth, 100.0f, 0.1f);
    EXPECT_NEAR(s.stamina, 100.0f, 0.1f);
    EXPECT_NEAR(s.maxStamina, 100.0f, 0.1f);
    EXPECT_NEAR(s.hunger, 100.0f, 0.1f);
    EXPECT_NEAR(s.thirst, 100.0f, 0.1f);
}

TEST(OW_PlayerWorldState_Defaults)
{
    PlayerWorldState ws;
    EXPECT_NEAR(ws.posX, 0.0f, 0.01f);
    EXPECT_NEAR(ws.posY, 0.0f, 0.01f);
    EXPECT_NEAR(ws.posZ, 0.0f, 0.01f);
    EXPECT_FALSE(ws.isSprinting);
    EXPECT_FALSE(ws.isSwimming);
    EXPECT_FALSE(ws.isClimbing);
    EXPECT_FALSE(ws.isInShelter);
}

TEST(OW_FastTravelPoint_Fields)
{
    FastTravelPoint fp;
    fp.pointId = 42;
    fp.name = "Village";
    fp.x = 100.0f;
    fp.y = 50.0f;
    fp.z = 200.0f;
    fp.regionId = 3;
    EXPECT_EQ(fp.pointId, 42u);
    EXPECT_EQ(fp.name, std::string("Village"));
    EXPECT_NEAR(fp.x, 100.0f, 0.01f);
}

// =============================================================================
// Exploration
// =============================================================================

TEST(OW_PointOfInterest_Fields)
{
    PointOfInterest poi;
    poi.poiId = 1;
    poi.name = "Ancient Ruins";
    poi.description = "Mysterious ruins";
    poi.x = 500.0f;
    poi.y = 10.0f;
    poi.z = 300.0f;
    poi.discoveryRadius = 50.0f;
    poi.regionId = 2;
    poi.xpReward = 100;
    poi.hasSecret = true;
    EXPECT_EQ(poi.poiId, 1u);
    EXPECT_EQ(poi.name, std::string("Ancient Ruins"));
    EXPECT_TRUE(poi.hasSecret);
    EXPECT_EQ(poi.xpReward, 100u);
}

TEST(OW_POIProgress_Defaults)
{
    POIProgress prog;
    prog.poiId = 5;
    prog.state = DiscoveryState::Undiscovered;
    prog.secretFound = false;
    EXPECT_EQ(prog.poiId, 5u);
    EXPECT_FALSE(prog.secretFound);
}

TEST(OW_RegionExploration_Fields)
{
    RegionExploration re;
    re.regionId = 1;
    re.totalPOIs = 10;
    re.discoveredPOIs = 3;
    re.completedPOIs = 1;
    re.completionPercent = 30.0f;
    EXPECT_EQ(re.totalPOIs, 10u);
    EXPECT_NEAR(re.completionPercent, 30.0f, 0.1f);
}

// =============================================================================
// Gathering
// =============================================================================

TEST(OW_ResourceNode_Fields)
{
    ResourceNode node;
    node.nodeId = 1;
    node.name = "Iron Vein";
    node.resource = ResourceType::Iron;
    node.maxYield = 10;
    node.currentYield = 10;
    node.respawnTime = 60.0f;
    node.isDepleted = false;
    EXPECT_EQ(node.nodeId, 1u);
    EXPECT_EQ(node.name, std::string("Iron Vein"));
    EXPECT_FALSE(node.isDepleted);
}

TEST(OW_ResourceInventory_BasicOps)
{
    ResourceInventory inv;
    EXPECT_EQ(inv.Get(ResourceType::Wood), 0u);

    inv.Add(ResourceType::Wood, 10);
    EXPECT_EQ(inv.Get(ResourceType::Wood), 10u);

    inv.Add(ResourceType::Wood, 5);
    EXPECT_EQ(inv.Get(ResourceType::Wood), 15u);

    EXPECT_TRUE(inv.Spend(ResourceType::Wood, 7));
    EXPECT_EQ(inv.Get(ResourceType::Wood), 8u);

    EXPECT_FALSE(inv.Spend(ResourceType::Wood, 100)); // not enough
    EXPECT_EQ(inv.Get(ResourceType::Wood), 8u);       // unchanged
}

TEST(OW_CraftingRecipe_Fields)
{
    CraftingRecipe recipe;
    recipe.recipeId = 1;
    recipe.resultName = "Iron Sword";
    recipe.description = "A basic iron sword";
    recipe.craftTime = 5.0f;
    recipe.requiresCraftingStation = true;
    EXPECT_EQ(recipe.recipeId, 1u);
    EXPECT_TRUE(recipe.requiresCraftingStation);
    EXPECT_NEAR(recipe.craftTime, 5.0f, 0.01f);
}

// =============================================================================
// Wildlife
// =============================================================================

TEST(OW_AnimalSpecies_Fields)
{
    AnimalSpecies species;
    species.name = "Deer";
    species.diet = DietType::Herbivore;
    species.health = 50.0f;
    species.speed = 8.0f;
    species.detectionRange = 30.0f;
    species.isTameable = true;
    species.formsHerds = true;
    species.herdSizeMin = 3;
    species.herdSizeMax = 8;
    EXPECT_EQ(species.name, std::string("Deer"));
    EXPECT_TRUE(species.isTameable);
    EXPECT_TRUE(species.formsHerds);
    EXPECT_NEAR(species.health, 50.0f, 0.1f);
}

TEST(OW_AnimalInstance_Fields)
{
    AnimalInstance inst;
    inst.instanceId = 42;
    inst.posX = 100.0f;
    inst.posY = 0.0f;
    inst.posZ = 200.0f;
    inst.health = 50.0f;
    inst.behavior = AnimalBehavior::Grazing;
    inst.isTamed = false;
    inst.isAlive = true;
    EXPECT_EQ(inst.instanceId, 42u);
    EXPECT_TRUE(inst.isAlive);
    EXPECT_FALSE(inst.isTamed);
}

TEST(OW_Herd_Fields)
{
    Herd herd;
    herd.herdId = 1;
    herd.type = AnimalType::Deer;
    herd.memberIds = {10, 11, 12};
    herd.centerX = 500.0f;
    herd.centerZ = 300.0f;
    herd.regionId = 2;
    EXPECT_EQ(herd.herdId, 1u);
    EXPECT_EQ(herd.memberIds.size(), 3u);
}

// =============================================================================
// Enum coverage
// =============================================================================

TEST(OW_EnumCoverage_ResourceTypes)
{
    // Verify key resource types exist
    ResourceType wood = ResourceType::Wood;
    ResourceType stone = ResourceType::Stone;
    ResourceType iron = ResourceType::Iron;
    EXPECT_TRUE(wood != stone);
    EXPECT_TRUE(stone != iron);
    (void)wood;
    (void)stone;
    (void)iron;
}

TEST(OW_EnumCoverage_AnimalBehaviors)
{
    AnimalBehavior roaming = AnimalBehavior::Roaming;
    AnimalBehavior grazing = AnimalBehavior::Grazing;
    AnimalBehavior fleeing = AnimalBehavior::Fleeing;
    EXPECT_TRUE(roaming != grazing);
    EXPECT_TRUE(grazing != fleeing);
    (void)roaming;
    (void)grazing;
    (void)fleeing;
}

TEST(OW_EnumCoverage_Biomes)
{
    Biome forest = Biome::Forest;
    Biome desert = Biome::Desert;
    Biome tundra = Biome::Tundra;
    EXPECT_TRUE(forest != desert);
    EXPECT_TRUE(desert != tundra);
    (void)forest;
    (void)desert;
    (void)tundra;
}
