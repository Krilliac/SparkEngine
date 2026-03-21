/**
 * @file MMOCraftingSystem.h
 * @brief Item crafting with recipes, stations, and skill progression
 * @author Spark Engine Team
 * @date 2026
 *
 * Integrates with MMOInventorySystem for material consumption and output.
 * Supports multiple crafting disciplines, station requirements, skill levels,
 * and timed crafting with progress tracking.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/MMOEnums.h"
#include "Inventory/MMOInventorySystem.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMO
{

    /// @brief A material requirement for a recipe
    struct CraftingMaterial
    {
        uint32_t itemDefId = 0;
        int count = 1;
    };

    /// @brief A crafting recipe definition
    struct CraftingRecipe
    {
        uint32_t id = 0;
        std::string name;
        std::string description;
        CraftingDiscipline discipline = CraftingDiscipline::Weaponsmithing;
        CraftingStation station = CraftingStation::None;
        int requiredSkillLevel = 0;
        std::vector<CraftingMaterial> materials;
        uint32_t resultItemId = 0;
        int resultCount = 1;
        float craftTime = 2.0f;
        int skillXP = 10;
    };

    /// @brief Per-discipline skill progress
    struct CraftingSkill
    {
        CraftingDiscipline discipline = CraftingDiscipline::Weaponsmithing;
        int level = 1;
        int currentXP = 0;
        int maxLevel = 100;
        int GetXPForNext() const { return level * 50 + 100; }
    };

    /// @brief Per-player crafting state
    struct CraftingState
    {
        std::unordered_map<int, CraftingSkill> skills;
        std::vector<uint32_t> knownRecipes;
        bool isCrafting = false;
        uint32_t activeRecipeId = 0;
        float craftProgress = 0.0f;
        CraftingStation nearbyStation = CraftingStation::None;
    };

    /**
     * @brief Recipe registry and crafting execution
     */
    class MMOCraftingSystem
    {
      public:
        MMOCraftingSystem() = default;
        ~MMOCraftingSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float dt, CraftingState& crafter, InventoryData& inv);
        void Shutdown();
        void RenderDebugUI();

        // === Recipes ===
        void RegisterRecipe(const CraftingRecipe& recipe);
        const CraftingRecipe* GetRecipe(uint32_t id) const;
        std::vector<const CraftingRecipe*> GetAvailableRecipes(const CraftingState& crafter) const;

        // === Crafting ===
        bool CanCraft(const CraftingState& crafter, const InventoryData& inv, uint32_t recipeId) const;
        bool StartCraft(CraftingState& crafter, InventoryData& inv, uint32_t recipeId);
        void CancelCraft(CraftingState& crafter);
        void LearnRecipe(CraftingState& crafter, uint32_t recipeId);
        bool KnowsRecipe(const CraftingState& crafter, uint32_t recipeId) const;
        int GetSkillLevel(const CraftingState& crafter, CraftingDiscipline disc) const;

        size_t GetRecipeCount() const { return m_recipes.size(); }
        std::string GetStatusString(const CraftingState& crafter) const;

      private:
        void RegisterDefaultRecipes();
        void CompleteCraft(CraftingState& crafter, InventoryData& inv);
        void AwardSkillXP(CraftingState& crafter, CraftingDiscipline disc, int xp);

        Spark::IEngineContext* m_context{nullptr};
        MMOInventorySystem* m_invSystem{nullptr};
        std::unordered_map<uint32_t, CraftingRecipe> m_recipes;
    };

} // namespace MMO
