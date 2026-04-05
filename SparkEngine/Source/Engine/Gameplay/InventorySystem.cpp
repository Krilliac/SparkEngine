/**
 * @file InventorySystem.cpp
 * @brief Implementation of the per-entity inventory management system
 */

#include "InventorySystem.h"
#include "../../Utils/LogMacros.h"
#include "../../Utils/SparkConsole.h"
#include "../Security/MemoryIntegrity.h"

#include <algorithm>

namespace Spark::Gameplay
{

    // Static sentinel for empty inventory queries
    const std::vector<InventorySlot> InventorySystem::s_emptyInventory;

    // ============================================================================
    // Singleton
    // ============================================================================

    InventorySystem& InventorySystem::GetInstance()
    {
        static InventorySystem instance;
        return instance;
    }

    // ============================================================================
    // Lifecycle
    // ============================================================================

    void InventorySystem::Initialize()
    {
        m_itemDefs.clear();
        m_inventories.clear();
        m_maxSlots.clear();

        Spark::SimpleConsole::GetInstance().LogInfo("[InventorySystem] Initialized");
        SPARK_LOG_INFO(Spark::LogCategory::Game, "InventorySystem initialized");
    }

    void InventorySystem::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game, "InventorySystem shutting down (%zu item defs, %zu inventories)",
                       m_itemDefs.size(), m_inventories.size());

        m_itemDefs.clear();
        m_inventories.clear();
        m_maxSlots.clear();
    }

    // ============================================================================
    // Item Registry
    // ============================================================================

    void InventorySystem::RegisterItem(const ItemDefinition& def)
    {
        if (def.itemId == 0)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "InventorySystem: Cannot register item with ID 0");
            return;
        }

        m_itemDefs[def.itemId] = def;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "InventorySystem: Registered item '%s' (id=%u, stack=%u)",
                       def.name.c_str(), def.itemId, def.maxStackSize);
    }

    const ItemDefinition* InventorySystem::GetItemDef(uint32_t itemId) const
    {
        auto it = m_itemDefs.find(itemId);
        return (it != m_itemDefs.end()) ? &it->second : nullptr;
    }

    // ============================================================================
    // Per-Entity Inventory
    // ============================================================================

    bool InventorySystem::AddItem(uint32_t entityId, uint32_t itemId, uint32_t count)
    {
        // Memory integrity: bypassing item validation allows adding
        // non-existent items or bypassing stack/capacity limits
        SPARK_INTEGRITY_CHECKPOINT("inventory_add_item");

        if (count == 0)
        {
            return true;
        }

        const ItemDefinition* def = GetItemDef(itemId);
        if (!def)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "InventorySystem: Unknown item ID %u", itemId);
            return false;
        }

        auto& slots = m_inventories[entityId];
        uint32_t maxSlots = kDefaultMaxSlots;
        auto maxIt = m_maxSlots.find(entityId);
        if (maxIt != m_maxSlots.end())
        {
            maxSlots = maxIt->second;
        }

        uint32_t remaining = count;

        // First pass: stack into existing slots with the same item
        for (auto& slot : slots)
        {
            if (remaining == 0)
            {
                break;
            }
            if (slot.itemId == itemId && slot.count < def->maxStackSize)
            {
                uint32_t space = def->maxStackSize - slot.count;
                uint32_t toAdd = std::min(remaining, space);
                slot.count += toAdd;
                remaining -= toAdd;
            }
        }

        // Second pass: fill empty or create new slots
        while (remaining > 0)
        {
            // Try to find an existing empty slot
            InventorySlot* emptySlot = nullptr;
            for (auto& slot : slots)
            {
                if (slot.IsEmpty())
                {
                    emptySlot = &slot;
                    break;
                }
            }

            if (!emptySlot)
            {
                // Need a new slot — check capacity (bypassing allows unlimited inventory)
                SPARK_BRANCH_GUARD_BEGIN("inventory_capacity_check")
                if (static_cast<uint32_t>(slots.size()) >= maxSlots)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Game,
                                   "InventorySystem: Entity %u inventory full (%u/%u slots), %u items not added",
                                   entityId, static_cast<uint32_t>(slots.size()), maxSlots, remaining);
                    return false;
                }
                SPARK_BRANCH_GUARD_END("inventory_capacity_check")
                slots.push_back({});
                emptySlot = &slots.back();
            }

            uint32_t toAdd = std::min(remaining, def->maxStackSize);
            emptySlot->itemId = itemId;
            emptySlot->count = toAdd;
            remaining -= toAdd;
        }

        SPARK_VERIFY_CHECKPOINT("inventory_add_item");
        return true;
    }

    bool InventorySystem::RemoveItem(uint32_t entityId, uint32_t itemId, uint32_t count)
    {
        if (count == 0)
        {
            return true;
        }

        auto invIt = m_inventories.find(entityId);
        if (invIt == m_inventories.end())
        {
            return false;
        }

        // Verify we have enough before modifying — bypassing allows
        // negative item counts and duplication exploits
        SPARK_BRANCH_GUARD_BEGIN("inventory_remove_validation")
        if (GetItemCount(entityId, itemId) < count)
        {
            return false;
        }
        SPARK_BRANCH_GUARD_END("inventory_remove_validation")

        auto& slots = invIt->second;
        uint32_t remaining = count;

        // Remove from slots back-to-front to prefer clearing later slots
        for (int i = static_cast<int>(slots.size()) - 1; i >= 0 && remaining > 0; --i)
        {
            auto& slot = slots[static_cast<size_t>(i)];
            if (slot.itemId != itemId)
            {
                continue;
            }

            uint32_t toRemove = std::min(remaining, slot.count);
            slot.count -= toRemove;
            remaining -= toRemove;

            if (slot.count == 0)
            {
                slot.itemId = 0;
            }
        }

        return remaining == 0;
    }

    uint32_t InventorySystem::GetItemCount(uint32_t entityId, uint32_t itemId) const
    {
        auto invIt = m_inventories.find(entityId);
        if (invIt == m_inventories.end())
        {
            return 0;
        }

        uint32_t total = 0;
        for (const auto& slot : invIt->second)
        {
            if (slot.itemId == itemId)
            {
                total += slot.count;
            }
        }
        return total;
    }

    bool InventorySystem::HasItem(uint32_t entityId, uint32_t itemId, uint32_t count) const
    {
        return GetItemCount(entityId, itemId) >= count;
    }

    const std::vector<InventorySlot>& InventorySystem::GetInventory(uint32_t entityId) const
    {
        auto invIt = m_inventories.find(entityId);
        if (invIt == m_inventories.end())
        {
            return s_emptyInventory;
        }
        return invIt->second;
    }

    // ============================================================================
    // Capacity
    // ============================================================================

    void InventorySystem::SetMaxSlots(uint32_t entityId, uint32_t maxSlots)
    {
        m_maxSlots[entityId] = maxSlots;
    }

    uint32_t InventorySystem::GetUsedSlots(uint32_t entityId) const
    {
        auto invIt = m_inventories.find(entityId);
        if (invIt == m_inventories.end())
        {
            return 0;
        }

        uint32_t used = 0;
        for (const auto& slot : invIt->second)
        {
            if (!slot.IsEmpty())
            {
                ++used;
            }
        }
        return used;
    }

    bool InventorySystem::IsFull(uint32_t entityId) const
    {
        uint32_t maxSlots = kDefaultMaxSlots;
        auto maxIt = m_maxSlots.find(entityId);
        if (maxIt != m_maxSlots.end())
        {
            maxSlots = maxIt->second;
        }

        auto invIt = m_inventories.find(entityId);
        if (invIt == m_inventories.end())
        {
            return maxSlots == 0;
        }

        // Full if every slot is occupied and we're at capacity
        uint32_t used = GetUsedSlots(entityId);
        return used >= maxSlots;
    }

    // ============================================================================
    // Transfer
    // ============================================================================

    bool InventorySystem::TransferItem(uint32_t fromEntity, uint32_t toEntity, uint32_t itemId, uint32_t count)
    {
        if (!HasItem(fromEntity, itemId, count))
        {
            return false;
        }

        // Try adding to destination first (non-destructive check)
        if (!AddItem(toEntity, itemId, count))
        {
            return false;
        }

        // Committed — remove from source (guaranteed to succeed since we checked)
        RemoveItem(fromEntity, itemId, count);
        return true;
    }

    // ============================================================================
    // Console
    // ============================================================================

    std::string InventorySystem::Console_GetStatus() const
    {
        std::string s = "Inventory System:\n";
        s += "  Registered items: " + std::to_string(m_itemDefs.size()) + "\n";
        s += "  Active inventories: " + std::to_string(m_inventories.size()) + "\n";

        for (const auto& [entityId, slots] : m_inventories)
        {
            uint32_t used = 0;
            for (const auto& slot : slots)
            {
                if (!slot.IsEmpty())
                {
                    ++used;
                }
            }

            uint32_t maxSlots = kDefaultMaxSlots;
            auto maxIt = m_maxSlots.find(entityId);
            if (maxIt != m_maxSlots.end())
            {
                maxSlots = maxIt->second;
            }

            s += "  Entity " + std::to_string(entityId) + ": " + std::to_string(used) + "/" + std::to_string(maxSlots) +
                 " slots\n";
        }

        return s;
    }

} // namespace Spark::Gameplay
