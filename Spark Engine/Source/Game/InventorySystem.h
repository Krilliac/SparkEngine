/**
 * @file InventorySystem.h
 * @brief Item-based inventory system with slots, stacking, and weight
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a complete inventory system for managing items, loot tables,
 * and item definitions. Designed as ECS-compatible POD components with
 * standalone utility functions.
 *
 * ## Usage
 * @code
 *   // Define items
 *   Spark::ItemRegistry registry;
 *   registry.RegisterItem({1, "Health Potion", "Restores 50 HP", 10, 0.5f, Spark::ItemRarity::Common});
 *   registry.RegisterItem({2, "Iron Sword", "A sturdy sword", 1, 3.0f, Spark::ItemRarity::Uncommon});
 *
 *   // Add to entity via ECS
 *   auto& inv = world.AddComponent<Spark::InventoryComponent>(entity);
 *   inv.maxSlots = 20;
 *   inv.maxWeight = 100.0f;
 *
 *   Spark::InventoryOps::AddItem(inv, 1, 5);  // 5 health potions
 * @endcode
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <functional>
#include <cstdint>
#include <random>

#include "../Engine/Events/EventSystem.h"

namespace Spark {

// =============================================================================
// Item Rarity
// =============================================================================

/**
 * @brief Item rarity tiers affecting drop rates and visual display
 */
enum class ItemRarity {
    Common,         ///< White - basic items
    Uncommon,       ///< Green - slightly better
    Rare,           ///< Blue - notable finds
    Epic,           ///< Purple - powerful items
    Legendary       ///< Gold - extremely rare
};

/**
 * @brief Item category for filtering and UI grouping
 */
enum class ItemCategory {
    Weapon,
    Armor,
    Consumable,
    Material,
    Quest,
    Misc
};

// =============================================================================
// Item Definition
// =============================================================================

/**
 * @brief Immutable definition of an item type (shared across all instances)
 *
 * ItemDef describes the properties of an item class, not an individual instance.
 * All items of the same type share the same ItemDef.
 */
struct ItemDef {
    uint32_t id = 0;                    ///< Unique item type identifier
    std::string name;                   ///< Display name
    std::string description;            ///< Flavor text / tooltip
    ItemCategory category = ItemCategory::Misc;
    ItemRarity rarity = ItemRarity::Common;
    int maxStackSize = 1;               ///< Max items per inventory slot (1 = unstackable)
    float weight = 1.0f;               ///< Weight per unit in kg
    std::string iconPath;              ///< Path to UI icon texture
    bool isQuestItem = false;          ///< Quest items cannot be dropped or sold
    int sellValue = 0;                 ///< Base sell price in currency
};

// =============================================================================
// Item Stack (inventory slot contents)
// =============================================================================

/**
 * @brief A stack of items in a single inventory slot
 */
struct ItemStack {
    uint32_t itemDefId = 0;            ///< References an ItemDef by ID
    int count = 0;                     ///< Number of items in this stack

    bool IsEmpty() const { return count <= 0 || itemDefId == 0; }
};

// =============================================================================
// Inventory Component (ECS)
// =============================================================================

/**
 * @brief ECS component representing an entity's inventory
 *
 * Pure data struct following the engine's ECS pattern.
 * Use InventoryOps for logic operations.
 */
struct InventoryComponent {
    std::vector<ItemStack> slots;      ///< Inventory slots
    int maxSlots = 20;                 ///< Maximum number of slots
    float maxWeight = 100.0f;          ///< Maximum carry weight in kg
    int currency = 0;                  ///< Currency/gold amount
};

// =============================================================================
// Item Registry
// =============================================================================

/**
 * @class ItemRegistry
 * @brief Global registry of all item definitions
 *
 * Stores ItemDef objects by ID for lookup. Typically one instance
 * exists per game session.
 */
class ItemRegistry {
public:
    /**
     * @brief Register a new item definition
     * @param def  The item definition to register
     */
    void RegisterItem(const ItemDef& def) {
        m_items[def.id] = def;
    }

    /**
     * @brief Look up an item definition by ID
     * @param id  Item type ID
     * @return    Pointer to the definition, or nullptr if not found
     */
    const ItemDef* GetItem(uint32_t id) const {
        auto it = m_items.find(id);
        return (it != m_items.end()) ? &it->second : nullptr;
    }

    /**
     * @brief Check if an item ID is registered
     */
    bool HasItem(uint32_t id) const {
        return m_items.count(id) > 0;
    }

    /**
     * @brief Get all registered item definitions
     */
    const std::unordered_map<uint32_t, ItemDef>& GetAllItems() const {
        return m_items;
    }

    /**
     * @brief Get the number of registered items
     */
    size_t GetItemCount() const { return m_items.size(); }

private:
    std::unordered_map<uint32_t, ItemDef> m_items;
};

// =============================================================================
// Inventory Operations (stateless utility functions)
// =============================================================================

/**
 * @brief Stateless operations for manipulating InventoryComponent data
 *
 * Follows the ECS pattern: components hold data, external functions provide logic.
 */
namespace InventoryOps {

    /**
     * @brief Calculate total weight of all items in inventory
     */
    inline float GetTotalWeight(const InventoryComponent& inv, const ItemRegistry& registry) {
        float total = 0.0f;
        for (const auto& slot : inv.slots) {
            if (!slot.IsEmpty()) {
                if (auto* def = registry.GetItem(slot.itemDefId)) {
                    total += def->weight * slot.count;
                }
            }
        }
        return total;
    }

    /**
     * @brief Try to add items to the inventory
     *
     * First tries to stack with existing slots, then uses empty slots.
     *
     * @param inv       Inventory to add to
     * @param registry  Item registry for looking up item properties
     * @param itemId    Item type ID to add
     * @param count     Number of items to add
     * @param eventBus  Optional EventBus to publish ItemPickedUpEvent
     * @param entityId  Entity ID for the event (only used if eventBus is set)
     * @return          Number of items actually added (may be less if full/overweight)
     */
    inline int AddItem(InventoryComponent& inv, const ItemRegistry& registry,
                       uint32_t itemId, int count = 1,
                       EventBus* eventBus = nullptr, uint32_t entityId = 0) {
        const ItemDef* def = registry.GetItem(itemId);
        if (!def || count <= 0) return 0;

        // Check weight limit
        float currentWeight = GetTotalWeight(inv, registry);
        float addedWeight = def->weight * count;
        if (currentWeight + addedWeight > inv.maxWeight) {
            // Reduce count to fit weight
            int fittable = static_cast<int>((inv.maxWeight - currentWeight) / def->weight);
            if (fittable <= 0) return 0;
            count = std::min(count, fittable);
        }

        int remaining = count;

        // Try to stack with existing slots
        for (auto& slot : inv.slots) {
            if (remaining <= 0) break;
            if (slot.itemDefId == itemId && slot.count < def->maxStackSize) {
                int canAdd = std::min(remaining, def->maxStackSize - slot.count);
                slot.count += canAdd;
                remaining -= canAdd;
            }
        }

        // Use empty slots
        while (remaining > 0 && static_cast<int>(inv.slots.size()) < inv.maxSlots) {
            int stackSize = std::min(remaining, def->maxStackSize);
            inv.slots.push_back({itemId, stackSize});
            remaining -= stackSize;
        }

        int added = count - remaining;

        // Publish item picked up event
        if (added > 0 && eventBus) {
            ItemPickedUpEvent evt;
            evt.entityId = entityId;
            evt.itemDefId = itemId;
            evt.count = added;
            eventBus->Publish(evt);
        }

        return added;
    }

    /**
     * @brief Remove items from inventory
     *
     * @param inv     Inventory to remove from
     * @param itemId  Item type ID to remove
     * @param count   Number to remove
     * @return        Number actually removed
     */
    inline int RemoveItem(InventoryComponent& inv, uint32_t itemId, int count = 1) {
        if (count <= 0) return 0;
        int remaining = count;

        for (auto it = inv.slots.begin(); it != inv.slots.end() && remaining > 0; ) {
            if (it->itemDefId == itemId) {
                int toRemove = std::min(remaining, it->count);
                it->count -= toRemove;
                remaining -= toRemove;
                if (it->count <= 0) {
                    it = inv.slots.erase(it);
                    continue;
                }
            }
            ++it;
        }

        return count - remaining;
    }

    /**
     * @brief Count total items of a given type in inventory
     */
    inline int CountItem(const InventoryComponent& inv, uint32_t itemId) {
        int total = 0;
        for (const auto& slot : inv.slots) {
            if (slot.itemDefId == itemId)
                total += slot.count;
        }
        return total;
    }

    /**
     * @brief Check if inventory contains at least N of an item
     */
    inline bool HasItem(const InventoryComponent& inv, uint32_t itemId, int minCount = 1) {
        return CountItem(inv, itemId) >= minCount;
    }

    /**
     * @brief Find the first slot index containing a given item
     * @return Slot index, or -1 if not found
     */
    inline int FindItem(const InventoryComponent& inv, uint32_t itemId) {
        for (int i = 0; i < static_cast<int>(inv.slots.size()); ++i) {
            if (inv.slots[i].itemDefId == itemId)
                return i;
        }
        return -1;
    }

    /**
     * @brief Remove all empty slots (compact the inventory)
     */
    inline void Compact(InventoryComponent& inv) {
        inv.slots.erase(
            std::remove_if(inv.slots.begin(), inv.slots.end(),
                [](const ItemStack& s) { return s.IsEmpty(); }),
            inv.slots.end()
        );
    }

    /**
     * @brief Clear the entire inventory
     */
    inline void Clear(InventoryComponent& inv) {
        inv.slots.clear();
    }

    /**
     * @brief Check if the inventory is full (no empty slots and no stackable space)
     */
    inline bool IsFull(const InventoryComponent& inv) {
        return static_cast<int>(inv.slots.size()) >= inv.maxSlots;
    }

} // namespace InventoryOps

// =============================================================================
// Loot Tables
// =============================================================================

/**
 * @brief A single entry in a loot table
 */
struct LootTableEntry {
    uint32_t itemDefId = 0;            ///< Item to drop
    int minCount = 1;                  ///< Minimum drop count
    int maxCount = 1;                  ///< Maximum drop count
    float weight = 1.0f;              ///< Relative drop weight (higher = more likely)
};

/**
 * @class LootTable
 * @brief Weighted random item selection for enemy drops, chest contents, etc.
 *
 * @code
 *   LootTable loot;
 *   loot.AddEntry({1, 1, 3, 10.0f});  // Health potion: 1-3, weight 10
 *   loot.AddEntry({2, 1, 1, 2.0f});   // Sword: 1, weight 2
 *
 *   auto drops = loot.Roll(rng, 2);   // Roll for 2 items
 * @endcode
 */
class LootTable {
public:
    void AddEntry(const LootTableEntry& entry) {
        m_entries.push_back(entry);
        m_totalWeight += entry.weight;
    }

    /**
     * @brief Roll the loot table for random items
     *
     * @param rng       Random engine
     * @param numRolls  Number of items to roll for
     * @return          Vector of (itemDefId, count) pairs
     */
    std::vector<ItemStack> Roll(std::mt19937& rng, int numRolls = 1) const {
        std::vector<ItemStack> results;
        if (m_entries.empty() || m_totalWeight <= 0.0f) return results;

        std::uniform_real_distribution<float> dist(0.0f, m_totalWeight);

        for (int r = 0; r < numRolls; ++r) {
            float roll = dist(rng);
            float cumulative = 0.0f;

            for (const auto& entry : m_entries) {
                cumulative += entry.weight;
                if (roll <= cumulative) {
                    std::uniform_int_distribution<int> countDist(entry.minCount, entry.maxCount);
                    results.push_back({entry.itemDefId, countDist(rng)});
                    break;
                }
            }
        }

        return results;
    }

    const std::vector<LootTableEntry>& GetEntries() const { return m_entries; }
    float GetTotalWeight() const { return m_totalWeight; }

private:
    std::vector<LootTableEntry> m_entries;
    float m_totalWeight = 0.0f;
};

} // namespace Spark
