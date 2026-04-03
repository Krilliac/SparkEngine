/**
 * @file OWGatheringSystem.h
 * @brief Resource nodes, harvesting, and crafting recipes
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages harvestable resource nodes scattered across biomes, a player
 * inventory of raw materials, and a crafting system that converts
 * gathered resources into tools, weapons, armor, and consumables.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/OpenWorldEnums.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace OpenWorld
{

    /// @brief A harvestable resource node in the world
    struct ResourceNode
    {
        uint32_t nodeId = 0;
        std::string name;
        ResourceType resource = ResourceType::Wood;
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        uint32_t regionId = 0;
        uint32_t maxYield = 5; ///< How many units before depleted
        uint32_t currentYield = 5;
        float respawnTime = 300.0f; ///< Seconds until refilled
        float respawnTimer = 0.0f;
        bool isDepleted = false;
    };

    /// @brief A crafting ingredient requirement
    struct CraftingIngredient
    {
        ResourceType resource = ResourceType::Wood;
        uint32_t amount = 1;
    };

    /// @brief A craftable item recipe
    struct CraftingRecipe
    {
        uint32_t recipeId = 0;
        std::string resultName;
        std::string description;
        CraftingCategory category = CraftingCategory::Tools;
        std::vector<CraftingIngredient> ingredients;
        float craftTime = 5.0f; ///< Seconds to craft
        bool requiresCraftingStation = false;
    };

    /// @brief Player resource inventory (raw material counts)
    struct ResourceInventory
    {
        std::unordered_map<ResourceType, uint32_t> resources;

        uint32_t Get(ResourceType type) const
        {
            auto it = resources.find(type);
            return it != resources.end() ? it->second : 0;
        }
        void Add(ResourceType type, uint32_t amount) { resources[type] += amount; }
        bool Spend(ResourceType type, uint32_t amount)
        {
            if (Get(type) < amount)
                return false;
            resources[type] -= amount;
            return true;
        }
    };

    /**
     * @brief Resource gathering and crafting system
     */
    class OWGatheringSystem
    {
      public:
        OWGatheringSystem() = default;
        ~OWGatheringSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        size_t GetNodeCount() const { return m_nodes.size(); }
        size_t GetRecipeCount() const { return m_recipes.size(); }
        const ResourceInventory& GetInventory() const { return m_inventory; }

        /// @brief Harvest a resource node, adding resources to inventory
        uint32_t HarvestNode(uint32_t nodeId);

        /// @brief Add resources directly (e.g., from hunting)
        void AddResource(ResourceType type, uint32_t amount);

        /// @brief Check if the player can craft a recipe
        bool CanCraft(uint32_t recipeId) const;

        /// @brief Craft an item, consuming resources
        bool Craft(uint32_t recipeId);

        std::string GetNodeListString() const;
        std::string GetRecipeListString() const;
        std::string GetInventoryString() const;

      private:
        void DefineResourceNodes();
        void DefineCraftingRecipes();
        void UpdateNodeRespawns(float deltaTime);

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, ResourceNode> m_nodes;
        std::vector<CraftingRecipe> m_recipes;
        ResourceInventory m_inventory;
        uint32_t m_totalHarvested{0};
        uint32_t m_totalCrafted{0};
        bool m_initialized{false};
    };

} // namespace OpenWorld
