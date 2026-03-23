/**
 * @file ProgressionSystem.h
 * @brief XP, leveling, and unlock progression system
 *
 * Tracks player experience, level-up thresholds, stat bonuses per level,
 * and weapon/ability unlocks. Designed to give a complete gameplay loop.
 */

#pragma once
#include "Core/Platform.h"
#include "Enums/GameSystemEnums.h"

#include <vector>
#include <string>
#include <functional>
#include <cstdint>
#include <unordered_map>

using SparkEditor::PlayerClass;
using SparkEditor::WeaponType;

namespace Spark
{

    /**
     * @brief An unlock granted at a specific level
     */
    struct LevelUnlock
    {
        int level = 0;
        enum class Type
        {
            Weapon,
            Ability,
            Class,
            Perk
        } type = Type::Weapon;
        std::string name;        ///< Display name
        std::string description; ///< What was unlocked
        int enumValue = 0;       ///< WeaponType/ClassAbility/PlayerClass cast value
    };

    /**
     * @brief Per-level stat bonuses applied automatically
     */
    struct LevelBonuses
    {
        float healthBonus = 0.0f;       ///< Added to max health
        float armorBonus = 0.0f;        ///< Added to max armor
        float damageMultiplier = 1.0f;  ///< Scales all weapon damage
        float moveSpeedBonus = 0.0f;    ///< Added to base move speed
        float shieldRegenBonus = 0.0f;  ///< Added to shield regen rate
        float energyRegenBonus = 0.0f;  ///< Added to energy regen rate
        float cooldownReduction = 0.0f; ///< Percentage reduction on ability cooldowns
        float xpMultiplier = 1.0f;      ///< Bonus XP from all sources
    };

    /**
     * @brief Callbacks for progression events
     */
    struct ProgressionCallbacks
    {
        std::function<void(int newLevel, const LevelBonuses& bonuses)> onLevelUp;
        std::function<void(const LevelUnlock& unlock)> onUnlock;
        std::function<void(int xpGained, int totalXP)> onXPGained;
    };

    /**
     * @brief XP and leveling progression system
     */
    class ProgressionSystem
    {
      public:
        ProgressionSystem();
        ~ProgressionSystem() = default;

        /**
         * @brief Initialize with default unlock table and XP curve
         */
        void Initialize();

        /**
         * @brief Award XP from various sources
         * @param amount Base XP amount (modified by multiplier)
         * @param source Description for logging ("kill", "wave_clear", "quest")
         */
        void AwardXP(int amount, const std::string& source = "");

        /**
         * @brief Get XP required to reach a specific level
         */
        int GetXPForLevel(int level) const;

        // === Accessors ===

        int GetLevel() const { return m_level; }
        int GetCurrentXP() const { return m_currentXP; }
        int GetXPToNextLevel() const;
        float GetLevelProgress() const; ///< 0.0 to 1.0 progress toward next level
        int GetMaxLevel() const { return m_maxLevel; }
        const LevelBonuses& GetCurrentBonuses() const { return m_currentBonuses; }

        /**
         * @brief Check if a weapon is unlocked at the current level
         */
        bool IsWeaponUnlocked(WeaponType weapon) const;

        /**
         * @brief Check if a class is unlocked at the current level
         */
        bool IsClassUnlocked(PlayerClass playerClass) const;

        /**
         * @brief Get all unlocks earned so far
         */
        const std::vector<LevelUnlock>& GetEarnedUnlocks() const { return m_earnedUnlocks; }

        // === Configuration ===

        ProgressionCallbacks& GetCallbacks() { return m_callbacks; }

        // === XP reward constants ===

        static constexpr int XP_PER_KILL = 50;
        static constexpr int XP_PER_HEADSHOT_BONUS = 25;
        static constexpr int XP_PER_WAVE_CLEAR = 150;
        static constexpr int XP_PER_BOSS_KILL = 200;
        static constexpr int XP_PER_QUEST_COMPLETE = 300;
        static constexpr int XP_PER_ASSIST = 20;

        // Console integration
        std::string Console_GetStatus() const;

      private:
        void CheckLevelUp();
        void RecalculateBonuses();
        void BuildUnlockTable();

        int m_level{1};
        int m_currentXP{0};
        int m_maxLevel{50};

        LevelBonuses m_currentBonuses;
        std::vector<LevelUnlock> m_unlockTable;   ///< All possible unlocks
        std::vector<LevelUnlock> m_earnedUnlocks; ///< Unlocks granted so far
        std::unordered_map<int, bool> m_unlockedWeapons;
        std::unordered_map<int, bool> m_unlockedClasses;

        ProgressionCallbacks m_callbacks;
    };

} // namespace Spark
