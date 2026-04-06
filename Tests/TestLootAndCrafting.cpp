// TestLootAndCrafting.cpp - Tests for Spark::Gameplay::LootTableManager and CraftingSystem
#include "TestFramework.h"
#include "Engine/Crafting/LootAndCraftingSystem.h"

TEST(LootTableManager_Initialize)
{
    auto& mgr = Spark::Gameplay::LootTableManager::GetInstance();
    mgr.Initialize();
    EXPECT_EQ(static_cast<size_t>(0), mgr.GetTableCount());
    mgr.Shutdown();
}

TEST(LootTableManager_RegisterAndLookup)
{
    auto& mgr = Spark::Gameplay::LootTableManager::GetInstance();
    mgr.Initialize();
    Spark::Gameplay::LootTable table;
    table.name = "goblin_drops";
    table.maxDrops = 2;
    table.entries.push_back({"gold", 10.0f, 1, 5, Spark::Gameplay::ItemRarity::Common, {}, {}, ""});
    mgr.RegisterTable("goblin_drops", table);
    EXPECT_EQ(static_cast<size_t>(1), mgr.GetTableCount());
    EXPECT_TRUE(mgr.GetTable("goblin_drops") != nullptr);
    EXPECT_TRUE(mgr.GetTable("nonexistent") == nullptr);
    mgr.Shutdown();
}

TEST(LootTableManager_RollLoot)
{
    auto& mgr = Spark::Gameplay::LootTableManager::GetInstance();
    mgr.Initialize();
    Spark::Gameplay::LootTable table;
    table.name = "test";
    table.maxDrops = 1;
    table.entries.push_back({"item_a", 100.0f, 1, 1, Spark::Gameplay::ItemRarity::Common, {}, {}, ""});
    mgr.RegisterTable("test", table);
    auto drops = mgr.RollLoot("test");
    EXPECT_EQ(static_cast<size_t>(1), drops.size());
    if (!drops.empty())
        EXPECT_EQ(std::string("item_a"), drops[0].itemId);
    mgr.Shutdown();
}

TEST(LootTableManager_GuaranteedDrops)
{
    auto& mgr = Spark::Gameplay::LootTableManager::GetInstance();
    mgr.Initialize();
    Spark::Gameplay::LootTable table;
    table.name = "boss";
    table.maxDrops = 0;
    table.guaranteedDrops.push_back({"key_item", 1.0f, 1, 1, Spark::Gameplay::ItemRarity::Epic, {}, {}, ""});
    mgr.RegisterTable("boss", table);
    auto drops = mgr.RollLoot("boss");
    EXPECT_EQ(static_cast<size_t>(1), drops.size());
    if (!drops.empty())
        EXPECT_EQ(std::string("key_item"), drops[0].itemId);
    mgr.Shutdown();
}

TEST(LootTableManager_LevelFiltering)
{
    auto& mgr = Spark::Gameplay::LootTableManager::GetInstance();
    mgr.Initialize();
    Spark::Gameplay::LootTable table;
    table.name = "leveled";
    table.maxDrops = 1;
    table.entries.push_back({"low_item", 100.0f, 1, 1, Spark::Gameplay::ItemRarity::Common, std::optional<uint32_t>(1),
                             std::optional<uint32_t>(5), ""});
    table.entries.push_back({"high_item", 100.0f, 1, 1, Spark::Gameplay::ItemRarity::Common,
                             std::optional<uint32_t>(10), std::optional<uint32_t>(20), ""});
    mgr.RegisterTable("leveled", table);
    auto drops = mgr.RollLoot("leveled", 3, 1.0f);
    EXPECT_FALSE(drops.empty());
    if (!drops.empty())
        EXPECT_EQ(std::string("low_item"), drops[0].itemId);
    mgr.Shutdown();
}

TEST(LootTableManager_UnregisterTable)
{
    auto& mgr = Spark::Gameplay::LootTableManager::GetInstance();
    mgr.Initialize();
    Spark::Gameplay::LootTable table;
    table.name = "temp";
    mgr.RegisterTable("temp", table);
    mgr.UnregisterTable("temp");
    EXPECT_EQ(static_cast<size_t>(0), mgr.GetTableCount());
    mgr.Shutdown();
}

TEST(CraftingSystem_RegisterAndCanCraft)
{
    auto& sys = Spark::Gameplay::CraftingSystem::GetInstance();
    sys.Initialize();
    Spark::Gameplay::CraftingRecipe recipe;
    recipe.recipeId = "potion";
    recipe.ingredients = {{"herb", 2}, {"water", 1}};
    recipe.results = {{"health_potion", 1}};
    sys.RegisterRecipe(recipe);
    EXPECT_TRUE(sys.GetRecipe("potion") != nullptr);
    std::unordered_map<std::string, uint32_t> has = {{"herb", 5}, {"water", 3}};
    EXPECT_TRUE(sys.CanCraft("potion", has));
    std::unordered_map<std::string, uint32_t> lacking = {{"herb", 1}};
    EXPECT_FALSE(sys.CanCraft("potion", lacking));
    sys.Shutdown();
}

TEST(CraftingSystem_StartCraftAndUpdate)
{
    auto& sys = Spark::Gameplay::CraftingSystem::GetInstance();
    sys.Initialize();
    Spark::Gameplay::CraftingRecipe recipe;
    recipe.recipeId = "shield";
    recipe.craftingTime = 2.0f;
    recipe.ingredients = {{"wood", 5}};
    recipe.results = {{"wood_shield", 1}};
    sys.RegisterRecipe(recipe);
    EXPECT_TRUE(sys.StartCraft("shield"));
    EXPECT_FALSE(sys.StartCraft("nonexistent"));
    sys.Update(1.0f);
    EXPECT_TRUE(sys.GetQueue()[0].state == Spark::Gameplay::CraftState::InProgress);
    sys.Update(1.5f);
    EXPECT_TRUE(sys.GetQueue()[0].state == Spark::Gameplay::CraftState::Complete);
    sys.Shutdown();
}

TEST(CraftingSystem_CraftingQueue)
{
    auto& sys = Spark::Gameplay::CraftingSystem::GetInstance();
    sys.Initialize();
    Spark::Gameplay::CraftingRecipe r1;
    r1.recipeId = "a";
    r1.craftingTime = 1.0f;
    sys.RegisterRecipe(r1);
    Spark::Gameplay::CraftingRecipe r2;
    r2.recipeId = "b";
    r2.craftingTime = 2.0f;
    sys.RegisterRecipe(r2);
    sys.StartCraft("a");
    sys.StartCraft("b");
    EXPECT_EQ(static_cast<size_t>(2), sys.GetQueue().size());
    EXPECT_TRUE(sys.CancelCraft(0));
    EXPECT_EQ(static_cast<size_t>(1), sys.GetQueue().size());
    EXPECT_FALSE(sys.CancelCraft(99));
    sys.Shutdown();
}

TEST(CraftingSystem_DiscoverRecipe)
{
    auto& sys = Spark::Gameplay::CraftingSystem::GetInstance();
    sys.Initialize();
    Spark::Gameplay::CraftingRecipe recipe;
    recipe.recipeId = "secret";
    recipe.isDiscovered = false;
    sys.RegisterRecipe(recipe);
    EXPECT_EQ(static_cast<size_t>(0), sys.GetDiscoveredRecipes().size());
    sys.DiscoverRecipe("secret");
    EXPECT_EQ(static_cast<size_t>(1), sys.GetDiscoveredRecipes().size());
    sys.Shutdown();
}

TEST(CraftingSystem_FindRecipesByIngredient)
{
    auto& sys = Spark::Gameplay::CraftingSystem::GetInstance();
    sys.Initialize();
    Spark::Gameplay::CraftingRecipe r1;
    r1.recipeId = "sword";
    r1.ingredients = {{"iron", 3}};
    sys.RegisterRecipe(r1);
    Spark::Gameplay::CraftingRecipe r2;
    r2.recipeId = "shield";
    r2.ingredients = {{"iron", 2}, {"wood", 1}};
    sys.RegisterRecipe(r2);
    auto found = sys.FindRecipesByIngredient("iron");
    EXPECT_EQ(static_cast<size_t>(2), found.size());
    auto none = sys.FindRecipesByIngredient("gold");
    EXPECT_EQ(static_cast<size_t>(0), none.size());
    sys.Shutdown();
}
