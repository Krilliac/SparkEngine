/**
 * @file ARPGLootSystem.h
 * @brief Diablo-style randomized loot generation with affixes and rarity tiers
 * @author Spark Engine Team
 * @date 2026
 *
 * Generates items with random affixes based on rarity. Normal items have no
 * affixes, Magic get 1-2, Rare get 3-6, and Legendary have preset affixes.
 * Rarity weights: Normal 60%, Magic 25%, Rare 10%, Legendary 4%, Set 1%.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/ARPGEnums.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ARPG
{

    /// @brief A single stat modifier on an item
    struct AffixData
    {
        std::string name;
        std::string statType; ///< e.g. "strength", "fire_resist", "attack_speed"
        float minValue = 0.0f;
        float maxValue = 0.0f;
        float rolledValue = 0.0f; ///< Actual value rolled during generation
    };

    /// @brief A generated item with base stats and random affixes
    struct ItemData
    {
        uint32_t itemId = 0;
        std::string name;
        ARPGItemSlot slot = ARPGItemSlot::MainHand;
        ARPGItemRarity rarity = ARPGItemRarity::Normal;
        int itemLevel = 1;
        float baseDamage = 0.0f; ///< For weapons
        float baseArmor = 0.0f;  ///< For armor
        std::vector<AffixData> affixes;
    };

    /**
     * @brief Randomized loot generation with affix pools and rarity weights
     */
    class ARPGLootSystem
    {
      public:
        ARPGLootSystem() = default;
        ~ARPGLootSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Shutdown();
        void RenderDebugUI();

        // === Loot generation ===
        ItemData GenerateItem(int level, ARPGItemRarity rarity);
        ItemData GenerateRandomDrop(int monsterLevel, ARPGMonsterRank monsterRank);

        // === Queries ===
        size_t GetAffixPoolSize() const { return m_affixPool.size(); }
        size_t GetGeneratedItemCount() const { return m_generatedCount; }
        std::string GetLootInfoString() const;

      private:
        void BuildAffixPool();
        ARPGItemRarity RollRarity(ARPGMonsterRank rank) const;
        std::string GenerateItemName(ARPGItemSlot slot, ARPGItemRarity rarity) const;
        void RollAffixes(ItemData& item, int count);

        Spark::IEngineContext* m_context{nullptr};
        std::vector<AffixData> m_affixPool;
        uint32_t m_nextItemId = 1;
        size_t m_generatedCount = 0;
    };

} // namespace ARPG
