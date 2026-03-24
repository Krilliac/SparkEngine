/**
 * @file RPGInventorySystem.h
 * @brief Item definitions, equipment, consumables, and weight-based inventory
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides item definitions with rarity tiers (Common through Legendary),
 * equipment items with stat bonuses per slot, consumable items, weight-based
 * inventory limits, and item comparison (stat difference display).
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RPGEnums.h"
#include "Character/RPGCharacterSystem.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace RPG
{

    /// @brief Immutable definition of an item
    struct RPGItemDef
    {
        uint32_t id = 0;
        std::string name;
        std::string description;
        ItemRarity rarity = ItemRarity::Common;
        ItemSlot equipSlot = ItemSlot::Count; ///< Count = not equippable
        float weight = 1.0f;
        int sellValue = 0;
        int maxStackSize = 1;
        bool isConsumable = false;

        // Equipment stat bonuses (only meaningful if equipSlot != Count)
        CharacterStats statBonus;

        // Consumable effects
        float healAmount = 0.0f;
        float manaRestore = 0.0f;
    };

    /// @brief A single inventory slot holding stacked items
    struct RPGItemStack
    {
        uint32_t itemDefId = 0;
        int count = 0;
        bool IsEmpty() const { return count <= 0 || itemDefId == 0; }
    };

    /// @brief Per-character inventory data
    struct RPGInventoryData
    {
        std::vector<RPGItemStack> slots;
        int maxSlots = 24;
        float maxWeight = 100.0f;
        int currency = 0;
    };

    /// @brief Result of comparing two equipment items
    struct ItemComparison
    {
        CharacterStats difference; ///< Positive = new item is better
        bool isUpgrade = false;
    };

    /**
     * @brief Item registry, inventory operations, and equipment comparison
     */
    class RPGInventorySystem
    {
      public:
        RPGInventorySystem() = default;
        ~RPGInventorySystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Shutdown();
        void RenderDebugUI();

        // === Item registry ===
        void RegisterItem(const RPGItemDef& def);
        const RPGItemDef* GetItem(uint32_t id) const;
        size_t GetItemCount() const { return m_items.size(); }
        std::string GetItemListString() const;

        // === Inventory operations ===
        float GetTotalWeight(const RPGInventoryData& inv) const;
        int AddItem(RPGInventoryData& inv, uint32_t itemId, int count = 1);
        int RemoveItem(RPGInventoryData& inv, uint32_t itemId, int count = 1);
        bool HasItem(const RPGInventoryData& inv, uint32_t itemId, int minCount = 1) const;
        bool IsFull(const RPGInventoryData& inv) const;

        // === Equipment comparison ===
        ItemComparison CompareItems(uint32_t currentItemId, uint32_t newItemId) const;

        // === Rarity helpers ===
        static const char* GetRarityName(ItemRarity rarity);

      private:
        void RegisterDefaultItems();

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, RPGItemDef> m_items;
    };

} // namespace RPG
