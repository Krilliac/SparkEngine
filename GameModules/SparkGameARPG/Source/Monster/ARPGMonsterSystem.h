/**
 * @file ARPGMonsterSystem.h
 * @brief Monster types, champion affixes, elite packs, and boss encounters
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines monster templates (skeleton, zombie, demon, etc.) with stats that
 * scale by level. Champion/elite monsters get 1-3 random affixes that modify
 * their behavior and stats.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/ARPGEnums.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ARPG
{

    /// @brief Champion affixes that modify monster behavior and stats
    enum class ChampionAffix : uint8_t
    {
        ExtraFast = 0,
        FireEnchanted = 1,
        LightningEnchanted = 2,
        Frozen = 3,
        Teleporter = 4,
        Vampiric = 5,  ///< Heals on hit
        Plagued = 6,   ///< Leaves poison pools
        Shielding = 7, ///< Periodically immune
        Arcane = 8,    ///< Spawns arcane beams
        Molten = 9,    ///< Leaves fire trail, explodes on death
        Count = 10
    };

    /// @brief Template for a monster type (base stats before level scaling)
    struct MonsterTemplate
    {
        std::string name;
        float baseHealth = 50.0f;
        float baseDamage = 8.0f;
        ARPGDamageType damageType = ARPGDamageType::Physical;
        float baseMoveSpeed = 3.0f;
        float baseXpReward = 20.0f;
        float baseLootChance = 0.15f; ///< 0.0 - 1.0
    };

    /// @brief A spawned monster instance with level-scaled stats
    struct MonsterData
    {
        uint32_t monsterId = 0;
        std::string name;
        ARPGMonsterRank rank = ARPGMonsterRank::Normal;
        int level = 1;
        float health = 50.0f;
        float maxHealth = 50.0f;
        float damage = 8.0f;
        ARPGDamageType damageType = ARPGDamageType::Physical;
        float moveSpeed = 3.0f;
        float xpReward = 20.0f;
        float lootChance = 0.15f;
        std::vector<ChampionAffix> affixes;
    };

    /**
     * @brief Monster template registry, spawning, and champion affix assignment
     */
    class ARPGMonsterSystem
    {
      public:
        ARPGMonsterSystem() = default;
        ~ARPGMonsterSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Shutdown();
        void Update(float deltaTime);
        void RenderDebugUI();

        // === Spawning ===
        MonsterData SpawnMonster(const std::string& templateName, int level,
                                 ARPGMonsterRank rank = ARPGMonsterRank::Normal);
        std::vector<MonsterData> SpawnElitePack(int level, int packSize = 4);
        MonsterData SpawnBoss(int level);

        // === Queries ===
        size_t GetTemplateCount() const { return m_templates.size(); }
        size_t GetActiveMonsterCount() const { return m_activeMonsters.size(); }
        std::string GetMonsterListString() const;

      private:
        void RegisterMonsterTemplates();
        void ApplyRankModifiers(MonsterData& monster) const;
        void AssignChampionAffixes(MonsterData& monster, int count) const;
        const MonsterTemplate* FindTemplate(const std::string& name) const;

        Spark::IEngineContext* m_context{nullptr};
        std::vector<MonsterTemplate> m_templates;
        std::vector<MonsterData> m_activeMonsters;
        uint32_t m_nextMonsterId = 1;
    };

} // namespace ARPG
