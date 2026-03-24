/**
 * @file ARPGHeroSystem.h
 * @brief Hero classes, attributes, leveling, and experience curves
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines 6 ARPG hero classes (Barbarian, Sorceress, Necromancer, Paladin,
 * Amazon, Rogue) with base stats, per-level growth rates, and experience
 * curves. Heroes gain XP, level up, and allocate attribute points.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/ARPGEnums.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ARPG
{

    /// @brief Per-level attribute growth rates for a hero class
    struct HeroGrowth
    {
        float strength = 1.0f;
        float dexterity = 1.0f;
        float intelligence = 1.0f;
        float vitality = 1.0f;
    };

    /// @brief Class template defining starting stats and growth
    struct HeroClassDef
    {
        ARPGHeroClass id = ARPGHeroClass::Barbarian;
        std::string name;
        std::string description;
        float baseStrength = 10.0f;
        float baseDexterity = 10.0f;
        float baseIntelligence = 10.0f;
        float baseVitality = 10.0f;
        float baseHealth = 100.0f;
        float baseMana = 50.0f;
        float healthPerLevel = 10.0f;
        float manaPerLevel = 5.0f;
        float baseMoveSpeed = 5.0f;
        HeroGrowth growth;
    };

    /// @brief Live hero instance with current state
    struct HeroData
    {
        uint32_t heroId = 0;
        std::string name;
        ARPGHeroClass heroClass = ARPGHeroClass::Barbarian;
        int level = 1;
        uint32_t experience = 0;
        uint32_t xpToNextLevel = 100;

        float strength = 10.0f;
        float dexterity = 10.0f;
        float intelligence = 10.0f;
        float vitality = 10.0f;

        float health = 100.0f;
        float maxHealth = 100.0f;
        float mana = 50.0f;
        float maxMana = 50.0f;
        float moveSpeed = 5.0f;

        int freeAttributePoints = 0;
    };

    /**
     * @brief Hero creation, class registry, leveling, and attribute allocation
     */
    class ARPGHeroSystem
    {
      public:
        ARPGHeroSystem() = default;
        ~ARPGHeroSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Shutdown();
        void Update(float deltaTime);
        void RenderDebugUI();

        // === Hero management ===
        uint32_t CreateHero(const std::string& name, ARPGHeroClass heroClass);
        HeroData* GetHero(uint32_t heroId);
        const HeroData* GetHero(uint32_t heroId) const;
        void GainExperience(uint32_t heroId, uint32_t amount);
        void AllocateAttribute(uint32_t heroId, const std::string& attribute);

        // === Queries ===
        const HeroClassDef* GetClassDef(ARPGHeroClass id) const;
        size_t GetClassCount() const { return m_classDefs.size(); }
        size_t GetHeroCount() const { return m_heroes.size(); }
        std::string GetHeroListString() const;

      private:
        void RegisterClassTemplates();
        void LevelUp(HeroData& hero);
        uint32_t CalculateXPForLevel(int level) const;

        Spark::IEngineContext* m_context{nullptr};
        std::vector<HeroClassDef> m_classDefs;
        std::unordered_map<uint32_t, HeroData> m_heroes;
        uint32_t m_nextHeroId = 1;

        static constexpr int MAX_LEVEL = 70;
        static constexpr int ATTRIBUTE_POINTS_PER_LEVEL = 5;
    };

} // namespace ARPG
