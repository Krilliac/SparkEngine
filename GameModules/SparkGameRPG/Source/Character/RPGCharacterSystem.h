/**
 * @file RPGCharacterSystem.h
 * @brief Character creation, class definitions, stat allocation, and leveling
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines 6 character classes with base stats, per-level stat growth,
 * equipment slots affecting computed stats, and active/passive abilities.
 * Characters gain XP and level up with class-specific stat growth curves.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RPGEnums.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace RPG
{

    /// @brief Core character attributes
    struct CharacterStats
    {
        float strength = 10.0f;
        float dexterity = 10.0f;
        float intelligence = 10.0f;
        float wisdom = 10.0f;
        float constitution = 10.0f;
        float charisma = 10.0f;
    };

    /// @brief Per-level stat growth multipliers for a class
    struct StatGrowth
    {
        float strength = 1.0f;
        float dexterity = 1.0f;
        float intelligence = 1.0f;
        float wisdom = 1.0f;
        float constitution = 1.0f;
        float charisma = 1.0f;
    };

    /// @brief Definition of an ability (active or passive)
    struct AbilityDef
    {
        uint32_t id = 0;
        std::string name;
        std::string description;
        bool isPassive = false;
        float cooldown = 0.0f; ///< Seconds between uses (active only)
        float manaCost = 0.0f; ///< Mana consumed on use
        DamageType damageType = DamageType::Physical;
        float baseDamage = 0.0f; ///< Base damage/heal amount
        int requiredLevel = 1;   ///< Level required to unlock
    };

    /// @brief Class definition with base stats, growth, and abilities
    struct ClassDef
    {
        CharacterClass id = CharacterClass::Warrior;
        std::string name;
        std::string description;
        CharacterStats baseStats;
        StatGrowth growth;
        float baseHealth = 100.0f;
        float baseMana = 50.0f;
        float healthPerLevel = 10.0f;
        float manaPerLevel = 5.0f;
        std::vector<AbilityDef> abilities;
    };

    /// @brief Equipment stat bonuses applied to a slot
    struct EquipmentBonus
    {
        ItemSlot slot = ItemSlot::MainHand;
        uint32_t itemId = 0;
        CharacterStats statBonus;
        std::array<float, static_cast<size_t>(DamageType::Count)> resistances{};
    };

    /// @brief Live character instance with current state
    struct CharacterData
    {
        uint32_t characterId = 0;
        std::string name;
        CharacterClass classId = CharacterClass::Warrior;
        int level = 1;
        uint32_t xp = 0;
        uint32_t xpToNextLevel = 100;
        float currentHealth = 100.0f;
        float maxHealth = 100.0f;
        float currentMana = 50.0f;
        float maxMana = 50.0f;
        CharacterStats baseStats;
        std::array<EquipmentBonus, static_cast<size_t>(ItemSlot::Count)> equipment{};
        int freeStatPoints = 0;
    };

    /**
     * @brief Character creation, class registry, stat computation, and leveling
     */
    class RPGCharacterSystem
    {
      public:
        RPGCharacterSystem() = default;
        ~RPGCharacterSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Shutdown();
        void RenderDebugUI();

        // === Class Registry ===
        const ClassDef* GetClassDef(CharacterClass id) const;
        size_t GetClassCount() const { return m_classes.size(); }
        std::string GetClassListString() const;

        // === Character CRUD ===
        uint32_t CreateCharacter(const std::string& name, CharacterClass classId);
        CharacterData* GetCharacter(uint32_t characterId);
        const CharacterData* GetCharacter(uint32_t characterId) const;

        // === Leveling ===
        void AddXP(uint32_t characterId, uint32_t amount);
        CharacterStats ComputeEffectiveStats(uint32_t characterId) const;
        void AllocateStatPoint(uint32_t characterId, const std::string& statName);

        // === Equipment ===
        void Equip(uint32_t characterId, ItemSlot slot, uint32_t itemId, const CharacterStats& bonus);
        void Unequip(uint32_t characterId, ItemSlot slot);

      private:
        void RegisterDefaultClasses();
        void LevelUp(CharacterData& character);
        void RecalculateDerivedStats(CharacterData& character) const;
        uint32_t CalculateXPForLevel(int level) const;

        Spark::IEngineContext* m_context{nullptr};
        std::vector<ClassDef> m_classes;
        std::unordered_map<uint32_t, CharacterData> m_characters;
        uint32_t m_nextCharId = 1;

        static constexpr int MAX_LEVEL = 50;
        static constexpr int STAT_POINTS_PER_LEVEL = 3;
    };

} // namespace RPG
