// TestInventorySystem.cpp - Tests for inventory, items, stacking, and loot tables
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <random>
#include <cstdint>

namespace TestInventory
{

    enum class ItemRarity
    {
        Common,
        Uncommon,
        Rare,
        Epic,
        Legendary
    };

    struct ItemDef
    {
        uint32_t id = 0;
        std::string name;
        int maxStackSize = 1;
        float weight = 1.0f;
        ItemRarity rarity = ItemRarity::Common;
    };

    struct ItemStack
    {
        uint32_t itemDefId = 0;
        int count = 0;
        bool IsEmpty() const { return count <= 0 || itemDefId == 0; }
    };

    struct InventoryComponent
    {
        std::vector<ItemStack> slots;
        int maxSlots = 20;
        float maxWeight = 100.0f;
    };

    class ItemRegistry
    {
      public:
        void RegisterItem(const ItemDef& def) { m_items[def.id] = def; }
        const ItemDef* GetItem(uint32_t id) const
        {
            auto it = m_items.find(id);
            return (it != m_items.end()) ? &it->second : nullptr;
        }
        size_t GetItemCount() const { return m_items.size(); }

      private:
        std::unordered_map<uint32_t, ItemDef> m_items;
    };

    namespace InventoryOps
    {

        inline float GetTotalWeight(const InventoryComponent& inv, const ItemRegistry& reg)
        {
            float total = 0.0f;
            for (const auto& slot : inv.slots)
            {
                if (!slot.IsEmpty())
                {
                    if (auto* def = reg.GetItem(slot.itemDefId))
                        total += def->weight * slot.count;
                }
            }
            return total;
        }

        inline int AddItem(InventoryComponent& inv, const ItemRegistry& reg, uint32_t itemId, int count = 1)
        {
            const ItemDef* def = reg.GetItem(itemId);
            if (!def || count <= 0)
                return 0;

            float currentWeight = GetTotalWeight(inv, reg);
            float addedWeight = def->weight * count;
            if (currentWeight + addedWeight > inv.maxWeight)
            {
                int fittable = static_cast<int>((inv.maxWeight - currentWeight) / def->weight);
                if (fittable <= 0)
                    return 0;
                count = std::min(count, fittable);
            }

            int remaining = count;
            for (auto& slot : inv.slots)
            {
                if (remaining <= 0)
                    break;
                if (slot.itemDefId == itemId && slot.count < def->maxStackSize)
                {
                    int canAdd = std::min(remaining, def->maxStackSize - slot.count);
                    slot.count += canAdd;
                    remaining -= canAdd;
                }
            }
            while (remaining > 0 && static_cast<int>(inv.slots.size()) < inv.maxSlots)
            {
                int stackSize = std::min(remaining, def->maxStackSize);
                inv.slots.push_back({itemId, stackSize});
                remaining -= stackSize;
            }
            return count - remaining;
        }

        inline int RemoveItem(InventoryComponent& inv, uint32_t itemId, int count = 1)
        {
            if (count <= 0)
                return 0;
            int remaining = count;
            for (auto it = inv.slots.begin(); it != inv.slots.end() && remaining > 0;)
            {
                if (it->itemDefId == itemId)
                {
                    int toRemove = std::min(remaining, it->count);
                    it->count -= toRemove;
                    remaining -= toRemove;
                    if (it->count <= 0)
                    {
                        it = inv.slots.erase(it);
                        continue;
                    }
                }
                ++it;
            }
            return count - remaining;
        }

        inline int CountItem(const InventoryComponent& inv, uint32_t itemId)
        {
            int total = 0;
            for (const auto& slot : inv.slots)
                if (slot.itemDefId == itemId)
                    total += slot.count;
            return total;
        }

        inline bool HasItem(const InventoryComponent& inv, uint32_t itemId, int minCount = 1)
        {
            return CountItem(inv, itemId) >= minCount;
        }

        inline int FindItem(const InventoryComponent& inv, uint32_t itemId)
        {
            for (int i = 0; i < static_cast<int>(inv.slots.size()); ++i)
                if (inv.slots[i].itemDefId == itemId)
                    return i;
            return -1;
        }

    } // namespace InventoryOps

    struct LootTableEntry
    {
        uint32_t itemDefId = 0;
        int minCount = 1;
        int maxCount = 1;
        float weight = 1.0f;
    };

    class LootTable
    {
      public:
        void AddEntry(const LootTableEntry& entry)
        {
            m_entries.push_back(entry);
            m_totalWeight += entry.weight;
        }

        std::vector<ItemStack> Roll(std::mt19937& rng, int numRolls = 1) const
        {
            std::vector<ItemStack> results;
            if (m_entries.empty())
                return results;
            std::uniform_real_distribution<float> dist(0.0f, m_totalWeight);
            for (int r = 0; r < numRolls; ++r)
            {
                float roll = dist(rng);
                float cum = 0.0f;
                for (const auto& entry : m_entries)
                {
                    cum += entry.weight;
                    if (roll <= cum)
                    {
                        std::uniform_int_distribution<int> countDist(entry.minCount, entry.maxCount);
                        results.push_back({entry.itemDefId, countDist(rng)});
                        break;
                    }
                }
            }
            return results;
        }

        float GetTotalWeight() const { return m_totalWeight; }

      private:
        std::vector<LootTableEntry> m_entries;
        float m_totalWeight = 0.0f;
    };

} // namespace TestInventory

// =============================================================================
// Tests
// =============================================================================

TEST(Inventory_AddItem)
{
    TestInventory::ItemRegistry reg;
    reg.RegisterItem({1, "Potion", 10, 0.5f});
    TestInventory::InventoryComponent inv;

    int added = TestInventory::InventoryOps::AddItem(inv, reg, 1, 5);
    EXPECT_EQ(added, 5);
    EXPECT_EQ(TestInventory::InventoryOps::CountItem(inv, 1), 5);
}

TEST(Inventory_StackingLimit)
{
    TestInventory::ItemRegistry reg;
    reg.RegisterItem({1, "Potion", 5, 0.1f}); // Max stack 5
    TestInventory::InventoryComponent inv;
    inv.maxSlots = 3;

    int added = TestInventory::InventoryOps::AddItem(inv, reg, 1, 12);
    EXPECT_EQ(added, 12);
    // Should be spread across 3 slots: 5 + 5 + 2
    EXPECT_EQ((int)inv.slots.size(), 3);

    // Try adding more than capacity
    added = TestInventory::InventoryOps::AddItem(inv, reg, 1, 5);
    EXPECT_EQ(added, 3); // Only 3 more fit (slot 3: 2+3=5)
}

TEST(Inventory_WeightLimit)
{
    TestInventory::ItemRegistry reg;
    reg.RegisterItem({1, "Heavy Rock", 99, 10.0f}); // 10kg each
    TestInventory::InventoryComponent inv;
    inv.maxWeight = 25.0f;

    int added = TestInventory::InventoryOps::AddItem(inv, reg, 1, 5);
    EXPECT_EQ(added, 2); // Only 2 fit within 25kg

    float weight = TestInventory::InventoryOps::GetTotalWeight(inv, reg);
    EXPECT_NEAR(weight, 20.0f, 0.01f);
}

TEST(Inventory_RemoveItem)
{
    TestInventory::ItemRegistry reg;
    reg.RegisterItem({1, "Potion", 10, 0.5f});
    TestInventory::InventoryComponent inv;

    TestInventory::InventoryOps::AddItem(inv, reg, 1, 8);
    int removed = TestInventory::InventoryOps::RemoveItem(inv, 1, 3);
    EXPECT_EQ(removed, 3);
    EXPECT_EQ(TestInventory::InventoryOps::CountItem(inv, 1), 5);
}

TEST(Inventory_RemoveMoreThanExists)
{
    TestInventory::ItemRegistry reg;
    reg.RegisterItem({1, "Potion", 10, 0.5f});
    TestInventory::InventoryComponent inv;

    TestInventory::InventoryOps::AddItem(inv, reg, 1, 3);
    int removed = TestInventory::InventoryOps::RemoveItem(inv, 1, 10);
    EXPECT_EQ(removed, 3);
    EXPECT_EQ(TestInventory::InventoryOps::CountItem(inv, 1), 0);
    EXPECT_TRUE(inv.slots.empty());
}

TEST(Inventory_HasItem)
{
    TestInventory::ItemRegistry reg;
    reg.RegisterItem({1, "Potion", 10, 0.5f});
    TestInventory::InventoryComponent inv;

    EXPECT_FALSE(TestInventory::InventoryOps::HasItem(inv, 1));

    TestInventory::InventoryOps::AddItem(inv, reg, 1, 5);
    EXPECT_TRUE(TestInventory::InventoryOps::HasItem(inv, 1));
    EXPECT_TRUE(TestInventory::InventoryOps::HasItem(inv, 1, 5));
    EXPECT_FALSE(TestInventory::InventoryOps::HasItem(inv, 1, 6));
}

TEST(Inventory_FindItem)
{
    TestInventory::ItemRegistry reg;
    reg.RegisterItem({1, "Potion", 10, 0.5f});
    reg.RegisterItem({2, "Sword", 1, 3.0f});
    TestInventory::InventoryComponent inv;

    EXPECT_EQ(TestInventory::InventoryOps::FindItem(inv, 1), -1);

    TestInventory::InventoryOps::AddItem(inv, reg, 1, 3);
    TestInventory::InventoryOps::AddItem(inv, reg, 2, 1);
    EXPECT_EQ(TestInventory::InventoryOps::FindItem(inv, 1), 0);
    EXPECT_EQ(TestInventory::InventoryOps::FindItem(inv, 2), 1);
    EXPECT_EQ(TestInventory::InventoryOps::FindItem(inv, 99), -1);
}

TEST(Inventory_MultipleItemTypes)
{
    TestInventory::ItemRegistry reg;
    reg.RegisterItem({1, "Potion", 10, 0.5f});
    reg.RegisterItem({2, "Sword", 1, 3.0f});
    reg.RegisterItem({3, "Shield", 1, 5.0f});
    TestInventory::InventoryComponent inv;

    TestInventory::InventoryOps::AddItem(inv, reg, 1, 5);
    TestInventory::InventoryOps::AddItem(inv, reg, 2, 1);
    TestInventory::InventoryOps::AddItem(inv, reg, 3, 1);

    EXPECT_EQ(TestInventory::InventoryOps::CountItem(inv, 1), 5);
    EXPECT_EQ(TestInventory::InventoryOps::CountItem(inv, 2), 1);
    EXPECT_EQ(TestInventory::InventoryOps::CountItem(inv, 3), 1);
    EXPECT_EQ((int)inv.slots.size(), 3);
}

TEST(Inventory_ItemRegistry)
{
    TestInventory::ItemRegistry reg;
    reg.RegisterItem({1, "Potion", 10, 0.5f});
    reg.RegisterItem({2, "Sword", 1, 3.0f});

    EXPECT_EQ(reg.GetItemCount(), (size_t)2);

    auto* potion = reg.GetItem(1);
    EXPECT_TRUE(potion != nullptr);
    EXPECT_EQ(potion->name, std::string("Potion"));
    EXPECT_EQ(potion->maxStackSize, 10);

    EXPECT_TRUE(reg.GetItem(99) == nullptr);
}

TEST(Inventory_LootTableRoll)
{
    TestInventory::LootTable loot;
    loot.AddEntry({1, 1, 3, 10.0f});
    loot.AddEntry({2, 1, 1, 2.0f});

    EXPECT_GT(loot.GetTotalWeight(), 0.0f);

    std::mt19937 rng(42); // Fixed seed for reproducibility
    auto drops = loot.Roll(rng, 5);
    EXPECT_EQ((int)drops.size(), 5);

    for (const auto& drop : drops)
    {
        EXPECT_TRUE(drop.itemDefId == 1 || drop.itemDefId == 2);
        EXPECT_GT(drop.count, 0);
    }
}

TEST(Inventory_AddInvalidItem)
{
    TestInventory::ItemRegistry reg;
    TestInventory::InventoryComponent inv;

    int added = TestInventory::InventoryOps::AddItem(inv, reg, 999, 1);
    EXPECT_EQ(added, 0);
    EXPECT_TRUE(inv.slots.empty());
}
