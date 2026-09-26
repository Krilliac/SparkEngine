/**
 * @file ARPGLootSystem.cpp
 * @brief Diablo-style randomized loot with affixes, rarity weights, and item naming
 */

#include "ARPGLootSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <sstream>
#include <utility>

namespace ARPG
{

    static std::mt19937& GetLootRNG()
    {
        static thread_local std::mt19937 rng{std::random_device{}()};
        return rng;
    }

    /// Inclusive affix-count range each rarity rolls; shared by generation and restore validation.
    static std::pair<int, int> AffixCountRange(ARPGItemRarity rarity)
    {
        switch (rarity)
        {
        case ARPGItemRarity::Magic:
            return {1, 2};
        case ARPGItemRarity::Rare:
            return {3, 6};
        case ARPGItemRarity::Legendary:
        case ARPGItemRarity::Set:
        case ARPGItemRarity::Unique:
            return {4, 4}; // Preset count for legendaries
        default:
            return {0, 0};
        }
    }

    /// Affix values scale with item level; generation and restore validation must agree exactly.
    static float AffixLevelScale(int itemLevel)
    {
        return 1.0f + static_cast<float>(itemLevel) * 0.02f;
    }

    bool ARPGLootSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        BuildAffixPool();

        SPARK_LOG_INFO(Spark::LogCategory::Game, "ARPG loot system initialized with %zu affixes", m_affixPool.size());
        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Loot system initialized (" +
                                                    std::to_string(m_affixPool.size()) + " affixes in pool)");
        return true;
    }

    void ARPGLootSystem::Shutdown()
    {
        m_affixPool.clear();
    }

    void ARPGLootSystem::BuildAffixPool()
    {
        m_affixPool.clear();

        // Offensive affixes
        m_affixPool.push_back({"of Strength", "strength", 1.0f, 20.0f, 0.0f});
        m_affixPool.push_back({"of Dexterity", "dexterity", 1.0f, 20.0f, 0.0f});
        m_affixPool.push_back({"of Intelligence", "intelligence", 1.0f, 20.0f, 0.0f});
        m_affixPool.push_back({"of Vitality", "vitality", 1.0f, 20.0f, 0.0f});
        m_affixPool.push_back({"of Fury", "attack_speed", 5.0f, 25.0f, 0.0f});
        m_affixPool.push_back({"of Precision", "critical_hit", 2.0f, 10.0f, 0.0f});
        m_affixPool.push_back({"of Carnage", "critical_damage", 10.0f, 50.0f, 0.0f});

        // Defensive affixes
        m_affixPool.push_back({"of the Flame", "fire_resist", 5.0f, 30.0f, 0.0f});
        m_affixPool.push_back({"of the Glacier", "cold_resist", 5.0f, 30.0f, 0.0f});
        m_affixPool.push_back({"of the Storm", "lightning_resist", 5.0f, 30.0f, 0.0f});
        m_affixPool.push_back({"of the Serpent", "poison_resist", 5.0f, 30.0f, 0.0f});
        m_affixPool.push_back({"of the Sentinel", "armor", 10.0f, 60.0f, 0.0f});

        // Utility affixes
        m_affixPool.push_back({"of Life", "max_health", 10.0f, 80.0f, 0.0f});
        m_affixPool.push_back({"of the Mind", "max_mana", 5.0f, 50.0f, 0.0f});
        m_affixPool.push_back({"of Swiftness", "move_speed", 3.0f, 12.0f, 0.0f});
        m_affixPool.push_back({"of Leeching", "life_steal", 1.0f, 8.0f, 0.0f});
    }

    ARPGItemRarity ARPGLootSystem::RollRarity(ARPGMonsterRank rank) const
    {
        // Base weights: Normal 60%, Magic 25%, Rare 10%, Legendary 4%, Set 1%
        // Higher monster ranks shift weights toward better loot
        float legendaryBonus = 0.0f;
        float rareBonus = 0.0f;

        switch (rank)
        {
        case ARPGMonsterRank::Champion:
            rareBonus = 5.0f;
            legendaryBonus = 2.0f;
            break;
        case ARPGMonsterRank::Elite:
            rareBonus = 10.0f;
            legendaryBonus = 4.0f;
            break;
        case ARPGMonsterRank::MiniBoss:
            rareBonus = 15.0f;
            legendaryBonus = 8.0f;
            break;
        case ARPGMonsterRank::Boss:
            rareBonus = 20.0f;
            legendaryBonus = 15.0f;
            break;
        default:
            break;
        }

        std::uniform_real_distribution<float> dist(0.0f, 100.0f);
        float roll = dist(GetLootRNG());

        float setThreshold = 1.0f;
        float legendaryThreshold = setThreshold + 4.0f + legendaryBonus;
        float rareThreshold = legendaryThreshold + 10.0f + rareBonus;
        float magicThreshold = rareThreshold + 25.0f;

        if (roll < setThreshold)
            return ARPGItemRarity::Set;
        if (roll < legendaryThreshold)
            return ARPGItemRarity::Legendary;
        if (roll < rareThreshold)
            return ARPGItemRarity::Rare;
        if (roll < magicThreshold)
            return ARPGItemRarity::Magic;
        return ARPGItemRarity::Normal;
    }

    std::string ARPGLootSystem::GenerateItemName(ARPGItemSlot slot, ARPGItemRarity rarity) const
    {
        // Base names by slot
        static const char* slotNames[] = {"Helm", "Plate", "Gauntlets", "Greaves", "Belt",
                                          "Ring", "Ring",  "Amulet",    "Sword",   "Shield"};

        auto slotIndex = static_cast<size_t>(slot);
        std::string baseName = (slotIndex < 10) ? slotNames[slotIndex] : "Item";

        // Rarity prefixes
        switch (rarity)
        {
        case ARPGItemRarity::Magic:
            return "Enchanted " + baseName;
        case ARPGItemRarity::Rare:
            return "Runic " + baseName;
        case ARPGItemRarity::Legendary:
            return "Ancient " + baseName;
        case ARPGItemRarity::Set:
            return "Ancestral " + baseName;
        default:
            return baseName;
        }
    }

    void ARPGLootSystem::RollAffixes(ItemData& item, int count)
    {
        if (m_affixPool.empty() || count <= 0)
            return;

        std::uniform_int_distribution<size_t> indexDist(0, m_affixPool.size() - 1);

        for (int i = 0; i < count; ++i)
        {
            AffixData affix = m_affixPool[indexDist(GetLootRNG())];

            const float levelScale = AffixLevelScale(item.itemLevel);
            std::uniform_real_distribution<float> valueDist(affix.minValue, affix.maxValue);
            affix.rolledValue = valueDist(GetLootRNG()) * levelScale;

            item.affixes.push_back(affix);
        }
    }

    ItemData ARPGLootSystem::BuildBaseItem(uint32_t itemId, ARPGItemSlot slot, ARPGItemRarity rarity, int level) const
    {
        ItemData item;
        item.itemId = itemId;
        item.slot = slot;
        item.rarity = rarity;
        item.itemLevel = level;
        item.name = GenerateItemName(slot, rarity);

        // Base stats scale with level
        const bool isWeapon = (slot == ARPGItemSlot::MainHand || slot == ARPGItemSlot::OffHand);
        if (isWeapon)
            item.baseDamage = 5.0f + static_cast<float>(level) * 2.5f;
        else
            item.baseArmor = 3.0f + static_cast<float>(level) * 1.5f;
        return item;
    }

    ItemData ARPGLootSystem::GenerateItem(int level, ARPGItemRarity rarity)
    {
        // Pick a random equipment slot
        std::uniform_int_distribution<int> slotDist(0, static_cast<int>(ARPGItemSlot::Count) - 1);
        auto slot = static_cast<ARPGItemSlot>(slotDist(GetLootRNG()));

        ItemData item = BuildBaseItem(m_nextItemId++, slot, rarity, level);

        const auto [minAffixes, maxAffixes] = AffixCountRange(rarity);
        std::uniform_int_distribution<int> countDist(minAffixes, maxAffixes);
        const int affixCount = minAffixes == maxAffixes ? minAffixes : countDist(GetLootRNG());

        RollAffixes(item, affixCount);
        m_generatedCount++;
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "ARPG item generated: %s (level %d, %d affixes)", item.name.c_str(),
                        item.itemLevel, affixCount);
        return item;
    }

    bool ARPGLootSystem::IsRestorableItem(const ItemData& item) const
    {
        // The maximum ID is refused so ReserveItemId can always move the cursor past a restored item.
        if (item.itemId == 0 || item.itemId == std::numeric_limits<uint32_t>::max() ||
            item.slot >= ARPGItemSlot::Count || item.rarity >= ARPGItemRarity::Count || item.itemLevel < 1 ||
            item.itemLevel > MAX_RESTORABLE_ITEM_LEVEL)
            return false;

        // Name and base stats are derived, never rolled, so they must match the derivation exactly.
        const ItemData expected = BuildBaseItem(item.itemId, item.slot, item.rarity, item.itemLevel);
        if (item.name != expected.name || item.baseDamage != expected.baseDamage ||
            item.baseArmor != expected.baseArmor)
            return false;

        const auto [minAffixes, maxAffixes] = AffixCountRange(item.rarity);
        const int affixCount = static_cast<int>(item.affixes.size());
        if (affixCount < minAffixes || affixCount > maxAffixes)
            return false;

        const float levelScale = AffixLevelScale(item.itemLevel);
        for (const AffixData& affix : item.affixes)
        {
            const auto poolEntry = std::ranges::find_if(m_affixPool, [&affix](const AffixData& candidate)
                                                        { return candidate.statType == affix.statType; });
            if (poolEntry == m_affixPool.end() || poolEntry->name != affix.name ||
                poolEntry->minValue != affix.minValue || poolEntry->maxValue != affix.maxValue ||
                !std::isfinite(affix.rolledValue) || affix.rolledValue < affix.minValue * levelScale ||
                affix.rolledValue > affix.maxValue * levelScale)
                return false;
        }
        return true;
    }

    void ARPGLootSystem::ReserveItemId(uint32_t itemId)
    {
        if (itemId >= m_nextItemId)
            m_nextItemId = itemId + 1;
    }

    ItemData ARPGLootSystem::GenerateRandomDrop(int monsterLevel, ARPGMonsterRank monsterRank)
    {
        ARPGItemRarity rarity = RollRarity(monsterRank);
        return GenerateItem(monsterLevel, rarity);
    }

    std::string ARPGLootSystem::GetLootInfoString() const
    {
        std::ostringstream ss;
        ss << "=== ARPG Loot System ===\n";
        ss << "Affix pool: " << m_affixPool.size() << " affixes\n";
        ss << "Items generated: " << m_generatedCount << "\n";
        ss << "\nRarity weights (base): Normal 60%, Magic 25%, Rare 10%, " << "Legendary 4%, Set 1%\n";
        ss << "Affix counts: Normal=0, Magic=1-2, Rare=3-6, Legendary=4\n";
        ss << "\nAffix pool:\n";
        for (const auto& affix : m_affixPool)
        {
            ss << "  " << affix.name << " [" << affix.statType << "] " << affix.minValue << "-" << affix.maxValue
               << "\n";
        }
        return ss.str();
    }

    void ARPGLootSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("ARPG Loot System"))
        {
            ImGui::Text("Affix pool: %zu | Generated: %zu", m_affixPool.size(), m_generatedCount);

            if (ImGui::TreeNode("Affix Pool"))
            {
                for (const auto& affix : m_affixPool)
                {
                    ImGui::Text("%s [%s] %.0f-%.0f", affix.name.c_str(), affix.statType.c_str(), affix.minValue,
                                affix.maxValue);
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace ARPG
