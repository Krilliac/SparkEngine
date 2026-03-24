/**
 * @file ProgressionSystem.cpp
 * @brief XP, leveling, and unlock progression
 */

#include "Core/Platform.h"
#include "ProgressionSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Spark
{

    ProgressionSystem::ProgressionSystem() = default;

    void ProgressionSystem::Initialize()
    {
        m_level = 1;
        m_currentXP = 0;
        m_earnedUnlocks.clear();
        m_unlockedWeapons.clear();
        m_unlockedClasses.clear();

        BuildUnlockTable();
        RecalculateBonuses();

        // Mark starting unlocks (level 1)
        m_unlockedWeapons[static_cast<int>(WeaponType::PISTOL)] = true;
        m_unlockedWeapons[static_cast<int>(WeaponType::RIFLE)] = true;
        m_unlockedClasses[static_cast<int>(PlayerClass::SCOUT)] = true;

        LOG_TO_CONSOLE_IMMEDIATE(L"Progression system initialized (level 1, 50 max)", L"SUCCESS");
    }

    void ProgressionSystem::AwardXP(int amount, const std::string& source)
    {
        if (m_level >= m_maxLevel)
            return;

        int modified = static_cast<int>(amount * m_currentBonuses.xpMultiplier);
        m_currentXP += modified;

        if (m_callbacks.onXPGained)
            m_callbacks.onXPGained(modified, m_currentXP);

        CheckLevelUp();
    }

    int ProgressionSystem::GetXPForLevel(int level) const
    {
        if (level <= 1)
            return 0;
        // Quadratic XP curve: each level requires more than the last
        // Level 2 = 200, Level 10 = 2000, Level 20 = 8000, Level 50 = 50000
        return static_cast<int>(100.0f * level * std::sqrt(static_cast<float>(level)));
    }

    int ProgressionSystem::GetXPToNextLevel() const
    {
        if (m_level >= m_maxLevel)
            return 0;
        return GetXPForLevel(m_level + 1) - m_currentXP;
    }

    float ProgressionSystem::GetLevelProgress() const
    {
        if (m_level >= m_maxLevel)
            return 1.0f;
        int currentLevelXP = GetXPForLevel(m_level);
        int nextLevelXP = GetXPForLevel(m_level + 1);
        int range = nextLevelXP - currentLevelXP;
        if (range <= 0)
            return 1.0f;
        return static_cast<float>(m_currentXP - currentLevelXP) / range;
    }

    bool ProgressionSystem::IsWeaponUnlocked(WeaponType weapon) const
    {
        auto it = m_unlockedWeapons.find(static_cast<int>(weapon));
        return it != m_unlockedWeapons.end() && it->second;
    }

    bool ProgressionSystem::IsClassUnlocked(PlayerClass playerClass) const
    {
        auto it = m_unlockedClasses.find(static_cast<int>(playerClass));
        return it != m_unlockedClasses.end() && it->second;
    }

    void ProgressionSystem::CheckLevelUp()
    {
        while (m_level < m_maxLevel && m_currentXP >= GetXPForLevel(m_level + 1))
        {
            m_level++;
            RecalculateBonuses();

            // Grant unlocks for this level
            for (const auto& unlock : m_unlockTable)
            {
                if (unlock.level == m_level)
                {
                    m_earnedUnlocks.push_back(unlock);
                    if (unlock.type == LevelUnlock::Type::Weapon)
                        m_unlockedWeapons[unlock.enumValue] = true;
                    else if (unlock.type == LevelUnlock::Type::Class)
                        m_unlockedClasses[unlock.enumValue] = true;

                    if (m_callbacks.onUnlock)
                        m_callbacks.onUnlock(unlock);
                }
            }

            if (m_callbacks.onLevelUp)
                m_callbacks.onLevelUp(m_level, m_currentBonuses);

            std::wstring msg = L"LEVEL UP! Now level " + std::to_wstring(m_level);
            LOG_TO_CONSOLE_IMMEDIATE(msg, L"SUCCESS");
        }
    }

    void ProgressionSystem::RecalculateBonuses()
    {
        m_currentBonuses = {};
        m_currentBonuses.healthBonus = (m_level - 1) * 5.0f;
        m_currentBonuses.armorBonus = (m_level - 1) * 2.0f;
        m_currentBonuses.damageMultiplier = 1.0f + (m_level - 1) * 0.02f;
        m_currentBonuses.moveSpeedBonus = (m_level - 1) * 0.1f;
        m_currentBonuses.shieldRegenBonus = (m_level - 1) * 0.5f;
        m_currentBonuses.energyRegenBonus = (m_level - 1) * 0.3f;
        m_currentBonuses.cooldownReduction = std::min((m_level - 1) * 0.01f, 0.3f); // Cap at 30%
        m_currentBonuses.xpMultiplier = 1.0f + (m_level - 1) * 0.01f;
    }

    void ProgressionSystem::BuildUnlockTable()
    {
        m_unlockTable = {
            // Weapons
            {3, LevelUnlock::Type::Weapon, "Shotgun", "Close-range devastation", static_cast<int>(WeaponType::SHOTGUN)},
            {5, LevelUnlock::Type::Weapon, "SMG", "High rate of fire", static_cast<int>(WeaponType::SUBMACHINE_GUN)},
            {7, LevelUnlock::Type::Weapon, "Sniper Rifle", "Long-range precision",
             static_cast<int>(WeaponType::SNIPER_RIFLE)},
            {10, LevelUnlock::Type::Weapon, "Assault Rifle", "Versatile combat",
             static_cast<int>(WeaponType::ASSAULT_RIFLE)},
            {12, LevelUnlock::Type::Weapon, "Rocket Launcher", "Explosive power",
             static_cast<int>(WeaponType::ROCKET_LAUNCHER)},
            {15, LevelUnlock::Type::Weapon, "Machine Gun", "Sustained fire", static_cast<int>(WeaponType::MACHINE_GUN)},
            {18, LevelUnlock::Type::Weapon, "Plasma Rifle", "Energy weapon",
             static_cast<int>(WeaponType::PLASMA_RIFLE)},
            {22, LevelUnlock::Type::Weapon, "Railgun", "Piercing shots", static_cast<int>(WeaponType::RAILGUN)},
            {25, LevelUnlock::Type::Weapon, "Minigun", "Maximum firepower", static_cast<int>(WeaponType::MINIGUN)},

            // Classes
            {2, LevelUnlock::Type::Class, "Medic", "Support healer class", static_cast<int>(PlayerClass::MEDIC)},
            {4, LevelUnlock::Type::Class, "Engineer", "Builder/deployer class",
             static_cast<int>(PlayerClass::ENGINEER)},
            {8, LevelUnlock::Type::Class, "Recon", "Stealth class", static_cast<int>(PlayerClass::RECON)},
            {14, LevelUnlock::Type::Class, "Vanguard", "Tank class", static_cast<int>(PlayerClass::VANGUARD)},
            {20, LevelUnlock::Type::Class, "Titan", "Heavy exosuit", static_cast<int>(PlayerClass::TITAN)},

            // Perks (tracked by name, no enum needed)
            {6, LevelUnlock::Type::Perk, "Fast Reload", "10% faster reload speed", 0},
            {9, LevelUnlock::Type::Perk, "Thick Skin", "+20 max health", 0},
            {11, LevelUnlock::Type::Perk, "Scavenger", "Enemies drop more loot", 0},
            {16, LevelUnlock::Type::Perk, "Quick Hands", "Faster weapon switch", 0},
            {19, LevelUnlock::Type::Perk, "Juggernaut", "+50 max armor", 0},
            {30, LevelUnlock::Type::Perk, "Double XP", "Permanent 2x XP multiplier", 0},
        };
    }

    std::string ProgressionSystem::Console_GetStatus() const
    {
        std::stringstream ss;
        ss << "=== Progression ===\n";
        ss << "Level: " << m_level << "/" << m_maxLevel << "\n";
        ss << "XP: " << m_currentXP << "/" << GetXPForLevel(m_level + 1) << "\n";
        ss << "Progress: " << static_cast<int>(GetLevelProgress() * 100.0f) << "%\n";
        ss << "Bonuses: HP+" << m_currentBonuses.healthBonus << " Armor+" << m_currentBonuses.armorBonus << " DMG x"
           << m_currentBonuses.damageMultiplier << "\n";
        ss << "Unlocks earned: " << m_earnedUnlocks.size() << "/" << m_unlockTable.size() << "\n";

        if (!m_earnedUnlocks.empty())
        {
            ss << "Recent unlocks:\n";
            int count = 0;
            for (auto it = m_earnedUnlocks.rbegin(); it != m_earnedUnlocks.rend() && count < 5; ++it, ++count)
            {
                ss << "  [Lv" << it->level << "] " << it->name << " - " << it->description << "\n";
            }
        }
        return ss.str();
    }

} // namespace Spark
