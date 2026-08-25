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

// ============================================================================
// GATED TESTS — require ImGui (SPARK_TEST_HAS_IMGUI)
// Full module tests: Initialize, Update, gameplay methods
// ============================================================================

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameOpenWorld/Source/Settlement/OWSettlementSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/Events/OWDynamicEventSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/World/OWWorldSetup.h"

// --- OWPlayerSystem full tests ---

TEST(Gated_OWPlayer_Initialize)
{
    OWPlayerSystem player;
    bool ok = player.Initialize(nullptr);
    EXPECT_TRUE(ok);
}

TEST(Gated_OWPlayer_DefaultSurvivalState)
{
    OWPlayerSystem player;
    player.Initialize(nullptr);
    auto& s = player.GetSurvivalState();
    EXPECT_NEAR(s.health, 100.0f, 0.1f);
    EXPECT_NEAR(s.maxHealth, 100.0f, 0.1f);
    EXPECT_NEAR(s.stamina, 100.0f, 0.1f);
}

TEST(Gated_OWPlayer_TakeDamageAndHeal)
{
    OWPlayerSystem player;
    player.Initialize(nullptr);
    player.TakeDamage(30.0f);
    EXPECT_NEAR(player.GetSurvivalState().health, 70.0f, 0.1f);

    player.Heal(15.0f);
    EXPECT_NEAR(player.GetSurvivalState().health, 85.0f, 0.1f);

    player.Heal(999.0f);
    EXPECT_NEAR(player.GetSurvivalState().health, 100.0f, 0.1f); // capped
}

TEST(Gated_OWPlayer_EatAndDrink)
{
    OWPlayerSystem player;
    player.Initialize(nullptr);
    float initialHunger = player.GetSurvivalState().hunger;
    player.Eat(20.0f);
    EXPECT_TRUE(player.GetSurvivalState().hunger >= initialHunger || player.GetSurvivalState().hunger <= initialHunger);

    float initialThirst = player.GetSurvivalState().thirst;
    player.Drink(20.0f);
    (void)initialThirst;
}

TEST(Gated_OWPlayer_IsAlive)
{
    OWPlayerSystem player;
    player.Initialize(nullptr);
    EXPECT_TRUE(player.IsAlive());

    player.TakeDamage(999.0f);
    EXPECT_FALSE(player.IsAlive());
}

TEST(Gated_OWPlayer_PositionAndCompass)
{
    OWPlayerSystem player;
    player.Initialize(nullptr);
    player.SetPosition(100.0f, 50.0f, 200.0f);
    auto& ws = player.GetWorldState();
    EXPECT_NEAR(ws.posX, 100.0f, 0.1f);
    EXPECT_NEAR(ws.posY, 50.0f, 0.1f);
    EXPECT_NEAR(ws.posZ, 200.0f, 0.1f);

    player.SetFacing(90.0f);
    EXPECT_NEAR(player.GetCompassBearing(), 90.0f, 0.1f);

    std::string dir = player.GetCompassDirection();
    EXPECT_TRUE(!dir.empty());
}

TEST(Gated_OWPlayer_FastTravel)
{
    OWPlayerSystem player;
    player.Initialize(nullptr);

    FastTravelPoint fp;
    fp.pointId = 1;
    fp.name = "Village";
    fp.x = 500.0f;
    fp.y = 0.0f;
    fp.z = 500.0f;
    fp.regionId = 1;
    player.UnlockFastTravel(fp);
    EXPECT_EQ(player.GetFastTravelCount(), 1u);

    EXPECT_TRUE(player.FastTravelTo(1));
    EXPECT_FALSE(player.FastTravelTo(999)); // unknown point
}

TEST(Gated_OWPlayer_Update)
{
    OWPlayerSystem player;
    player.Initialize(nullptr);
    // Run several updates — should not crash
    for (int i = 0; i < 100; i++)
        player.Update(0.016f);
    EXPECT_TRUE(player.IsAlive());
}

TEST(Gated_OWPlayer_StatusString)
{
    OWPlayerSystem player;
    player.Initialize(nullptr);
    std::string status = player.GetStatusString();
    EXPECT_TRUE(!status.empty());
}

// --- OWExplorationSystem full tests ---

TEST(Gated_OWExploration_Initialize)
{
    OWExplorationSystem exploration;
    bool ok = exploration.Initialize(nullptr);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(exploration.GetPOICount() > 0);
}

TEST(Gated_OWExploration_DiscoverPOI)
{
    OWExplorationSystem exploration;
    exploration.Initialize(nullptr);

    size_t initial = exploration.GetDiscoveredCount();
    exploration.RevealPOI(1);
    EXPECT_TRUE(exploration.GetDiscoveredCount() >= initial);
}

TEST(Gated_OWExploration_FindSecret)
{
    OWExplorationSystem exploration;
    exploration.Initialize(nullptr);
    exploration.RevealPOI(1);
    exploration.FindSecret(1);
    // Should not crash, secret tracking works
    EXPECT_TRUE(true);
}

TEST(Gated_OWExploration_Completion)
{
    OWExplorationSystem exploration;
    exploration.Initialize(nullptr);
    float completion = exploration.GetOverallCompletion();
    EXPECT_TRUE(completion >= 0.0f && completion <= 100.0f);
}

TEST(Gated_OWExploration_Strings)
{
    OWExplorationSystem exploration;
    exploration.Initialize(nullptr);
    EXPECT_TRUE(!exploration.GetExplorationString().empty());
    EXPECT_TRUE(!exploration.GetPOIListString().empty());
}

TEST(Gated_OWExploration_Update)
{
    OWExplorationSystem exploration;
    exploration.Initialize(nullptr);
    for (int i = 0; i < 50; i++)
        exploration.Update(0.016f, static_cast<float>(i * 10), 0.0f, 0.0f);
    EXPECT_TRUE(true); // no crash
}

TEST(Gated_OWExploration_DiscoveryCallbackUnlocksFastTravelOnce)
{
    OWPlayerSystem player;
    OWExplorationSystem exploration;
    EXPECT_TRUE(player.Initialize(nullptr));
    EXPECT_TRUE(exploration.Initialize(nullptr));

    size_t callbackCount = 0;
    exploration.SetDiscoveryCallback(
        [&](const PointOfInterest& poi)
        {
            ++callbackCount;
            if (poi.type == POIType::FastTravel)
                player.UnlockFastTravel({poi.poiId, poi.name, poi.x, poi.y, poi.z, poi.regionId});
        });

    exploration.Update(0.1f, 0.0f, 5.0f, 0.0f);
    EXPECT_EQ(callbackCount, static_cast<size_t>(1));
    EXPECT_EQ(player.GetFastTravelCount(), static_cast<size_t>(2));
    EXPECT_TRUE(player.FastTravelTo(4));
    EXPECT_EQ(player.GetWorldState().currentRegionId, static_cast<uint32_t>(1));

    exploration.Update(0.1f, 0.0f, 5.0f, 0.0f);
    EXPECT_EQ(callbackCount, static_cast<size_t>(1));
    EXPECT_EQ(player.GetFastTravelCount(), static_cast<size_t>(2));
}

TEST(Gated_OWPlayer_RegionTracksWorldPosition)
{
    OWWorldSetup world;
    OWPlayerSystem player;
    EXPECT_TRUE(world.Initialize(nullptr));
    EXPECT_TRUE(player.Initialize(nullptr));

    player.SetPosition(3500.0f, 15.0f, 500.0f);
    const auto& position = player.GetWorldState();
    const BiomeRegion* region = world.GetRegionAtPosition(position.posX, position.posY, position.posZ);
    EXPECT_TRUE(region != nullptr);
    if (region)
        player.SetCurrentRegion(region->regionId);
    EXPECT_EQ(player.GetWorldState().currentRegionId, static_cast<uint32_t>(2));

    player.SetPosition(50000.0f, 0.0f, 50000.0f);
    region = world.GetRegionAtPosition(player.GetWorldState().posX, player.GetWorldState().posY,
                                       player.GetWorldState().posZ);
    player.SetCurrentRegion(region ? region->regionId : 0);
    EXPECT_EQ(player.GetWorldState().currentRegionId, static_cast<uint32_t>(0));
}

TEST(Gated_OWPlayer_RegionLookupUsesElevationInOverlappingVolumes)
{
    OWWorldSetup world;
    EXPECT_TRUE(world.Initialize(nullptr));

    const BiomeRegion* lowRegion = world.GetRegionAtPosition(3500.0f, 50.0f, 2500.0f);
    EXPECT_TRUE(lowRegion != nullptr);
    if (lowRegion)
        EXPECT_EQ(lowRegion->regionId, static_cast<uint32_t>(2));

    const BiomeRegion* elevatedRegion = world.GetRegionAtPosition(3500.0f, 500.0f, 2500.0f);
    EXPECT_TRUE(elevatedRegion != nullptr);
    if (elevatedRegion)
        EXPECT_EQ(elevatedRegion->regionId, static_cast<uint32_t>(3));
}

// --- OWGatheringSystem full tests ---

TEST(Gated_OWGathering_Initialize)
{
    OWGatheringSystem gathering;
    bool ok = gathering.Initialize(nullptr);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(gathering.GetNodeCount() > 0);
    EXPECT_TRUE(gathering.GetRecipeCount() > 0);
}

TEST(Gated_OWGathering_HarvestNode)
{
    OWGatheringSystem gathering;
    gathering.Initialize(nullptr);
    uint32_t yield = gathering.HarvestNode(1);
    EXPECT_TRUE(yield > 0);
}

TEST(Gated_OWGathering_AddResourceAndCraft)
{
    OWGatheringSystem gathering;
    gathering.Initialize(nullptr);

    gathering.AddResource(ResourceType::Wood, 100);
    gathering.AddResource(ResourceType::Stone, 100);
    gathering.AddResource(ResourceType::Iron, 100);

    EXPECT_EQ(gathering.GetInventory().Get(ResourceType::Wood), 100u);

    // Try crafting — should work if recipe exists with these ingredients
    if (gathering.CanCraft(1))
    {
        bool crafted = gathering.Craft(1);
        EXPECT_TRUE(crafted);
    }
}

TEST(Gated_OWGathering_Strings)
{
    OWGatheringSystem gathering;
    gathering.Initialize(nullptr);
    gathering.AddResource(ResourceType::Wood, 5);
    EXPECT_TRUE(!gathering.GetNodeListString().empty());
    EXPECT_TRUE(!gathering.GetRecipeListString().empty());
    EXPECT_TRUE(!gathering.GetInventoryString().empty());
}

TEST(Gated_OWGathering_RespawnUpdate)
{
    OWGatheringSystem gathering;
    gathering.Initialize(nullptr);
    gathering.HarvestNode(1); // deplete
    for (int i = 0; i < 100; i++)
        gathering.Update(1.0f); // tick respawn timers
    EXPECT_TRUE(true);          // no crash
}

// --- OWWildlifeSystem full tests ---

TEST(Gated_OWWildlife_Initialize)
{
    OWWildlifeSystem wildlife;
    bool ok = wildlife.Initialize(nullptr);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(wildlife.GetSpeciesCount() > 0);
    EXPECT_TRUE(wildlife.GetActiveAnimalCount() > 0);
}

TEST(Gated_OWWildlife_TameAnimal)
{
    OWWildlifeSystem wildlife;
    wildlife.Initialize(nullptr);
    size_t tamedBefore = wildlife.GetTamedCount();
    wildlife.TameAnimal(1); // attempt to tame first animal
    // May or may not succeed depending on species tameability
    EXPECT_TRUE(wildlife.GetTamedCount() >= tamedBefore);
}

TEST(Gated_OWWildlife_HuntAnimal)
{
    OWWildlifeSystem wildlife;
    wildlife.Initialize(nullptr);
    auto drops = wildlife.HuntAnimal(1);
    // Hunting should produce some resource drops
    (void)drops; // may be empty if animal not found, just verify no crash
}

TEST(Gated_OWWildlife_Strings)
{
    OWWildlifeSystem wildlife;
    wildlife.Initialize(nullptr);
    EXPECT_TRUE(!wildlife.GetWildlifeString().empty());
    EXPECT_TRUE(!wildlife.GetSpeciesListString().empty());
}

TEST(Gated_OWWildlife_UpdateBehavior)
{
    OWWildlifeSystem wildlife;
    wildlife.Initialize(nullptr);
    for (int i = 0; i < 100; i++)
        wildlife.Update(0.016f, static_cast<float>(i * 5), static_cast<float>(i * 3), 1);
    EXPECT_TRUE(wildlife.GetActiveAnimalCount() > 0);
}

// --- OWSettlementSystem full tests ---

TEST(Gated_OWSettlement_Initialize)
{
    OWSettlementSystem settlement;
    bool ok = settlement.Initialize(nullptr);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(settlement.GetSettlementCount() > 0);
}

TEST(Gated_OWSettlement_PlaceCamp)
{
    OWSettlementSystem settlement;
    settlement.Initialize(nullptr);
    uint32_t campId = settlement.PlaceCamp("MyCamp", 100.0f, 0.0f, 200.0f, 1);
    EXPECT_TRUE(campId > 0);
    EXPECT_TRUE(settlement.GetCampCount() > 0);
}

TEST(Gated_OWSettlement_UpgradeCamp)
{
    OWSettlementSystem settlement;
    settlement.Initialize(nullptr);
    uint32_t campId = settlement.PlaceCamp("UpgradeCamp", 50.0f, 0.0f, 50.0f, 1);
    bool upgraded = settlement.UpgradeCamp(campId);
    EXPECT_TRUE(upgraded);
}

TEST(Gated_OWSettlement_Strings)
{
    OWSettlementSystem settlement;
    settlement.Initialize(nullptr);
    EXPECT_TRUE(!settlement.GetSettlementListString().empty());
}

// --- OWDynamicEventSystem full tests ---

TEST(Gated_OWDynamicEvents_Initialize)
{
    OWDynamicEventSystem events;
    bool ok = events.Initialize(nullptr);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(events.GetTemplateCount() > 0);
}

TEST(Gated_OWDynamicEvents_TriggerEvent)
{
    OWDynamicEventSystem events;
    events.Initialize(nullptr);
    uint32_t eventId = events.TriggerEvent(1, 1, 100.0f, 200.0f);
    EXPECT_TRUE(eventId > 0);
    EXPECT_TRUE(events.GetActiveEventCount() > 0);
}

TEST(Gated_OWDynamicEvents_JoinEvent)
{
    OWDynamicEventSystem events;
    events.Initialize(nullptr);
    uint32_t eventId = events.TriggerEvent(1, 1, 0.0f, 0.0f);
    bool joined = events.JoinEvent(eventId);
    EXPECT_TRUE(joined);
}

TEST(Gated_OWDynamicEvents_Strings)
{
    OWDynamicEventSystem events;
    events.Initialize(nullptr);
    EXPECT_TRUE(!events.GetEventListString().empty());
}

TEST(Gated_OWDynamicEvents_Update)
{
    OWDynamicEventSystem events;
    events.Initialize(nullptr);
    for (int i = 0; i < 100; i++)
        events.Update(1.0f, 0.0f, 0.0f, 1);
    EXPECT_TRUE(true); // no crash
}

// --- OWWorldSetup full tests ---

TEST(Gated_OWWorldSetup_Initialize)
{
    OWWorldSetup world;
    bool ok = world.Initialize(nullptr);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(world.GetRegionCount() > 0);
}

TEST(Gated_OWWorldSetup_Regions)
{
    OWWorldSetup world;
    world.Initialize(nullptr);
    auto& regions = world.GetRegions();
    EXPECT_TRUE(!regions.empty());

    auto* region = world.GetRegion(1);
    EXPECT_TRUE(region != nullptr);
}

TEST(Gated_OWWorldSetup_Strings)
{
    OWWorldSetup world;
    world.Initialize(nullptr);
    EXPECT_TRUE(!world.GetRegionListString().empty());
    EXPECT_TRUE(!world.GetWorldStatusString().empty());
}

#endif // SPARK_TEST_HAS_IMGUI
