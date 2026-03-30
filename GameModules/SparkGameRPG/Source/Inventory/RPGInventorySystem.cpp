/**
 * @file RPGInventorySystem.cpp
 * @brief Item registry, inventory management, and equipment comparison
 */

#include "RPGInventorySystem.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <sstream>

namespace RPG
{

    bool RPGInventorySystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterDefaultItems();

        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Inventory system initialized (" +
                                                    std::to_string(m_items.size()) + " items)");
        return true;
    }

    void RPGInventorySystem::Shutdown()
    {
        m_items.clear();
    }

    void RPGInventorySystem::RegisterDefaultItems()
    {
        // === Consumables ===
        RegisterItem({1,
                      "Health Potion",
                      "Restores 50 HP",
                      ItemRarity::Common,
                      ItemSlot::Count,
                      0.5f,
                      5,
                      10,
                      true,
                      {},
                      50.0f,
                      0.0f});
        RegisterItem({2,
                      "Mana Elixir",
                      "Restores 40 MP",
                      ItemRarity::Common,
                      ItemSlot::Count,
                      0.5f,
                      8,
                      10,
                      true,
                      {},
                      0.0f,
                      40.0f});
        RegisterItem({3,
                      "Greater Health Potion",
                      "Restores 120 HP",
                      ItemRarity::Uncommon,
                      ItemSlot::Count,
                      0.5f,
                      15,
                      5,
                      true,
                      {},
                      120.0f,
                      0.0f});
        RegisterItem({4,
                      "Elixir of Power",
                      "Restores 30 HP and 30 MP",
                      ItemRarity::Rare,
                      ItemSlot::Count,
                      0.5f,
                      30,
                      5,
                      true,
                      {},
                      30.0f,
                      30.0f});

        // === Weapons (MainHand) ===
        RegisterItem({10,
                      "Rusty Sword",
                      "A worn blade, better than nothing",
                      ItemRarity::Common,
                      ItemSlot::MainHand,
                      3.0f,
                      10,
                      1,
                      false,
                      {3, 0, 0, 0, 0, 0},
                      0,
                      0});
        RegisterItem({11,
                      "Steel Longsword",
                      "A reliable warrior's blade",
                      ItemRarity::Uncommon,
                      ItemSlot::MainHand,
                      4.0f,
                      45,
                      1,
                      false,
                      {6, 1, 0, 0, 0, 0},
                      0,
                      0});
        RegisterItem({12,
                      "Arcane Staff",
                      "Channels magical energy",
                      ItemRarity::Uncommon,
                      ItemSlot::MainHand,
                      3.5f,
                      50,
                      1,
                      false,
                      {0, 0, 8, 4, 0, 0},
                      0,
                      0});
        RegisterItem({13,
                      "Hunter's Bow",
                      "A fine longbow for the wilds",
                      ItemRarity::Uncommon,
                      ItemSlot::MainHand,
                      2.5f,
                      40,
                      1,
                      false,
                      {0, 7, 0, 0, 0, 0},
                      0,
                      0});
        RegisterItem({14,
                      "Flamebrand",
                      "A blade wreathed in eternal fire",
                      ItemRarity::Epic,
                      ItemSlot::MainHand,
                      4.0f,
                      200,
                      1,
                      false,
                      {8, 2, 6, 0, 0, 0},
                      0,
                      0});
        RegisterItem({15,
                      "Dawnbreaker",
                      "Holy blade forged at first light",
                      ItemRarity::Legendary,
                      ItemSlot::MainHand,
                      5.0f,
                      500,
                      1,
                      false,
                      {10, 3, 5, 5, 3, 3},
                      0,
                      0});

        // === Offhand (OffHand) ===
        RegisterItem({20,
                      "Wooden Shield",
                      "Basic protection",
                      ItemRarity::Common,
                      ItemSlot::OffHand,
                      4.0f,
                      15,
                      1,
                      false,
                      {0, 0, 0, 0, 3, 0},
                      0,
                      0});
        RegisterItem({21,
                      "Iron Buckler",
                      "Sturdy metal shield",
                      ItemRarity::Uncommon,
                      ItemSlot::OffHand,
                      5.0f,
                      40,
                      1,
                      false,
                      {2, 0, 0, 0, 5, 0},
                      0,
                      0});

        // === Armor (Chest) ===
        RegisterItem({30,
                      "Leather Vest",
                      "Light and flexible",
                      ItemRarity::Common,
                      ItemSlot::Chest,
                      5.0f,
                      20,
                      1,
                      false,
                      {0, 1, 0, 0, 2, 0},
                      0,
                      0});
        RegisterItem({31,
                      "Chainmail Hauberk",
                      "Interlocking metal rings",
                      ItemRarity::Uncommon,
                      ItemSlot::Chest,
                      10.0f,
                      60,
                      1,
                      false,
                      {1, 0, 0, 0, 5, 0},
                      0,
                      0});
        RegisterItem({32,
                      "Shadow Robe",
                      "Woven from darkness itself",
                      ItemRarity::Epic,
                      ItemSlot::Chest,
                      2.0f,
                      180,
                      1,
                      false,
                      {0, 0, 7, 5, 1, 3},
                      0,
                      0});

        // === Headgear (Head) ===
        RegisterItem({40,
                      "Leather Cap",
                      "Simple head protection",
                      ItemRarity::Common,
                      ItemSlot::Head,
                      1.0f,
                      8,
                      1,
                      false,
                      {0, 0, 0, 0, 1, 0},
                      0,
                      0});
        RegisterItem({41,
                      "Wizard's Hat",
                      "Enhances magical focus",
                      ItemRarity::Uncommon,
                      ItemSlot::Head,
                      0.5f,
                      35,
                      1,
                      false,
                      {0, 0, 4, 2, 0, 1},
                      0,
                      0});

        // === Accessories ===
        RegisterItem({50,
                      "Silver Ring",
                      "A simple silver band",
                      ItemRarity::Common,
                      ItemSlot::Ring,
                      0.1f,
                      20,
                      1,
                      false,
                      {1, 1, 1, 1, 1, 1},
                      0,
                      0});
        RegisterItem({51,
                      "Amulet of Vigor",
                      "Pulses with vitality",
                      ItemRarity::Rare,
                      ItemSlot::Amulet,
                      0.2f,
                      80,
                      1,
                      false,
                      {2, 0, 0, 0, 5, 2},
                      0,
                      0});

        // === Boots (Feet) ===
        RegisterItem({60,
                      "Traveler's Boots",
                      "Comfortable for long journeys",
                      ItemRarity::Common,
                      ItemSlot::Feet,
                      2.0f,
                      12,
                      1,
                      false,
                      {0, 2, 0, 0, 1, 0},
                      0,
                      0});

        // === Gloves (Hands) ===
        RegisterItem({70,
                      "Leather Gloves",
                      "Basic hand protection",
                      ItemRarity::Common,
                      ItemSlot::Hands,
                      1.0f,
                      10,
                      1,
                      false,
                      {1, 1, 0, 0, 0, 0},
                      0,
                      0});

        // === Leg armor (Legs) ===
        RegisterItem({80,
                      "Iron Greaves",
                      "Heavy leg protection",
                      ItemRarity::Uncommon,
                      ItemSlot::Legs,
                      6.0f,
                      45,
                      1,
                      false,
                      {1, 0, 0, 0, 4, 0},
                      0,
                      0});
    }

    // === Item registry ===

    void RPGInventorySystem::RegisterItem(const RPGItemDef& def)
    {
        m_items[def.id] = def;
    }

    const RPGItemDef* RPGInventorySystem::GetItem(uint32_t id) const
    {
        auto it = m_items.find(id);
        return it != m_items.end() ? &it->second : nullptr;
    }

    std::string RPGInventorySystem::GetItemListString() const
    {
        std::ostringstream ss;
        ss << "=== RPG Items ===\n";
        for (const auto& [id, def] : m_items)
        {
            ss << "[" << id << "] " << def.name << " (" << GetRarityName(def.rarity) << ") - " << def.description
               << "\n";
        }
        return ss.str();
    }

    // === Inventory operations ===

    float RPGInventorySystem::GetTotalWeight(const RPGInventoryData& inv) const
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

    int RPGInventorySystem::AddItem(RPGInventoryData& inv, uint32_t itemId, int count)
    {
        const auto* def = GetItem(itemId);
        if (!def || count <= 0)
            return 0;

        // Weight check
        float currentWeight = GetTotalWeight(inv);
        float addWeight = def->weight * count;
        if (currentWeight + addWeight > inv.maxWeight)
        {
            int fittable = static_cast<int>((inv.maxWeight - currentWeight) / def->weight);
            if (fittable <= 0)
                return 0;
            count = std::min(count, fittable);
        }

        int remaining = count;

        // Stack with existing slots
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

    int RPGInventorySystem::RemoveItem(RPGInventoryData& inv, uint32_t itemId, int count)
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

    bool RPGInventorySystem::HasItem(const RPGInventoryData& inv, uint32_t itemId, int minCount) const
    {
        int total = 0;
        for (const auto& slot : inv.slots)
        {
            if (slot.itemDefId == itemId)
                total += slot.count;
        }
        return total >= minCount;
    }

    bool RPGInventorySystem::IsFull(const RPGInventoryData& inv) const
    {
        return static_cast<int>(inv.slots.size()) >= inv.maxSlots;
    }

    // === Equipment comparison ===

    ItemComparison RPGInventorySystem::CompareItems(uint32_t currentItemId, uint32_t newItemId) const
    {
        ItemComparison result;
        const auto* current = GetItem(currentItemId);
        const auto* newItem = GetItem(newItemId);

        if (!newItem)
            return result;

        CharacterStats currentBonus = current ? current->statBonus : CharacterStats{};
        CharacterStats newBonus = newItem->statBonus;

        result.difference.strength = newBonus.strength - currentBonus.strength;
        result.difference.dexterity = newBonus.dexterity - currentBonus.dexterity;
        result.difference.intelligence = newBonus.intelligence - currentBonus.intelligence;
        result.difference.wisdom = newBonus.wisdom - currentBonus.wisdom;
        result.difference.constitution = newBonus.constitution - currentBonus.constitution;
        result.difference.charisma = newBonus.charisma - currentBonus.charisma;

        // Consider it an upgrade if total stat gain is positive
        float totalDiff = result.difference.strength + result.difference.dexterity + result.difference.intelligence +
                          result.difference.wisdom + result.difference.constitution + result.difference.charisma;
        result.isUpgrade = (totalDiff > 0.0f);

        return result;
    }

    const char* RPGInventorySystem::GetRarityName(ItemRarity rarity)
    {
        switch (rarity)
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
        default:
            return "Unknown";
        }
    }

    void RPGInventorySystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("RPG Inventory System"))
        {
            ImGui::Text("Registered Items: %zu", m_items.size());

            if (ImGui::TreeNode("Item Registry"))
            {
                for (const auto& [id, def] : m_items)
                {
                    ImGui::Text("[%u] %s (%s) - %dg, %.1f kg", id, def.name.c_str(), GetRarityName(def.rarity),
                                def.sellValue, def.weight);
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace RPG
