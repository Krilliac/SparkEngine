/**
 * @file LootAndCraftingSystem.h
 * @brief Engine-level loot tables with weighted random drops and a crafting system
 *
 * Two header-only singletons (GetInstance / Initialize / Shutdown / Console_GetStatus):
 * - **LootTableManager** -- weighted random loot with rarity, level filtering, luck.
 * - **CraftingSystem** -- recipes, ingredient validation, timed queues, discovery.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "Utils/LogMacros.h"

namespace Spark::Gameplay
{

    /// @brief Item rarity tiers used by loot and crafting systems
    enum class ItemRarity : uint8_t
    {
        Common,
        Uncommon,
        Rare,
        Epic,
        Legendary
    };

    /// @brief Convert an ItemRarity to a human-readable string
    [[nodiscard]] inline const char* ItemRarityToString(ItemRarity r)
    {
        switch (r)
        {
        case ItemRarity::Common:
            return "Common";
        case ItemRarity::Uncommon:
            return "Uncommon";
        case ItemRarity::Rare:
            return "Rare";
        case ItemRarity::Epic:
            return "Epic";
        case ItemRarity::Legendary:
            return "Legendary";
        }
        return "Unknown";
    }

    // -- Loot Table Data Types --------------------------------------------------

    /// @brief A single weighted entry in a loot table
    struct LootEntry
    {
        std::string itemId;    ///< Item to drop
        float weight = 1.0f;   ///< Relative drop weight
        uint32_t minCount = 1; ///< Minimum quantity per drop
        uint32_t maxCount = 1; ///< Maximum quantity per drop
        ItemRarity rarity = ItemRarity::Common;
        std::optional<uint32_t> minLevel; ///< Min player level (nullopt = none)
        std::optional<uint32_t> maxLevel; ///< Max player level (nullopt = none)
        std::string condition;            ///< Optional conditional expression
    };

    /// @brief A named collection of loot entries with guaranteed drops and pool rules
    struct LootTable
    {
        std::string name;
        std::vector<LootEntry> entries;         ///< Random pool (weighted)
        std::vector<LootEntry> guaranteedDrops; ///< Always dropped
        uint32_t maxDrops = 1;                  ///< Max rolls from random pool
        float emptyWeight = 0.0f;               ///< Weight for "nothing" outcome
    };

    /// @brief Result of a single loot roll
    struct LootDrop
    {
        std::string itemId;
        uint32_t count = 0;
        ItemRarity rarity = ItemRarity::Common;
    };

    // -- LootTableManager (singleton) -------------------------------------------

    /// @brief Manages loot tables and performs weighted random loot generation.
    class LootTableManager
    {
      public:
        static LootTableManager& GetInstance()
        {
            static LootTableManager instance;
            return instance;
        }

        /// @brief Initialize and seed the RNG
        void Initialize()
        {
            m_tables.clear();
            m_rng.seed(std::random_device{}());
            m_initialized = true;
            SPARK_LOG_INFO(Spark::LogCategory::Game, "LootTableManager initialized");
        }

        /// @brief Shut down and release all loot tables
        void Shutdown()
        {
            m_tables.clear();
            m_initialized = false;
        }

        /// @brief Register a loot table by name
        void RegisterTable(const std::string& name, const LootTable& table) { m_tables[name] = table; }

        /// @brief Get a registered loot table, or nullptr
        [[nodiscard]] const LootTable* GetTable(const std::string& name) const
        {
            auto it = m_tables.find(name);
            return (it != m_tables.end()) ? &it->second : nullptr;
        }

        /// @brief Remove a loot table by name
        void UnregisterTable(const std::string& name) { m_tables.erase(name); }

        /// @brief Roll loot with level filtering and luck modifier
        /// @param luckModifier Multiplied into non-Common weights (1.0 = normal)
        [[nodiscard]] std::vector<LootDrop> RollLoot(const std::string& tableName, uint32_t playerLevel = 0,
                                                     float luckModifier = 1.0f)
        {
            std::vector<LootDrop> results;
            const auto* table = GetTable(tableName);
            if (!table)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Game, "LootTableManager::RollLoot: table '%s' not found",
                               tableName.c_str());
                return results;
            }

            for (const auto& entry : table->guaranteedDrops)
            {
                if (PassesLevelFilter(entry, playerLevel))
                    results.push_back(MakeDrop(entry));
            }

            std::vector<const LootEntry*> eligible;
            std::vector<float> weights;
            float totalWeight = table->emptyWeight;
            for (const auto& entry : table->entries)
            {
                if (!PassesLevelFilter(entry, playerLevel))
                    continue;
                float w = entry.weight;
                if (entry.rarity != ItemRarity::Common)
                    w *= luckModifier;
                eligible.push_back(&entry);
                weights.push_back(w);
                totalWeight += w;
            }

            if (eligible.empty() && table->emptyWeight <= 0.0f)
                return results;

            for (uint32_t i = 0; i < table->maxDrops; ++i)
            {
                std::uniform_real_distribution<float> dist(0.0f, totalWeight);
                float roll = dist(m_rng);
                if (roll < table->emptyWeight)
                    continue;
                roll -= table->emptyWeight;
                for (size_t j = 0; j < eligible.size(); ++j)
                {
                    roll -= weights[j];
                    if (roll <= 0.0f)
                    {
                        results.push_back(MakeDrop(*eligible[j]));
                        break;
                    }
                }
            }
            SPARK_LOG_DEBUG(Spark::LogCategory::Game, "LootTableManager::RollLoot: table='%s', %zu drops",
                            tableName.c_str(), results.size());
            return results;
        }

        /// @brief Roll a single random item (no level/luck filtering)
        [[nodiscard]] LootDrop RollSingle(const std::string& tableName)
        {
            auto drops = RollLoot(tableName, 0, 1.0f);
            return drops.empty() ? LootDrop{} : drops.front();
        }

        /// @brief Get names of all registered tables
        [[nodiscard]] std::vector<std::string> GetTableNames() const
        {
            std::vector<std::string> names;
            names.reserve(m_tables.size());
            for (const auto& [n, _] : m_tables)
                names.push_back(n);
            return names;
        }

        [[nodiscard]] size_t GetTableCount() const { return m_tables.size(); }

        /// @brief Human-readable status for debugging
        [[nodiscard]] std::string Console_GetStatus() const
        {
            return std::string("LootTableManager: ") + (m_initialized ? "initialized" : "not initialized") +
                   ", tables=" + std::to_string(m_tables.size());
        }

      private:
        LootTableManager() = default;

        [[nodiscard]] static bool PassesLevelFilter(const LootEntry& entry, uint32_t level)
        {
            if (entry.minLevel.has_value() && level < *entry.minLevel)
                return false;
            if (entry.maxLevel.has_value() && level > *entry.maxLevel)
                return false;
            return true;
        }

        [[nodiscard]] LootDrop MakeDrop(const LootEntry& entry)
        {
            uint32_t count = entry.minCount;
            if (entry.maxCount > entry.minCount)
            {
                std::uniform_int_distribution<uint32_t> d(entry.minCount, entry.maxCount);
                count = d(m_rng);
            }
            return {entry.itemId, count, entry.rarity};
        }

        bool m_initialized = false;
        std::unordered_map<std::string, LootTable> m_tables;
        std::mt19937 m_rng;
    };

    // -- Crafting Data Types ----------------------------------------------------

    /// @brief An ingredient or result item with quantity
    struct CraftingIngredient
    {
        std::string itemId;
        uint32_t count = 1;
    };

    /// @brief A crafting recipe defining inputs, outputs, and requirements
    struct CraftingRecipe
    {
        std::string recipeId;
        std::string name;
        std::vector<CraftingIngredient> ingredients; ///< Required inputs
        std::vector<CraftingIngredient> results;     ///< Produced outputs
        float craftingTime = 0.0f;                   ///< Seconds (0 = instant)
        std::string requiredStation;                 ///< Station type (empty = anywhere)
        uint32_t requiredSkillLevel = 0;
        float skillXpReward = 0.0f;
        bool isDiscovered = false;
    };

    /// @brief State of an in-progress craft
    enum class CraftState : uint8_t
    {
        Queued,
        InProgress,
        Complete,
        Failed
    };

    /// @brief An entry in the crafting queue tracking a single craft's progress
    struct CraftingQueue
    {
        std::string recipeId;
        float startTime = 0.0f;
        float endTime = 0.0f;
        float progress = 0.0f; ///< [0, 1]
        CraftState state = CraftState::Queued;
    };

    // -- CraftingSystem (singleton) ---------------------------------------------

    /// @brief Manages crafting recipes, validation, timed queues, and recipe discovery.
    class CraftingSystem
    {
      public:
        static CraftingSystem& GetInstance()
        {
            static CraftingSystem instance;
            return instance;
        }

        /// @brief Initialize, clearing all state
        void Initialize()
        {
            m_recipes.clear();
            m_queue.clear();
            m_elapsedTime = 0.0f;
            m_initialized = true;
            SPARK_LOG_INFO(Spark::LogCategory::Game, "CraftingSystem initialized");
        }

        /// @brief Shut down and release all state
        void Shutdown()
        {
            m_recipes.clear();
            m_queue.clear();
            m_initialized = false;
        }

        /// @brief Register a crafting recipe
        void RegisterRecipe(const CraftingRecipe& recipe) { m_recipes[recipe.recipeId] = recipe; }

        /// @brief Get a recipe by ID, or nullptr
        [[nodiscard]] const CraftingRecipe* GetRecipe(const std::string& id) const
        {
            auto it = m_recipes.find(id);
            return (it != m_recipes.end()) ? &it->second : nullptr;
        }

        /// @brief Remove a recipe by ID
        void UnregisterRecipe(const std::string& id) { m_recipes.erase(id); }

        /// @brief Check if a recipe can be crafted with given items, station, and skill
        [[nodiscard]] bool CanCraft(const std::string& recipeId,
                                    const std::unordered_map<std::string, uint32_t>& availableItems,
                                    const std::string& station = "", uint32_t skillLevel = 0) const
        {
            const auto* recipe = GetRecipe(recipeId);
            if (!recipe)
                return false;
            if (!recipe->requiredStation.empty() && recipe->requiredStation != station)
                return false;
            if (recipe->requiredSkillLevel > 0 && skillLevel < recipe->requiredSkillLevel)
                return false;
            for (const auto& ing : recipe->ingredients)
            {
                auto it = availableItems.find(ing.itemId);
                if (it == availableItems.end() || it->second < ing.count)
                    return false;
            }
            return true;
        }

        /// @brief Queue a recipe for crafting; returns true if recipe exists
        bool StartCraft(const std::string& recipeId)
        {
            const auto* recipe = GetRecipe(recipeId);
            if (!recipe)
                return false;
            SPARK_LOG_INFO(Spark::LogCategory::Game, "CraftingSystem: starting craft '%s'", recipe->name.c_str());
            CraftingQueue entry;
            entry.recipeId = recipeId;
            entry.startTime = m_elapsedTime;
            entry.endTime = m_elapsedTime + recipe->craftingTime;
            entry.progress = (recipe->craftingTime <= 0.0f) ? 1.0f : 0.0f;
            entry.state = (recipe->craftingTime <= 0.0f) ? CraftState::Complete : CraftState::Queued;
            m_queue.push_back(entry);
            return true;
        }

        /// @brief Advance all queued crafts by deltaTime seconds
        void Update(float deltaTime)
        {
            m_elapsedTime += deltaTime;
            for (auto& entry : m_queue)
            {
                if (entry.state == CraftState::Complete || entry.state == CraftState::Failed)
                    continue;
                entry.state = CraftState::InProgress;
                float duration = entry.endTime - entry.startTime;
                if (duration <= 0.0f)
                {
                    entry.progress = 1.0f;
                    entry.state = CraftState::Complete;
                }
                else
                {
                    entry.progress = std::clamp((m_elapsedTime - entry.startTime) / duration, 0.0f, 1.0f);
                    if (entry.progress >= 1.0f)
                    {
                        entry.state = CraftState::Complete;
                        SPARK_LOG_INFO(Spark::LogCategory::Game, "CraftingSystem: craft '%s' complete",
                                       entry.recipeId.c_str());
                    }
                }
            }
        }

        /// @brief Get the current crafting queue
        [[nodiscard]] const std::vector<CraftingQueue>& GetQueue() const { return m_queue; }

        /// @brief Cancel a queued craft by index; returns true on success
        bool CancelCraft(size_t index)
        {
            if (index >= m_queue.size())
                return false;
            m_queue.erase(m_queue.begin() + static_cast<ptrdiff_t>(index));
            return true;
        }

        /// @brief Mark a recipe as discovered/learned
        void DiscoverRecipe(const std::string& id)
        {
            auto it = m_recipes.find(id);
            if (it != m_recipes.end())
                it->second.isDiscovered = true;
        }

        /// @brief Get all discovered recipes
        [[nodiscard]] std::vector<const CraftingRecipe*> GetDiscoveredRecipes() const
        {
            std::vector<const CraftingRecipe*> result;
            for (const auto& [_, r] : m_recipes)
                if (r.isDiscovered)
                    result.push_back(&r);
            return result;
        }

        /// @brief Get all registered recipes
        [[nodiscard]] std::vector<const CraftingRecipe*> GetAllRecipes() const
        {
            std::vector<const CraftingRecipe*> result;
            result.reserve(m_recipes.size());
            for (const auto& [_, r] : m_recipes)
                result.push_back(&r);
            return result;
        }

        /// @brief Find recipes that use itemId as an ingredient
        [[nodiscard]] std::vector<const CraftingRecipe*> FindRecipesByIngredient(const std::string& itemId) const
        {
            std::vector<const CraftingRecipe*> result;
            for (const auto& [_, recipe] : m_recipes)
            {
                for (const auto& ing : recipe.ingredients)
                {
                    if (ing.itemId == itemId)
                    {
                        result.push_back(&recipe);
                        break;
                    }
                }
            }
            return result;
        }

        /// @brief Find recipes that produce itemId as a result
        [[nodiscard]] std::vector<const CraftingRecipe*> FindRecipesByResult(const std::string& itemId) const
        {
            std::vector<const CraftingRecipe*> result;
            for (const auto& [_, recipe] : m_recipes)
            {
                for (const auto& res : recipe.results)
                {
                    if (res.itemId == itemId)
                    {
                        result.push_back(&recipe);
                        break;
                    }
                }
            }
            return result;
        }

        /// @brief Human-readable status for debugging
        [[nodiscard]] std::string Console_GetStatus() const
        {
            size_t completed = 0;
            for (const auto& e : m_queue)
            {
                if (e.state == CraftState::Complete)
                    ++completed;
            }
            return std::string("CraftingSystem: ") + (m_initialized ? "initialized" : "not initialized") +
                   ", recipes=" + std::to_string(m_recipes.size()) + ", queue=" + std::to_string(m_queue.size()) +
                   ", completed=" + std::to_string(completed);
        }

      private:
        CraftingSystem() = default;

        bool m_initialized = false;
        float m_elapsedTime = 0.0f;
        std::unordered_map<std::string, CraftingRecipe> m_recipes;
        std::vector<CraftingQueue> m_queue;
    };

} // namespace Spark::Gameplay
