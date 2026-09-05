/**
 * @file InventorySystem.h
 * @brief Reusable per-entity inventory management with item registry and stacking
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a generic inventory system that can be used across game genres:
 * - Centralized item definition registry (ItemDefinition)
 * - Per-entity inventories with slot-based storage and stacking
 * - Capacity limits, item transfer between entities
 * - Console integration for debugging
 *
 * ## Usage
 * @code
 *   auto& inv = InventorySystem::GetInstance();
 *   inv.Initialize();
 *
 *   ItemDefinition sword;
 *   sword.itemId = 1;
 *   sword.name = "Iron Sword";
 *   sword.maxStackSize = 1;
 *   sword.isEquippable = true;
 *   inv.RegisterItem(sword);
 *
 *   inv.SetMaxSlots(playerEntity, 20);
 *   inv.AddItem(playerEntity, 1);
 * @endcode
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Gameplay
{

    // ============================================================================
    // Item Definition (static registry data)
    // ============================================================================

    struct ItemDefinition
    {
        uint32_t itemId = 0;
        std::string name;
        std::string description;

        enum class Rarity : uint8_t
        {
            Common,
            Uncommon,
            Rare,
            Epic,
            Legendary
        };

        Rarity rarity = Rarity::Common;
        float weight = 0.0f;
        uint32_t maxStackSize = 1;
        bool isConsumable = false;
        bool isEquippable = false;
    };

    // ============================================================================
    // Inventory Slot (runtime per-entity state)
    // ============================================================================

    struct InventorySlot
    {
        uint32_t itemId = 0;
        uint32_t count = 0;

        /// @brief Returns true if this slot contains no items
        [[nodiscard]] bool IsEmpty() const { return itemId == 0 || count == 0; }
    };

    /** @brief Serializable per-entity inventory state (slot layout plus capacity). */
    struct InventorySnapshot
    {
        uint32_t maxSlots = 0;            ///< Capacity at capture time. 0 = the entity had no inventory.
        std::vector<InventorySlot> slots; ///< Slots in capture order, including empty ones.
    };

    // ============================================================================
    // Inventory System
    // ============================================================================

    /**
     * @brief Manages item definitions and per-entity inventories with stacking and capacity.
     *
     * Singleton. Call Initialize() at engine startup, Shutdown() on teardown.
     * Items are registered globally; inventories are tracked per entity ID.
     */
    class InventorySystem
    {
      public:
        static InventorySystem& GetInstance();

        /// @brief Initialize the inventory system, clearing all state
        void Initialize();

        /// @brief Shut down the inventory system
        void Shutdown();

        // -- Item Registry --

        /// @brief Register a new item definition
        void RegisterItem(const ItemDefinition& def);

        /// @brief Look up an item definition by ID
        /// @return Pointer to the definition, or nullptr if not found
        [[nodiscard]] const ItemDefinition* GetItemDef(uint32_t itemId) const;

        // -- Per-Entity Inventory --

        /// @brief Add items to an entity's inventory, respecting stacking and capacity
        /// @return true if all items were added successfully
        bool AddItem(uint32_t entityId, uint32_t itemId, uint32_t count = 1);

        /// @brief Remove items from an entity's inventory
        /// @return true if the requested count was removed
        bool RemoveItem(uint32_t entityId, uint32_t itemId, uint32_t count = 1);

        /// @brief Get total count of a specific item in an entity's inventory
        [[nodiscard]] uint32_t GetItemCount(uint32_t entityId, uint32_t itemId) const;

        /// @brief Check if an entity has at least count of an item
        [[nodiscard]] bool HasItem(uint32_t entityId, uint32_t itemId, uint32_t count = 1) const;

        /// @brief Get all inventory slots for an entity
        [[nodiscard]] const std::vector<InventorySlot>& GetInventory(uint32_t entityId) const;

        // -- Capacity --

        /// @brief Set maximum number of inventory slots for an entity
        void SetMaxSlots(uint32_t entityId, uint32_t maxSlots);

        /// @brief Get number of non-empty slots
        [[nodiscard]] uint32_t GetUsedSlots(uint32_t entityId) const;

        /// @brief Check if all slots are occupied
        [[nodiscard]] bool IsFull(uint32_t entityId) const;

        // -- Transfer --

        /// @brief Move items from one entity's inventory to another
        /// @return true if the transfer succeeded
        bool TransferItem(uint32_t fromEntity, uint32_t toEntity, uint32_t itemId, uint32_t count = 1);

        // -- Persistence --

        /// @brief Capture all inventory state for one entity, including empty slots.
        /// @return A snapshot with maxSlots == 0 when the entity owns no inventory state.
        [[nodiscard]] InventorySnapshot CaptureEntityState(uint32_t entityId) const;

        /// @brief Validate a snapshot against registered item definitions without changing live state.
        [[nodiscard]] bool ValidateEntityState(const InventorySnapshot& snapshot) const;

        /// @brief Atomically replace one entity's inventory after validating the snapshot.
        /// @return false (leaving live state untouched) when the snapshot is invalid.
        bool RestoreEntityState(uint32_t entityId, const InventorySnapshot& snapshot);

        /// @brief Remove all inventory state owned by an entity that is being destroyed or replaced.
        void ClearEntityState(uint32_t entityId);

        // -- Console --

        /// @brief Get a human-readable status string for debugging
        [[nodiscard]] std::string Console_GetStatus() const;

      private:
        InventorySystem() = default;

        /// @brief Default slot capacity for new inventories
        static constexpr uint32_t kDefaultMaxSlots = 20;

        // Item definition registry
        std::unordered_map<uint32_t, ItemDefinition> m_itemDefs;

        // Per-entity inventory slots
        std::unordered_map<uint32_t, std::vector<InventorySlot>> m_inventories;

        // Per-entity max slot count
        std::unordered_map<uint32_t, uint32_t> m_maxSlots;

        // Sentinel returned for entities with no inventory
        static const std::vector<InventorySlot> s_emptyInventory;
    };

} // namespace Spark::Gameplay
