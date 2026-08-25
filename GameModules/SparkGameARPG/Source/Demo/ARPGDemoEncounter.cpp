/**
 * @file ARPGDemoEncounter.cpp
 * @brief Playable ARPG encounter orchestration.
 */

#include "ARPGDemoEncounter.h"

#include "Combat/ARPGCombatSystem.h"
#include "Dungeon/ARPGDungeonSystem.h"
#include "Hero/ARPGHeroSystem.h"
#include "Loot/ARPGLootSystem.h"
#include "Monster/ARPGMonsterSystem.h"
#include "Skill/ARPGSkillSystem.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ARPG
{
    bool ARPGDemoEncounter::Initialize(ARPGHeroSystem* heroes, ARPGCombatSystem* combat, ARPGLootSystem* loot,
                                       ARPGDungeonSystem* dungeon, ARPGSkillSystem* skills, ARPGMonsterSystem* monsters)
    {
        if (!heroes || !combat || !loot || !dungeon || !skills || !monsters)
            return false;

        m_heroes = heroes;
        m_combat = combat;
        m_loot = loot;
        m_dungeon = dungeon;
        m_skills = skills;
        m_monsters = monsters;

        m_state = {};
        m_state.heroId = m_heroes->CreateHero("Astra", ARPGHeroClass::Barbarian);
        const HeroData* hero = GetHero();
        if (!hero)
        {
            Shutdown();
            return false;
        }

        const auto availableSkills = m_skills->GetAvailableSkills(hero->heroClass, hero->level);
        if (!availableSkills.empty())
        {
            m_state.primarySkillId = availableSkills.front()->skillId;
            m_skills->LearnSkill(hero->heroId, m_state.primarySkillId);
        }

        Restart();
        return GetTarget() != nullptr;
    }

    void ARPGDemoEncounter::Shutdown()
    {
        m_state = {};
        m_heroes = nullptr;
        m_combat = nullptr;
        m_loot = nullptr;
        m_dungeon = nullptr;
        m_skills = nullptr;
        m_monsters = nullptr;
    }

    void ARPGDemoEncounter::Update()
    {
        if (!m_monsters || !m_dungeon)
            return;

        const MonsterData* target = GetTarget();
        if (!target)
        {
            m_state.targetMonsterId = 0;
            SpawnNextTarget();
            return;
        }

        if (target->health <= 0.0f)
            HandleDefeat(target->xpReward);
    }

    void ARPGDemoEncounter::Restart()
    {
        if (!m_dungeon || !m_monsters)
            return;

        m_monsters->ClearActiveMonsters();
        m_dungeon->SetDungeonTier(ARPGDungeonTier::Normal);
        m_dungeon->DescendToNextFloor();
        m_state.targetMonsterId = 0;
        m_state.killsOnFloor = 0;
        m_state.totalKills = 0;
        m_state.lastDamage = 0.0f;
        m_state.lastAttackWasSkill = false;

        if (HeroData* hero = m_heroes ? m_heroes->GetHero(m_state.heroId) : nullptr)
        {
            hero->health = hero->maxHealth;
            hero->mana = hero->maxMana;
        }
        SpawnNextTarget();
    }

    bool ARPGDemoEncounter::BasicAttack()
    {
        const HeroData* hero = GetHero();
        if (!hero)
            return false;
        return ResolveAttack(15.0f + hero->strength, ARPGDamageType::Physical, false);
    }

    bool ARPGDemoEncounter::UsePrimarySkill()
    {
        HeroData* hero = m_heroes ? m_heroes->GetHero(m_state.heroId) : nullptr;
        const SkillData* skill = m_skills ? m_skills->GetSkill(m_state.primarySkillId) : nullptr;
        if (!hero || !skill || !GetTarget() || hero->mana < skill->manaCost ||
            !m_skills->UseSkill(hero->heroId, skill->skillId))
            return false;

        hero->mana -= skill->manaCost;
        return ResolveAttack(skill->baseDamage, skill->damageType, true);
    }

    const HeroData* ARPGDemoEncounter::GetHero() const
    {
        return m_heroes ? m_heroes->GetHero(m_state.heroId) : nullptr;
    }

    const MonsterData* ARPGDemoEncounter::GetTarget() const
    {
        return m_monsters ? m_monsters->GetMonster(m_state.targetMonsterId) : nullptr;
    }

    bool ARPGDemoEncounter::ResolveAttack(float damage, ARPGDamageType type, bool isSkill)
    {
        const MonsterData* target = GetTarget();
        if (!target || !m_combat || !std::isfinite(damage) || damage <= 0.0f)
            return false;

        DamageInstance attack;
        attack.sourceId = m_state.heroId;
        attack.targetId = target->monsterId;
        attack.baseDamage = damage;
        attack.damageType = type;
        attack.critChance = 0.0f;

        const DamageResult result = m_combat->PerformAttack(attack, {});
        const float xpReward = target->xpReward;
        if (!m_monsters->DamageMonster(target->monsterId, result.finalDamage))
            return false;

        m_state.lastDamage = result.finalDamage;
        m_state.lastAttackWasSkill = isSkill;
        if (const MonsterData* damaged = GetTarget(); damaged && damaged->health <= 0.0f)
            HandleDefeat(xpReward);
        return true;
    }

    void ARPGDemoEncounter::HandleDefeat(float xpReward)
    {
        if (m_heroes)
            m_heroes->GainExperience(m_state.heroId, static_cast<uint32_t>(std::max(1.0f, xpReward)));
        if (m_loot)
        {
            const int floor = m_dungeon ? std::max(1, m_dungeon->GetCurrentFloorNumber()) : 1;
            m_loot->GenerateRandomDrop(floor, ARPGMonsterRank::Normal);
        }

        ++m_state.killsOnFloor;
        ++m_state.totalKills;
        m_monsters->Update(0.0f);
        m_state.targetMonsterId = 0;

        if (m_state.killsOnFloor >= KillsPerFloor && m_dungeon)
        {
            m_dungeon->DescendToNextFloor();
            m_state.killsOnFloor = 0;
        }
        SpawnNextTarget();
    }

    void ARPGDemoEncounter::SpawnNextTarget()
    {
        if (!m_monsters || !m_dungeon)
            return;

        const DungeonLevel* floor = m_dungeon->GetCurrentFloor();
        const int monsterLevel = floor ? std::max(1, floor->monsterLevel) : 1;
        MonsterData target;
        if (floor && floor->hasBoss)
            target = m_monsters->SpawnBoss(monsterLevel);
        else
        {
            static constexpr const char* Names[] = {"Skeleton", "Fallen Demon", "Zombie"};
            target = m_monsters->SpawnMonster(Names[m_state.totalKills % 3], monsterLevel);
        }
        m_state.targetMonsterId = target.monsterId;
    }

    std::string ARPGDemoEncounter::GetStatusString() const
    {
        std::ostringstream status;
        const HeroData* hero = GetHero();
        const MonsterData* target = GetTarget();
        status << "=== ARPG Demo Encounter ===\n";
        status << "Floor: " << (m_dungeon ? m_dungeon->GetCurrentFloorNumber() : 0)
               << " | Kills: " << m_state.totalKills << " (" << m_state.killsOnFloor << "/" << KillsPerFloor << ")\n";
        if (hero)
            status << "Hero: " << hero->name << " Lv" << hero->level << " HP " << hero->health << "/" << hero->maxHealth
                   << " MP " << hero->mana << "/" << hero->maxMana << "\n";
        if (target)
            status << "Target: " << target->name << " Lv" << target->level << " HP " << target->health << "/"
                   << target->maxHealth << "\n";
        status << "Controls: Space basic attack, Q primary skill, R restart encounter";
        return status.str();
    }
} // namespace ARPG
