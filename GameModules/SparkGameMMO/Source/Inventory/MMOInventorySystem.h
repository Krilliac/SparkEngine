/**
 * @file MMOInventorySystem.h
 * @brief MMO item definitions, inventory management, and loot tables
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides item definitions, per-player inventory with weight/slot limits,
 * and weighted loot tables for enemy drops. Designed for server-authoritative
 * item management where the server validates all transactions.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/MMOEnums.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMO
{

    /// @brief Immutable definition of an item type
    struct ItemDef
    {
        uint32_t id = 0;
        std::string name;
        std::string description;
        ItemCategory category = ItemCategory::Misc;
        ItemRarity rarity = ItemRarity::Common;
        int maxStackSize = 1;
        float weight = 1.0f;
        int sellValue = 0;
        bool isQuestItem = false;
    };

    /// @brief A stack of items in a single inventory slot
    struct ItemStack
    {
        uint32_t itemDefId = 0;
        int count = 0;
        bool IsEmpty() const { return count <= 0 || itemDefId == 0; }
    };

    /// @brief Per-player inventory data
    struct InventoryData
    {
        std::vector<ItemStack> slots;
        int maxSlots = 30;
        float maxWeight = 150.0f;
        int currency = 0;
    };

    /// @brief Loot table entry for weighted random drops
    struct LootTableEntry
    {
        uint32_t itemDefId = 0;
        int minCount = 1;
        int maxCount = 1;
        float weight = 1.0f;
    };

    /**
     * @brief Item registry, inventory operations, and loot tables
     */
    class MMOInventorySystem
    {
      public:
        MMOInventorySystem() = default;
        ~MMOInventorySystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Shutdown();
        void RenderDebugUI();

        // === Item Registry ===

        void RegisterItem(const ItemDef& def);
        const ItemDef* GetItem(uint32_t id) const;
        size_t GetItemCount() const { return m_items.size(); }

        // === Inventory Operations ===

        float GetTotalWeight(const InventoryData& inv) const;
        int AddItem(InventoryData& inv, uint32_t itemId, int count = 1);
        int RemoveItem(InventoryData& inv, uint32_t itemId, int count = 1);
        int CountItem(const InventoryData& inv, uint32_t itemId) const;
        bool HasItem(const InventoryData& inv, uint32_t itemId, int minCount = 1) const;
        bool IsFull(const InventoryData& inv) const;

        // === Loot Tables ===

        void RegisterLootTable(const std::string& tableName, const std::vector<LootTableEntry>& entries);
        std::vector<ItemStack> RollLootTable(const std::string& tableName, int rolls = 1);

        std::string GetInventoryString(const InventoryData& inv) const;

      private:
        void RegisterDefaultItems();
        void RegisterDefaultLootTables();

        struct LootTable
        {
            std::vector<LootTableEntry> entries;
            float totalWeight = 0.0f;
        };

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, ItemDef> m_items;
        std::unordered_map<std::string, LootTable> m_lootTables;
        std::mt19937 m_rng{std::random_device{}()};
    };

} // namespace MMO
