/**
 * @file MMOAchievementSystem.h
 * @brief Achievement tracking with stat-based criteria and tiered rewards
 * @author Spark Engine Team
 * @date 2026
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/MMOEnums.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MMO
{

    /// @brief A single criterion within an achievement
    struct AchievementCriterion
    {
        std::string description;
        std::string trackingKey; ///< e.g., "kills_total", "quests_completed"
        int requiredCount = 1;
    };

    /// @brief Reward for completing an achievement
    struct AchievementReward
    {
        int xp = 0;
        int currency = 0;
        std::string title;
        int points = 10;
    };

    /// @brief An achievement definition
    struct AchievementDef
    {
        uint32_t id = 0;
        std::string name;
        std::string description;
        AchievementCategory category = AchievementCategory::Combat;
        bool isHidden = false;
        std::vector<AchievementCriterion> criteria;
        AchievementReward reward;
        uint32_t nextInChain = 0;
    };

    /// @brief Runtime criterion progress
    struct CriterionProgress
    {
        int current = 0;
        bool completed = false;
    };

    /// @brief Runtime achievement progress
    struct AchievementProgress
    {
        uint32_t achievementId = 0;
        std::vector<CriterionProgress> criteriaProgress;
        bool completed = false;
    };

    /// @brief Per-player achievement state
    struct AchievementState
    {
        std::vector<AchievementProgress> progress;
        std::unordered_set<uint32_t> completedIds;
        std::unordered_map<std::string, int> stats;
        int totalPoints = 0;
        std::vector<std::string> earnedTitles;
        std::string activeTitle;
    };

    /**
     * @brief Achievement registry and tracking system
     */
    class MMOAchievementSystem
    {
      public:
        MMOAchievementSystem() = default;
        ~MMOAchievementSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Shutdown();
        void RenderDebugUI();

        void RegisterAchievement(const AchievementDef& def);
        const AchievementDef* GetAchievement(uint32_t id) const;

        /// Increment a stat and check related achievements
        void IncrementStat(AchievementState& state, const std::string& statKey, int increment = 1);
        void SetStat(AchievementState& state, const std::string& statKey, int value);
        void ForceComplete(AchievementState& state, uint32_t achId);
        void InitializePlayerState(AchievementState& state) const;

        float GetCompletionPercent(const AchievementState& state) const;
        size_t GetAchievementCount() const { return m_achievements.size(); }
        std::string GetStatusString(const AchievementState& state) const;

      private:
        void RegisterDefaultAchievements();
        void CheckAchievement(AchievementState& state, uint32_t achId);
        void CompleteAchievement(AchievementState& state, uint32_t achId);

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, AchievementDef> m_achievements;
        std::unordered_map<std::string, std::vector<uint32_t>> m_statToAch;
    };

} // namespace MMO
