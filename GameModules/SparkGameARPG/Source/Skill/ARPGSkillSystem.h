/**
 * @file ARPGSkillSystem.h
 * @brief Skill trees with active/passive abilities, cooldowns, and class tiers
 * @author Spark Engine Team
 * @date 2026
 *
 * Each hero class has a skill tree with 4-6 skills unlocked at level tiers.
 * Skills have mana costs, cooldowns, damage types, and can be active,
 * passive, aura, channeled, or summon type.
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

    class ARPGHeroSystem;

    /// @brief Definition of a single skill
    struct SkillData
    {
        uint32_t skillId = 0;
        std::string name;
        std::string description;
        ARPGSkillType type = ARPGSkillType::Active;
        ARPGHeroClass heroClass = ARPGHeroClass::Barbarian;
        float manaCost = 0.0f;
        float cooldown = 0.0f; ///< Seconds between uses
        ARPGDamageType damageType = ARPGDamageType::Physical;
        float baseDamage = 0.0f;
        int requiredLevel = 1; ///< Level to unlock
    };

    /// @brief Cooldown state for a learned skill
    struct SkillCooldownState
    {
        uint32_t skillId = 0;
        float remainingCooldown = 0.0f;
    };

    /**
     * @brief Skill trees with per-class abilities, learning, and cooldown tracking
     */
    class ARPGSkillSystem
    {
      public:
        ARPGSkillSystem() = default;
        ~ARPGSkillSystem() = default;

        bool Initialize(Spark::IEngineContext* context, const ARPGHeroSystem* heroSystem);
        void Shutdown();
        void Update(float deltaTime);
        void RenderDebugUI();

        // === Skill management ===
        bool LearnSkill(uint32_t heroId, uint32_t skillId);
        bool UseSkill(uint32_t heroId, uint32_t skillId);
        std::vector<const SkillData*> GetAvailableSkills(ARPGHeroClass heroClass, int heroLevel) const;
        std::vector<uint32_t> GetLearnedSkills(uint32_t heroId) const;

        // === Queries ===
        const SkillData* GetSkill(uint32_t skillId) const;
        size_t GetTotalSkillCount() const { return m_allSkills.size(); }
        std::string GetSkillListString() const;

      private:
        void RegisterSkillTrees();
        void AddSkill(const SkillData& skill);

        Spark::IEngineContext* m_context{nullptr};
        const ARPGHeroSystem* m_heroSystem{nullptr};
        std::vector<SkillData> m_allSkills;

        /// Hero ID -> list of learned skill IDs
        std::unordered_map<uint32_t, std::vector<uint32_t>> m_learnedSkills;

        /// Hero ID -> active cooldown states
        std::unordered_map<uint32_t, std::vector<SkillCooldownState>> m_cooldowns;

        uint32_t m_nextSkillId = 1;
    };

} // namespace ARPG
