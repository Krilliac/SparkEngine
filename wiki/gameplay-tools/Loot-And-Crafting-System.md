# Loot and Crafting System

Engine-level loot table manager with weighted random drops and a crafting system with recipes, timed queues, and discovery.

**Source:** `SparkEngine/Source/Engine/Crafting/LootAndCraftingSystem.h`

## Overview

The Loot and Crafting system provides two cooperating singletons for item generation and transformation. `LootTableManager` handles weighted random loot generation with rarity tiers, level filtering, and a luck modifier that scales the probability of non-Common drops. `CraftingSystem` manages recipes with ingredient validation, station requirements, skill level gating, timed crafting queues, and a recipe discovery mechanic.

Loot tables support both guaranteed drops (always given) and a weighted random pool. Each entry specifies an item ID, relative weight, quantity range, rarity tier, and optional level restrictions. The "empty weight" parameter controls the probability of rolling nothing from the pool. The luck modifier multiplies weights on non-Common entries, making higher-rarity items more likely when the player has luck bonuses.

Crafting recipes define required ingredients with quantities, output items, crafting duration (0 for instant), an optional crafting station type, and skill requirements. Recipes can be marked as undiscovered until the player learns them. The crafting queue tracks active crafts with progress normalized to [0, 1] and automatically completes them when their duration elapses.

## Architecture

```
LootTableManager (singleton)           CraftingSystem (singleton)
  |                                      |
  +-- LootTable "enemies_forest"         +-- CraftingRecipe "iron_sword"
  |     +-- guaranteedDrops[]            |     +-- ingredients: [{iron, 3}, {wood, 1}]
  |     +-- entries[] (weighted)         |     +-- results: [{iron_sword, 1}]
  |     +-- emptyWeight                  |     +-- craftingTime: 5.0s
  |     +-- maxDrops                     |     +-- requiredStation: "forge"
  |                                      |
  +-- RollLoot(table, level, luck)       +-- CraftingQueue[]
        --> LootDrop[]                         +-- progress [0..1]
                                               +-- state: Queued/InProgress/Complete
```

## Key Classes

| Class | Description |
|-------|-------------|
| `LootTableManager` | Singleton for registering loot tables and rolling drops |
| `CraftingSystem` | Singleton for recipes, validation, timed queues, and discovery |
| `LootTable` | Named collection of weighted entries with guaranteed drops |
| `LootEntry` | Single weighted entry with item, rarity, count range, and level filter |
| `CraftingRecipe` | Recipe with ingredients, results, time, station, and skill requirements |
| `CraftingQueue` | In-progress craft tracking progress and state |
| `ItemRarity` | Enum: Common, Uncommon, Rare, Epic, Legendary |

## Usage

### Loot Tables

```cpp
auto& loot = Spark::Gameplay::LootTableManager::GetInstance();
loot.Initialize();

// Define a loot table
Spark::Gameplay::LootTable table;
table.name = "forest_chest";
table.maxDrops = 3;
table.emptyWeight = 2.0f;  // 2 weight units for "nothing"
table.guaranteedDrops = {{"gold_coin", 1.0f, 5, 10, Spark::Gameplay::ItemRarity::Common}};
table.entries = {
    {"iron_sword", 5.0f, 1, 1, Spark::Gameplay::ItemRarity::Uncommon},
    {"health_potion", 10.0f, 1, 3, Spark::Gameplay::ItemRarity::Common},
    {"diamond", 0.5f, 1, 1, Spark::Gameplay::ItemRarity::Legendary,
     std::nullopt, std::nullopt, ""},
};
loot.RegisterTable("forest_chest", table);

// Roll loot with level and luck
auto drops = loot.RollLoot("forest_chest", /*playerLevel=*/10, /*luck=*/1.5f);
for (const auto& drop : drops)
{
    // drop.itemId, drop.count, drop.rarity
}
```

### Crafting

```cpp
auto& crafting = Spark::Gameplay::CraftingSystem::GetInstance();
crafting.Initialize();

// Register a recipe
Spark::Gameplay::CraftingRecipe recipe;
recipe.recipeId = "iron_sword";
recipe.name = "Iron Sword";
recipe.ingredients = {{"iron_ingot", 3}, {"wood_plank", 1}};
recipe.results = {{"iron_sword", 1}};
recipe.craftingTime = 5.0f;
recipe.requiredStation = "forge";
recipe.requiredSkillLevel = 2;
crafting.RegisterRecipe(recipe);

// Check if craftable
std::unordered_map<std::string, uint32_t> inventory = {
    {"iron_ingot", 5}, {"wood_plank", 3}
};
if (crafting.CanCraft("iron_sword", inventory, "forge", /*skill=*/3))
{
    crafting.StartCraft("iron_sword");
}

// Per-frame update advances queued crafts
crafting.Update(deltaTime);

// Check queue status
for (const auto& entry : crafting.GetQueue())
{
    // entry.progress, entry.state
}
```

## API Reference

### LootTableManager

| Method | Description |
|--------|-------------|
| `Initialize()` | Clear tables and seed RNG |
| `RegisterTable(name, table)` | Register a loot table |
| `RollLoot(table, level, luck)` | Roll drops with level filtering and luck modifier |
| `RollSingle(table)` | Roll a single item with no modifiers |
| `GetTable(name)` | Get a table by name (or nullptr) |
| `GetTableNames()` | List all registered table names |

### CraftingSystem

| Method | Description |
|--------|-------------|
| `Initialize()` | Clear all recipes and queue |
| `RegisterRecipe(recipe)` | Register a crafting recipe |
| `CanCraft(id, items, station, skill)` | Check if a recipe can be crafted |
| `StartCraft(id)` | Queue a recipe for crafting |
| `Update(dt)` | Advance all queued crafts |
| `CancelCraft(index)` | Cancel a queued craft by index |
| `DiscoverRecipe(id)` | Mark a recipe as discovered |
| `GetDiscoveredRecipes()` | Get all discovered recipes |
| `FindRecipesByIngredient(item)` | Find recipes using an ingredient |
| `FindRecipesByResult(item)` | Find recipes producing an item |

## Configuration

| Setting | Description |
|---------|-------------|
| `LootTable::maxDrops` | Maximum rolls from the random pool per loot event |
| `LootTable::emptyWeight` | Weight for "no drop" outcome (0 = always drops something) |
| `LootEntry::minLevel/maxLevel` | Optional player level range for entry eligibility |
| `CraftingRecipe::craftingTime` | Duration in seconds (0 = instant) |
| `CraftingRecipe::requiredStation` | Station type required (empty = craft anywhere) |

## Related Systems

- [Inventory System](Gameplay-Systems.md) -- Manages player items that loot populates and crafting consumes
- [DataTable System](DataTable-System.md) -- Loot tables and recipes can be loaded from CSV/JSON data tables
- [Quest System](Gameplay-Systems.md) -- Quests can reward loot table rolls or unlock recipes
