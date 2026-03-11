/**
 * @file AchievementSystem.h
 * @brief Achievement and persistent player statistics tracking
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Provides an engine-level system for defining achievements with conditions,
 * tracking persistent player statistics, and firing events on unlock. Designed
 * to be platform-agnostic — game modules can bridge to Steam, Xbox, or
 * PlayStation achievement APIs via the provided callbacks.
 *
 * ## Usage
 * @code
 *   auto& achievements = AchievementSystem::Get();
 *   achievements.DefineAchievement("first_blood", "First Blood", "Get your first kill");
 *   achievements.DefineStatistic("kills", StatType::Integer);
 *   achievements.DefineStatistic("play_time", StatType::Float);
 *
 *   achievements.SetCondition("first_blood", "kills", 1);
 *
 *   // During gameplay:
 *   achievements.IncrementStat("kills", 1);   // auto-checks conditions
 *   achievements.AddStatFloat("play_time", deltaTime);
 *
 *   // Persistence:
 *   achievements.SaveToFile("Data/Save/stats.json");
 *   achievements.LoadFromFile("Data/Save/stats.json");
 * @endcode
 */

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <cstdint>
#include <mutex>

namespace Spark
{

    // =============================================================================
    // StatType
    // =============================================================================

    /**
 * @brief Types of persistent statistics that can be tracked.
 */
    enum class StatType
    {
        Integer, ///< Whole number (kills, deaths, items collected)
        Float,   ///< Fractional value (play time, distance traveled)
        Maximum, ///< Tracks highest value seen (best score, longest streak)
        Minimum  ///< Tracks lowest value seen (fastest time, fewest deaths)
    };

    // =============================================================================
    // Statistic
    // =============================================================================

    /**
 * @brief A single tracked statistic.
 */
    struct Statistic
    {
        std::string name;        ///< Statistic identifier
        StatType type;           ///< How the stat is aggregated
        int64_t intValue = 0;    ///< Current integer value
        double floatValue = 0.0; ///< Current float value
    };

    // =============================================================================
    // Achievement
    // =============================================================================

    /**
 * @brief Definition of a single achievement.
 */
    struct Achievement
    {
        std::string id;          ///< Unique identifier
        std::string name;        ///< Display name
        std::string description; ///< Human-readable description
        std::string iconPath;    ///< Path to achievement icon texture
        bool unlocked = false;   ///< Whether the achievement has been earned
        bool hidden = false;     ///< Hidden until unlocked (spoiler prevention)
        float progress = 0.0f;   ///< Current progress (0.0 to 1.0)

        /** @brief Condition: stat name that must reach `requiredValue`. */
        std::string conditionStat;
        int64_t requiredValue = 0;
    };

    // =============================================================================
    // AchievementSystem
    // =============================================================================

    /**
 * @class AchievementSystem
 * @brief Manages achievements, statistics, and unlock events.
 *
 * Thread-safe singleton. Automatically checks achievement conditions when
 * statistics are updated. Fires callbacks on unlock for platform integration.
 */
    class AchievementSystem
    {
      public:
        /** @brief Get the singleton instance. */
        static AchievementSystem& Get();

        // --- Achievement definition ---

        /**
     * @brief Define a new achievement.
     * @param id          Unique identifier.
     * @param name        Display name.
     * @param description Description text.
     * @param hidden      Whether to hide until unlocked.
     */
        void DefineAchievement(const std::string& id, const std::string& name, const std::string& description,
                               bool hidden = false);

        /**
     * @brief Set the unlock condition for an achievement.
     * @param achievementId Achievement ID.
     * @param statName      Statistic that triggers the unlock.
     * @param requiredValue Value the stat must reach.
     */
        void SetCondition(const std::string& achievementId, const std::string& statName, int64_t requiredValue);

        /**
     * @brief Manually unlock an achievement (for script-triggered unlocks).
     * @param id Achievement ID.
     * @return true if newly unlocked, false if already unlocked or not found.
     */
        bool UnlockAchievement(const std::string& id);

        /**
     * @brief Get achievement info by ID.
     * @param id Achievement ID.
     * @return Pointer to Achievement, or nullptr if not found.
     */
        const Achievement* GetAchievement(const std::string& id) const;

        /** @brief Get all defined achievements. */
        std::vector<Achievement> GetAllAchievements() const;

        /** @brief Get count of unlocked achievements. */
        size_t GetUnlockedCount() const;

        /** @brief Get total achievement count. */
        size_t GetTotalCount() const;

        // --- Statistics ---

        /**
     * @brief Define a new statistic.
     * @param name Unique statistic name.
     * @param type Aggregation type.
     */
        void DefineStatistic(const std::string& name, StatType type);

        /**
     * @brief Increment an integer statistic by a given amount.
     * @param name  Statistic name.
     * @param delta Amount to add.
     */
        void IncrementStat(const std::string& name, int64_t delta = 1);

        /**
     * @brief Add to a float statistic.
     * @param name  Statistic name.
     * @param delta Amount to add.
     */
        void AddStatFloat(const std::string& name, double delta);

        /**
     * @brief Set a statistic to a specific value (for Maximum/Minimum types).
     * @param name  Statistic name.
     * @param value Value to compare/set.
     */
        void SetStat(const std::string& name, int64_t value);

        /**
     * @brief Get the current integer value of a statistic.
     * @param name Statistic name.
     * @return Current value, or 0 if not found.
     */
        int64_t GetStatInt(const std::string& name) const;

        /**
     * @brief Get the current float value of a statistic.
     * @param name Statistic name.
     * @return Current value, or 0.0 if not found.
     */
        double GetStatFloat(const std::string& name) const;

        /** @brief Get all defined statistics. */
        std::vector<Statistic> GetAllStatistics() const;

        // --- Persistence ---

        /**
     * @brief Save all achievements and statistics to a JSON file.
     * @param filePath Output file path.
     * @return true if saving succeeded.
     */
        bool SaveToFile(const std::string& filePath) const;

        /**
     * @brief Load achievements and statistics from a JSON file.
     * @param filePath Input file path.
     * @return true if loading succeeded.
     */
        bool LoadFromFile(const std::string& filePath);

        /**
     * @brief Reset all progress (statistics and achievement unlocks).
     */
        void ResetAll();

        // --- Callbacks ---

        /**
     * @brief Register a callback invoked when an achievement is unlocked.
     * @param callback Function called with the achievement ID.
     */
        void OnAchievementUnlocked(std::function<void(const std::string&)> callback);

        // --- Console integration ---

        /** @brief Get achievement system status (console integration). */
        std::string Console_GetStatus() const;

        /** @brief List all statistics (console integration). */
        std::string Console_ListStats() const;

      private:
        AchievementSystem() = default;

        void CheckConditions(const std::string& statName);

        std::unordered_map<std::string, Achievement> m_achievements;
        std::unordered_map<std::string, Statistic> m_statistics;
        std::vector<std::function<void(const std::string&)>> m_unlockCallbacks;
        mutable std::mutex m_mutex;
    };

} // namespace Spark
