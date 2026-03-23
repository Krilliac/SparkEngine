/**
 * @file MMOWorldBossSystem.h
 * @brief Open-world boss encounters with phases, contribution tracking, shared loot
 * @author Spark Engine Team
 * @date 2026
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/MMOEnums.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMO
{

    /// @brief Phase transition definition
    struct BossPhaseTransition
    {
        BossPhase from = BossPhase::Phase1;
        BossPhase to = BossPhase::Transition;
        float healthThreshold = 0.5f;
        std::string announcement;
        float transitionDuration = 5.0f;
    };

    /// @brief Boss special ability
    struct BossAbility
    {
        std::string name;
        float cooldown = 30.0f;
        float cooldownRemaining = 0.0f;
        float damage = 500.0f;
        float radius = 15.0f;
        BossPhase activeInPhase = BossPhase::Phase1;
        std::string warningMessage;
        float warningTime = 3.0f;
    };

    /// @brief Per-player contribution tracking
    struct BossContribution
    {
        uint32_t playerId = 0;
        std::string playerName;
        float damageDealt = 0.0f;
        float healingDone = 0.0f;
        float participationTime = 0.0f;
        int deaths = 0;
        float score = 0.0f;
    };

    /// @brief World boss definition
    struct WorldBossDef
    {
        uint32_t id = 0;
        std::string name;
        std::string title;
        std::string description;
        float spawnX = 0, spawnY = 0, spawnZ = 0;
        float maxHealth = 1000000.0f;
        float baseDamage = 200.0f;
        float detectionRange = 100.0f;
        std::vector<BossPhaseTransition> transitions;
        std::vector<BossAbility> abilities;
        int minPlayersForLoot = 3;
        float spawnCooldown = 14400.0f;
        std::string lootTable;
    };

    /// @brief Live world boss instance
    struct WorldBossInstance
    {
        uint32_t bossDefId = 0;
        BossPhase currentPhase = BossPhase::Dormant;
        float currentHealth = 0.0f;
        float maxHealth = 0.0f;
        float spawnTimer = 0.0f;
        float phaseTimer = 0.0f;
        float enrageTimer = 0.0f;
        float encounterDuration = 0.0f;
        std::vector<BossAbility> activeAbilities;
        std::unordered_map<uint32_t, BossContribution> contributions;

        float GetHealthPct() const { return maxHealth > 0 ? currentHealth / maxHealth : 0.0f; }
        int GetParticipantCount() const { return static_cast<int>(contributions.size()); }
        bool IsAlive() const { return currentPhase != BossPhase::Dormant && currentPhase != BossPhase::Defeated; }
    };

    /**
     * @brief World boss encounter management
     */
    class MMOWorldBossSystem
    {
      public:
        MMOWorldBossSystem() = default;
        ~MMOWorldBossSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float dt);
        void Shutdown();
        void RenderDebugUI();

        void RegisterBoss(const WorldBossDef& def);
        const WorldBossDef* GetBossDef(uint32_t id) const;

        bool SpawnBoss(uint32_t bossDefId);
        bool DamageBoss(uint32_t bossDefId, uint32_t attackerId, const std::string& attackerName, float damage);
        void RecordHealing(uint32_t bossDefId, uint32_t healerId, const std::string& healerName, float amount);
        void ResetBoss(uint32_t bossDefId);

        const WorldBossInstance* GetBossInstance(uint32_t bossDefId) const;
        std::vector<BossContribution> GetContributionRanking(uint32_t bossDefId) const;
        bool QualifiesForLoot(uint32_t bossDefId, uint32_t playerId) const;
        bool IsBossActive(uint32_t bossDefId) const;
        BossPhase GetBossPhase(uint32_t bossDefId) const;

        size_t GetBossCount() const { return m_bossDefs.size(); }
        std::string GetBossStatusString() const;

      private:
        void RegisterDefaultBosses();
        void UpdatePhases(WorldBossInstance& inst, const WorldBossDef& def);
        void UpdateAbilities(WorldBossInstance& inst, float dt);
        void DefeatBoss(WorldBossInstance& inst, const WorldBossDef& def);
        void CalculateContributions(WorldBossInstance& inst);

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, WorldBossDef> m_bossDefs;
        std::unordered_map<uint32_t, WorldBossInstance> m_instances;

        static constexpr float MIN_CONTRIBUTION_PCT = 0.02f;
        static constexpr float ENRAGE_TIME = 900.0f;
    };

} // namespace MMO
