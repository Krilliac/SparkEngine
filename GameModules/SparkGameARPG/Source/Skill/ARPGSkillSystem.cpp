/**
 * @file ARPGSkillSystem.cpp
 * @brief Skill tree population, learning, cooldown tracking, and usage
 */

#include "ARPGSkillSystem.h"
#include "Engine/Security/MemoryIntegrity.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <sstream>

namespace ARPG
{

    bool ARPGSkillSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterSkillTrees();

        SPARK_LOG_INFO(Spark::LogCategory::Game, "ARPG skill system initialized with %zu skills", m_allSkills.size());
        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Skill system initialized (" +
                                                    std::to_string(m_allSkills.size()) + " skills)");
        return true;
    }

    void ARPGSkillSystem::Shutdown()
    {
        m_learnedSkills.clear();
        m_cooldowns.clear();
    }

    void ARPGSkillSystem::Update(float deltaTime)
    {
        // Tick down all active cooldowns
        for (auto& [heroId, cooldowns] : m_cooldowns)
        {
            for (auto& cd : cooldowns)
            {
                if (cd.remainingCooldown > 0.0f)
                    cd.remainingCooldown -= deltaTime;
            }

            // Remove expired cooldowns
            std::erase_if(cooldowns, [](const SkillCooldownState& cd) { return cd.remainingCooldown <= 0.0f; });
        }
    }

    void ARPGSkillSystem::AddSkill(const SkillData& skill)
    {
        m_allSkills.push_back(skill);
    }

    void ARPGSkillSystem::RegisterSkillTrees()
    {
        m_allSkills.clear();
        m_nextSkillId = 1;

        // === Barbarian skills ===
        AddSkill({m_nextSkillId++, "Whirlwind", "Spin dealing damage to all nearby enemies", ARPGSkillType::Channeled,
                  ARPGHeroClass::Barbarian, 15.0f, 0.0f, ARPGDamageType::Physical, 35.0f, 1});
        AddSkill({m_nextSkillId++, "Leap", "Jump to a target area, stunning enemies on landing", ARPGSkillType::Active,
                  ARPGHeroClass::Barbarian, 20.0f, 8.0f, ARPGDamageType::Physical, 50.0f, 5});
        AddSkill({m_nextSkillId++, "War Cry", "Increases damage and armor for nearby allies", ARPGSkillType::Aura,
                  ARPGHeroClass::Barbarian, 25.0f, 30.0f, ARPGDamageType::Physical, 0.0f, 10});
        AddSkill({m_nextSkillId++, "Frenzy", "Each hit increases attack speed, stacking", ARPGSkillType::Active,
                  ARPGHeroClass::Barbarian, 5.0f, 0.0f, ARPGDamageType::Physical, 20.0f, 15});
        AddSkill({m_nextSkillId++, "Iron Skin", "Passively reduces all physical damage taken", ARPGSkillType::Passive,
                  ARPGHeroClass::Barbarian, 0.0f, 0.0f, ARPGDamageType::Physical, 0.0f, 20});

        // === Sorceress skills ===
        AddSkill({m_nextSkillId++, "Fireball", "Hurls an explosive ball of fire", ARPGSkillType::Active,
                  ARPGHeroClass::Sorceress, 12.0f, 1.5f, ARPGDamageType::Fire, 45.0f, 1});
        AddSkill({m_nextSkillId++, "Frozen Orb", "Launches a freezing sphere that shatters", ARPGSkillType::Active,
                  ARPGHeroClass::Sorceress, 20.0f, 3.0f, ARPGDamageType::Cold, 55.0f, 5});
        AddSkill({m_nextSkillId++, "Chain Lightning", "Lightning arcs between multiple targets", ARPGSkillType::Active,
                  ARPGHeroClass::Sorceress, 18.0f, 2.0f, ARPGDamageType::Lightning, 40.0f, 10});
        AddSkill({m_nextSkillId++, "Teleport", "Instantly move to target location", ARPGSkillType::Active,
                  ARPGHeroClass::Sorceress, 25.0f, 6.0f, ARPGDamageType::Arcane, 0.0f, 15});
        AddSkill({m_nextSkillId++, "Arcane Mastery", "Passively reduces mana cost of all spells",
                  ARPGSkillType::Passive, ARPGHeroClass::Sorceress, 0.0f, 0.0f, ARPGDamageType::Arcane, 0.0f, 20});

        // === Necromancer skills ===
        AddSkill({m_nextSkillId++, "Raise Skeleton", "Summons a skeleton warrior to fight", ARPGSkillType::Summon,
                  ARPGHeroClass::Necromancer, 15.0f, 5.0f, ARPGDamageType::Physical, 15.0f, 1});
        AddSkill({m_nextSkillId++, "Corpse Explosion", "Detonates a corpse dealing AoE damage", ARPGSkillType::Active,
                  ARPGHeroClass::Necromancer, 10.0f, 1.0f, ARPGDamageType::Physical, 60.0f, 5});
        AddSkill({m_nextSkillId++, "Poison Nova", "Releases a ring of poison in all directions", ARPGSkillType::Active,
                  ARPGHeroClass::Necromancer, 30.0f, 8.0f, ARPGDamageType::Poison, 35.0f, 10});
        AddSkill({m_nextSkillId++, "Bone Armor", "Surrounds caster with a bone shield", ARPGSkillType::Active,
                  ARPGHeroClass::Necromancer, 20.0f, 15.0f, ARPGDamageType::Physical, 0.0f, 15});

        // === Paladin skills ===
        AddSkill({m_nextSkillId++, "Blessed Hammer", "Throws a spiraling holy hammer", ARPGSkillType::Active,
                  ARPGHeroClass::Paladin, 12.0f, 0.0f, ARPGDamageType::Holy, 40.0f, 1});
        AddSkill({m_nextSkillId++, "Conviction", "Aura that reduces enemy resistances", ARPGSkillType::Aura,
                  ARPGHeroClass::Paladin, 0.0f, 0.0f, ARPGDamageType::Holy, 0.0f, 5});
        AddSkill({m_nextSkillId++, "Smite", "Bashes an enemy with shield, stunning them", ARPGSkillType::Active,
                  ARPGHeroClass::Paladin, 8.0f, 4.0f, ARPGDamageType::Holy, 30.0f, 10});
        AddSkill({m_nextSkillId++, "Holy Shield", "Passively increases block chance", ARPGSkillType::Passive,
                  ARPGHeroClass::Paladin, 0.0f, 0.0f, ARPGDamageType::Holy, 0.0f, 15});

        // === Amazon skills ===
        AddSkill({m_nextSkillId++, "Multiple Shot", "Fires a spread of arrows at enemies", ARPGSkillType::Active,
                  ARPGHeroClass::Amazon, 10.0f, 0.0f, ARPGDamageType::Physical, 25.0f, 1});
        AddSkill({m_nextSkillId++, "Lightning Fury", "Throws a charged javelin that splits", ARPGSkillType::Active,
                  ARPGHeroClass::Amazon, 18.0f, 2.0f, ARPGDamageType::Lightning, 50.0f, 5});
        AddSkill({m_nextSkillId++, "Valkyrie", "Summons a powerful valkyrie companion", ARPGSkillType::Summon,
                  ARPGHeroClass::Amazon, 30.0f, 20.0f, ARPGDamageType::Physical, 25.0f, 15});
        AddSkill({m_nextSkillId++, "Critical Strike", "Passively increases critical hit chance", ARPGSkillType::Passive,
                  ARPGHeroClass::Amazon, 0.0f, 0.0f, ARPGDamageType::Physical, 0.0f, 10});

        // === Rogue skills ===
        AddSkill({m_nextSkillId++, "Shadow Strike", "Teleport behind target and stab", ARPGSkillType::Active,
                  ARPGHeroClass::Rogue, 15.0f, 6.0f, ARPGDamageType::Physical, 55.0f, 1});
        AddSkill({m_nextSkillId++, "Fan of Knives", "Throws daggers in all directions", ARPGSkillType::Active,
                  ARPGHeroClass::Rogue, 20.0f, 8.0f, ARPGDamageType::Physical, 30.0f, 5});
        AddSkill({m_nextSkillId++, "Poison Trap", "Places a trap that poisons enemies", ARPGSkillType::Active,
                  ARPGHeroClass::Rogue, 12.0f, 10.0f, ARPGDamageType::Poison, 20.0f, 10});
        AddSkill({m_nextSkillId++, "Blade Fury", "Channels a barrage of thrown blades", ARPGSkillType::Channeled,
                  ARPGHeroClass::Rogue, 8.0f, 0.0f, ARPGDamageType::Physical, 18.0f, 15});
    }

    const SkillData* ARPGSkillSystem::GetSkill(uint32_t skillId) const
    {
        for (const auto& skill : m_allSkills)
        {
            if (skill.skillId == skillId)
                return &skill;
        }
        return nullptr;
    }

    std::vector<const SkillData*> ARPGSkillSystem::GetAvailableSkills(ARPGHeroClass heroClass, int heroLevel) const
    {
        std::vector<const SkillData*> result;
        for (const auto& skill : m_allSkills)
        {
            if (skill.heroClass == heroClass && skill.requiredLevel <= heroLevel)
                result.push_back(&skill);
        }
        return result;
    }

    bool ARPGSkillSystem::LearnSkill(uint32_t heroId, uint32_t skillId, int heroLevel)
    {
        const auto* skill = GetSkill(skillId);
        if (!skill || heroLevel < skill->requiredLevel)
            return false;

        auto& learned = m_learnedSkills[heroId];

        // Check if already learned
        for (uint32_t id : learned)
        {
            if (id == skillId)
                return false;
        }

        learned.push_back(skillId);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "ARPG skill learned: %s (id=%u)", skill->name.c_str(), skillId);
        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Learned skill: " + skill->name);
        return true;
    }

    bool ARPGSkillSystem::UseSkill(uint32_t heroId, uint32_t skillId)
    {
        const auto* skill = GetSkill(skillId);
        if (!skill)
            return false;

        // Check if on cooldown — bypassing allows instant ability spam
        SPARK_BRANCH_GUARD_BEGIN("arpg_cooldown_check")
        auto cdIt = m_cooldowns.find(heroId);
        if (cdIt != m_cooldowns.end())
        {
            for (const auto& cd : cdIt->second)
            {
                if (cd.skillId == skillId && cd.remainingCooldown > 0.0f)
                    return false; // Still on cooldown
            }
        }
        SPARK_BRANCH_GUARD_END("arpg_cooldown_check")

        // Apply cooldown if skill has one — bypassing skips cooldown entirely
        SPARK_BRANCH_GUARD_BEGIN("arpg_cooldown_apply")
        if (skill->cooldown > 0.0f)
        {
            m_cooldowns[heroId].push_back({skillId, skill->cooldown});
        }
        SPARK_BRANCH_GUARD_END("arpg_cooldown_apply")
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "ARPG skill used: %s (cd=%.1fs)", skill->name.c_str(),
                        skill->cooldown);

        return true;
    }

    std::vector<uint32_t> ARPGSkillSystem::GetLearnedSkills(uint32_t heroId) const
    {
        auto it = m_learnedSkills.find(heroId);
        if (it != m_learnedSkills.end())
            return it->second;
        return {};
    }

    std::string ARPGSkillSystem::GetSkillListString() const
    {
        std::ostringstream ss;
        ss << "=== ARPG Skill Trees ===\n";

        static const char* classNames[] = {"Barbarian", "Sorceress", "Necromancer", "Paladin", "Amazon", "Rogue"};

        for (int c = 0; c < static_cast<int>(ARPGHeroClass::Count); ++c)
        {
            auto heroClass = static_cast<ARPGHeroClass>(c);
            ss << "\n" << classNames[c] << ":\n";

            for (const auto& skill : m_allSkills)
            {
                if (skill.heroClass != heroClass)
                    continue;

                static const char* typeNames[] = {"Active", "Passive", "Aura", "Channeled", "Summon"};
                auto typeIndex = static_cast<size_t>(skill.type);
                const char* typeName = (typeIndex < 5) ? typeNames[typeIndex] : "?";

                ss << "  [Lv" << skill.requiredLevel << "] " << skill.name << " (" << typeName << ")";
                if (skill.manaCost > 0.0f)
                    ss << " MP:" << skill.manaCost;
                if (skill.cooldown > 0.0f)
                    ss << " CD:" << skill.cooldown << "s";
                if (skill.baseDamage > 0.0f)
                    ss << " Dmg:" << skill.baseDamage;
                ss << "\n    " << skill.description << "\n";
            }
        }
        return ss.str();
    }

    void ARPGSkillSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("ARPG Skill System"))
        {
            ImGui::Text("Total skills: %zu", m_allSkills.size());

            static const char* classNames[] = {"Barbarian", "Sorceress", "Necromancer", "Paladin", "Amazon", "Rogue"};

            for (int c = 0; c < static_cast<int>(ARPGHeroClass::Count); ++c)
            {
                auto heroClass = static_cast<ARPGHeroClass>(c);
                if (ImGui::TreeNode(classNames[c]))
                {
                    for (const auto& skill : m_allSkills)
                    {
                        if (skill.heroClass != heroClass)
                            continue;
                        ImGui::Text("[Lv%d] %s - %s", skill.requiredLevel, skill.name.c_str(),
                                    skill.description.c_str());
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace ARPG
