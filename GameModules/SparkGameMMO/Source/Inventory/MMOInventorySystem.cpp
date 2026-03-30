/**
 * @file MMOInventorySystem.cpp
 * @brief Item registry, inventory operations, and loot table implementation
 */

#include "MMOInventorySystem.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <sstream>

namespace MMO
{

    bool MMOInventorySystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterDefaultItems();
        RegisterDefaultLootTables();

        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[MMO] Inventory system initialized (" + std::to_string(m_items.size()) + " items)");
        return true;
    }

    void MMOInventorySystem::Shutdown()
    {
        m_items.clear();
        m_lootTables.clear();
    }

    void MMOInventorySystem::RegisterDefaultItems()
    {
        // Consumables
        RegisterItem(
            {1, "Health Potion", "Restores 50 HP", ItemCategory::Consumable, ItemRarity::Common, 10, 0.5f, 5, false});
        RegisterItem(
            {2, "Mana Elixir", "Restores 30 MP", ItemCategory::Consumable, ItemRarity::Common, 10, 0.5f, 8, false});
        RegisterItem({3, "Shield Potion", "Grants 25 shield", ItemCategory::Consumable, ItemRarity::Uncommon, 5, 0.5f,
                      15, false});
        RegisterItem({4, "Combat Ration", "Boosts stamina regen for 60s", ItemCategory::Consumable, ItemRarity::Common,
                      5, 0.3f, 3, false});
        RegisterItem(
            {5, "Bandage", "Heals 20 HP over 10s", ItemCategory::Consumable, ItemRarity::Common, 20, 0.1f, 1, false});

        // Materials
        RegisterItem(
            {10, "Herb", "Common healing herb", ItemCategory::Material, ItemRarity::Common, 50, 0.1f, 1, false});
        RegisterItem({11, "Glass Vial", "Empty container for potions", ItemCategory::Material, ItemRarity::Common, 20,
                      0.2f, 2, false});
        RegisterItem({12, "Crystal Shard", "Shimmering magical crystal", ItemCategory::Material, ItemRarity::Uncommon,
                      20, 0.3f, 10, false});
        RegisterItem(
            {13, "Iron Ore", "Raw iron for smelting", ItemCategory::Material, ItemRarity::Common, 30, 1.0f, 3, false});
        RegisterItem({14, "Leather Strip", "Tanned leather strip", ItemCategory::Material, ItemRarity::Common, 30, 0.2f,
                      2, false});
        RegisterItem({15, "Steel Ingot", "Refined steel bar", ItemCategory::Material, ItemRarity::Uncommon, 20, 1.5f,
                      12, false});
        RegisterItem({16, "Precision Tool", "Fine engineering tool", ItemCategory::Material, ItemRarity::Rare, 5, 0.5f,
                      25, false});
        RegisterItem(
            {17, "Gunpowder", "Explosive propellant", ItemCategory::Material, ItemRarity::Common, 30, 0.3f, 5, false});
        RegisterItem({18, "Circuit Board", "Electronic component", ItemCategory::Material, ItemRarity::Rare, 10, 0.2f,
                      30, false});
        RegisterItem({19, "Raw Meat", "Uncooked meat", ItemCategory::Material, ItemRarity::Common, 20, 0.5f, 2, false});
        RegisterItem(
            {20, "Cloth Scrap", "Torn fabric scrap", ItemCategory::Material, ItemRarity::Common, 50, 0.1f, 1, false});

        // Weapons
        RegisterItem(
            {30, "Iron Sword", "A sturdy blade", ItemCategory::Weapon, ItemRarity::Common, 1, 3.0f, 25, false});
        RegisterItem({31, "Steel Rifle Barrel", "Precision machined barrel", ItemCategory::Weapon, ItemRarity::Rare, 1,
                      2.0f, 80, false});
        RegisterItem({32, "Plasma Rifle", "High-energy plasma weapon", ItemCategory::Weapon, ItemRarity::Epic, 1, 5.0f,
                      200, false});

        // Crafted items
        RegisterItem({40, "Frag Grenade", "Explosive fragmentation grenade", ItemCategory::Weapon, ItemRarity::Uncommon,
                      5, 0.5f, 15, false});
        RegisterItem({41, "Turret Kit", "Deployable automated turret", ItemCategory::Weapon, ItemRarity::Rare, 1, 5.0f,
                      100, false});
        RegisterItem({50, "Combat Ration (Cooked)", "Prepared meal, stamina boost", ItemCategory::Consumable,
                      ItemRarity::Common, 10, 0.3f, 5, false});
        RegisterItem({51, "Field Bandage", "Quick-apply bandage", ItemCategory::Consumable, ItemRarity::Common, 20,
                      0.1f, 2, false});

        // Quest items
        RegisterItem({100, "Ancient Key", "Opens the Shadow Crypt gate", ItemCategory::Quest, ItemRarity::Epic, 1, 0.1f,
                      0, true});
        RegisterItem({101, "Commander's Sigil", "Proof of military rank", ItemCategory::Quest, ItemRarity::Rare, 1,
                      0.1f, 0, true});

        // Armor
        RegisterItem(
            {60, "Iron Chestplate", "Basic iron armor", ItemCategory::Armor, ItemRarity::Common, 1, 8.0f, 40, false});
        RegisterItem({61, "Steel Helmet", "Reinforced steel helmet", ItemCategory::Armor, ItemRarity::Uncommon, 1, 3.0f,
                      35, false});
        RegisterItem({62, "Shadow Cloak", "Cloak woven from shadow threads", ItemCategory::Armor, ItemRarity::Epic, 1,
                      1.0f, 150, false});
    }

    void MMOInventorySystem::RegisterDefaultLootTables()
    {
        // Standard mob drops
        RegisterLootTable("mob_common", {
                                            {10, 1, 3, 10.0f}, // Herb
                                            {13, 1, 2, 8.0f},  // Iron Ore
                                            {14, 1, 2, 6.0f},  // Leather Strip
                                            {20, 1, 3, 10.0f}, // Cloth Scrap
                                            {19, 1, 1, 5.0f},  // Raw Meat
                                            {1, 1, 1, 4.0f},   // Health Potion
                                        });

        // Elite mob drops
        RegisterLootTable("mob_elite", {
                                           {15, 1, 2, 8.0f}, // Steel Ingot
                                           {12, 1, 1, 6.0f}, // Crystal Shard
                                           {3, 1, 1, 5.0f},  // Shield Potion
                                           {30, 1, 1, 2.0f}, // Iron Sword
                                           {60, 1, 1, 2.0f}, // Iron Chestplate
                                       });

        // Boss drops
        RegisterLootTable("boss_loot", {
                                           {32, 1, 1, 2.0f}, // Plasma Rifle
                                           {62, 1, 1, 2.0f}, // Shadow Cloak
                                           {16, 1, 2, 5.0f}, // Precision Tool
                                           {18, 1, 1, 4.0f}, // Circuit Board
                                           {12, 2, 5, 8.0f}, // Crystal Shard
                                       });
    }

    void MMOInventorySystem::RegisterItem(const ItemDef& def)
    {
        m_items[def.id] = def;
    }

    const ItemDef* MMOInventorySystem::GetItem(uint32_t id) const
    {
        auto it = m_items.find(id);
        return it != m_items.end() ? &it->second : nullptr;
    }

    float MMOInventorySystem::GetTotalWeight(const InventoryData& inv) const
    {
        float total = 0.0f;
        for (const auto& slot : inv.slots)
        {
            if (!slot.IsEmpty())
            {
                if (auto* def = GetItem(slot.itemDefId))
                    total += def->weight * slot.count;
            }
        }
        return total;
    }

    int MMOInventorySystem::AddItem(InventoryData& inv, uint32_t itemId, int count)
    {
        const ItemDef* def = GetItem(itemId);
        if (!def || count <= 0)
            return 0;

        // Weight check
        float currentWeight = GetTotalWeight(inv);
        float addWeight = def->weight * count;
        if (currentWeight + addWeight > inv.maxWeight)
        {
            if (def->weight <= 0.0f)
                return 0; // Zero-weight items can't reduce over-weight
            int fittable = static_cast<int>((inv.maxWeight - currentWeight) / def->weight);
            if (fittable <= 0)
                return 0;
            count = std::min(count, fittable);
        }

        int remaining = count;

        // Stack with existing
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

        // New slots
        while (remaining > 0 && static_cast<int>(inv.slots.size()) < inv.maxSlots)
        {
            int stackSize = std::min(remaining, def->maxStackSize);
            inv.slots.push_back({itemId, stackSize});
            remaining -= stackSize;
        }

        return count - remaining;
    }

    int MMOInventorySystem::RemoveItem(InventoryData& inv, uint32_t itemId, int count)
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

    int MMOInventorySystem::CountItem(const InventoryData& inv, uint32_t itemId) const
    {
        int total = 0;
        for (const auto& slot : inv.slots)
        {
            if (slot.itemDefId == itemId)
                total += slot.count;
        }
        return total;
    }

    bool MMOInventorySystem::HasItem(const InventoryData& inv, uint32_t itemId, int minCount) const
    {
        return CountItem(inv, itemId) >= minCount;
    }

    bool MMOInventorySystem::IsFull(const InventoryData& inv) const
    {
        return static_cast<int>(inv.slots.size()) >= inv.maxSlots;
    }

    void MMOInventorySystem::RegisterLootTable(const std::string& tableName, const std::vector<LootTableEntry>& entries)
    {
        LootTable table;
        table.entries = entries;
        table.totalWeight = 0.0f;
        for (const auto& e : entries)
            table.totalWeight += e.weight;
        m_lootTables[tableName] = std::move(table);
    }

    std::vector<ItemStack> MMOInventorySystem::RollLootTable(const std::string& tableName, int rolls)
    {
        std::vector<ItemStack> results;
        auto it = m_lootTables.find(tableName);
        if (it == m_lootTables.end() || it->second.entries.empty())
            return results;

        const auto& table = it->second;
        if (table.totalWeight <= 0.0f)
            return results;
        std::uniform_real_distribution<float> dist(0.0f, table.totalWeight);

        for (int r = 0; r < rolls; ++r)
        {
            float roll = dist(m_rng);
            float cumulative = 0.0f;
            for (const auto& entry : table.entries)
            {
                cumulative += entry.weight;
                if (roll <= cumulative)
                {
                    std::uniform_int_distribution<int> countDist(entry.minCount, entry.maxCount);
                    results.push_back({entry.itemDefId, countDist(m_rng)});
                    break;
                }
            }
        }
        return results;
    }

    std::string MMOInventorySystem::GetInventoryString(const InventoryData& inv) const
    {
        std::ostringstream ss;
        ss << "Slots: " << inv.slots.size() << "/" << inv.maxSlots;
        ss << " | Weight: " << GetTotalWeight(inv) << "/" << inv.maxWeight;
        ss << " | Currency: " << inv.currency << "\n";
        for (const auto& slot : inv.slots)
        {
            if (auto* def = GetItem(slot.itemDefId))
                ss << "  " << def->name << " x" << slot.count << "\n";
        }
        return ss.str();
    }

    void MMOInventorySystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("MMO Inventory System"))
        {
            ImGui::Text("Registered Items: %zu", m_items.size());
            ImGui::Text("Loot Tables: %zu", m_lootTables.size());

            if (ImGui::TreeNode("Item Registry"))
            {
                for (const auto& [id, def] : m_items)
                {
                    ImGui::Text("[%u] %s (%s) - %dg", id, def.name.c_str(), def.description.c_str(), def.sellValue);
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace MMO
