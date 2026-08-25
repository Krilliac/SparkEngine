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
#include <iomanip>
#include <limits>
#include <sstream>

namespace ARPG
{
    namespace
    {
        struct ARPGDemoSnapshot
        {
            int floor = 0;
            uint32_t killsOnFloor = 0;
            uint32_t totalKills = 0;
            ARPGHeroClass heroClass = ARPGHeroClass::Barbarian;
            int level = 0;
            uint32_t experience = 0;
            uint32_t xpToNextLevel = 0;
            float strength = 0.0f;
            float dexterity = 0.0f;
            float intelligence = 0.0f;
            float vitality = 0.0f;
            float health = 0.0f;
            float maxHealth = 0.0f;
            float mana = 0.0f;
            float maxMana = 0.0f;
            float moveSpeed = 0.0f;
            int freeAttributePoints = 0;
            uint32_t primarySkillId = 0;
            float targetHealth = 0.0f;
            float targetMaxHealth = 0.0f;
        };

        bool ParseARPGDemoSnapshot(const std::string& serializedState, const ARPGSkillSystem* skills,
                                   uint32_t killsPerFloor, ARPGDemoSnapshot& result)
        {
            std::istringstream snapshot(serializedState);
            std::string magic;
            int version = 0;
            int heroClass = 0;
            if (!(snapshot >> magic >> version >> result.floor >> result.killsOnFloor >> result.totalKills >>
                  heroClass >> result.level >> result.experience >> result.xpToNextLevel >> result.strength >>
                  result.dexterity >> result.intelligence >> result.vitality >> result.health >> result.maxHealth >>
                  result.mana >> result.maxMana >> result.moveSpeed >> result.freeAttributePoints >>
                  result.primarySkillId >> result.targetHealth >> result.targetMaxHealth) ||
                magic != "ARPGDEMO" || version != 2 || result.floor < 1 || result.floor > 1000 ||
                result.killsOnFloor >= killsPerFloor || heroClass < 0 ||
                heroClass >= static_cast<int>(ARPGHeroClass::Count) || result.level < 1 || result.level > 70 ||
                result.xpToNextLevel == 0 || !std::isfinite(result.strength) || !std::isfinite(result.dexterity) ||
                !std::isfinite(result.intelligence) || !std::isfinite(result.vitality) ||
                !std::isfinite(result.health) || !std::isfinite(result.maxHealth) || !std::isfinite(result.mana) ||
                !std::isfinite(result.maxMana) || !std::isfinite(result.moveSpeed) ||
                !std::isfinite(result.targetHealth) || !std::isfinite(result.targetMaxHealth) ||
                result.maxHealth <= 0.0f || result.maxMana < 0.0f || result.moveSpeed <= 0.0f ||
                result.freeAttributePoints < 0 || result.health < 0.0f || result.health > result.maxHealth ||
                result.mana < 0.0f || result.mana > result.maxMana || result.targetHealth <= 0.0f ||
                result.targetMaxHealth <= 0.0f || result.targetHealth > result.targetMaxHealth)
                return false;

            snapshot >> std::ws;
            if (!snapshot.eof())
                return false;

            result.heroClass = static_cast<ARPGHeroClass>(heroClass);
            const SkillData* skill = skills ? skills->GetSkill(result.primarySkillId) : nullptr;
            return skill && skill->heroClass == result.heroClass && skill->requiredLevel <= result.level;
        }
    } // namespace

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

    std::string ARPGDemoEncounter::SerializeState() const
    {
        const HeroData* hero = GetHero();
        const MonsterData* target = GetTarget();
        if (!hero || !target || !m_dungeon)
            return {};

        std::ostringstream snapshot;
        snapshot << std::setprecision(std::numeric_limits<float>::max_digits10) << "ARPGDEMO 2 "
                 << m_dungeon->GetCurrentFloorNumber() << ' ' << m_state.killsOnFloor << ' ' << m_state.totalKills
                 << ' ' << static_cast<int>(hero->heroClass) << ' ' << hero->level << ' ' << hero->experience << ' '
                 << hero->xpToNextLevel << ' ' << hero->strength << ' ' << hero->dexterity << ' ' << hero->intelligence
                 << ' ' << hero->vitality << ' ' << hero->health << ' ' << hero->maxHealth << ' ' << hero->mana << ' '
                 << hero->maxMana << ' ' << hero->moveSpeed << ' ' << hero->freeAttributePoints << ' '
                 << m_state.primarySkillId << ' ' << target->health << ' ' << target->maxHealth;
        return snapshot.str();
    }

    bool ARPGDemoEncounter::CanRestoreState(const std::string& serializedState) const
    {
        ARPGDemoSnapshot snapshot;
        return m_heroes && m_dungeon && m_monsters &&
               ParseARPGDemoSnapshot(serializedState, m_skills, KillsPerFloor, snapshot);
    }

    bool ARPGDemoEncounter::RestoreState(const std::string& serializedState)
    {
        if (!m_heroes || !m_dungeon || !m_monsters)
            return false;

        ARPGDemoSnapshot snapshot;
        if (!ParseARPGDemoSnapshot(serializedState, m_skills, KillsPerFloor, snapshot))
            return false;

        m_monsters->ClearActiveMonsters();
        m_dungeon->SetDungeonTier(ARPGDungeonTier::Normal);
        for (int currentFloor = 0; currentFloor < snapshot.floor; ++currentFloor)
            m_dungeon->DescendToNextFloor();

        m_state.killsOnFloor = snapshot.killsOnFloor;
        m_state.totalKills = snapshot.totalKills;
        m_state.primarySkillId = snapshot.primarySkillId;
        m_state.lastDamage = 0.0f;
        m_state.lastAttackWasSkill = false;
        m_state.targetMonsterId = 0;

        HeroData* hero = m_heroes->GetHero(m_state.heroId);
        if (!hero)
            return false;
        hero->heroClass = snapshot.heroClass;
        hero->level = snapshot.level;
        hero->experience = snapshot.experience;
        hero->xpToNextLevel = snapshot.xpToNextLevel;
        hero->strength = snapshot.strength;
        hero->dexterity = snapshot.dexterity;
        hero->intelligence = snapshot.intelligence;
        hero->vitality = snapshot.vitality;
        hero->health = snapshot.health;
        hero->maxHealth = snapshot.maxHealth;
        hero->mana = snapshot.mana;
        hero->maxMana = snapshot.maxMana;
        hero->moveSpeed = snapshot.moveSpeed;
        hero->freeAttributePoints = snapshot.freeAttributePoints;
        const auto learnedSkills = m_skills->GetLearnedSkills(hero->heroId);
        if (std::find(learnedSkills.begin(), learnedSkills.end(), snapshot.primarySkillId) == learnedSkills.end() &&
            !m_skills->LearnSkill(hero->heroId, snapshot.primarySkillId))
            return false;

        SpawnNextTarget();
        MonsterData* target = m_monsters->GetMonster(m_state.targetMonsterId);
        if (!target)
        {
            Restart();
            return false;
        }
        target->maxHealth = snapshot.targetMaxHealth;
        target->health = snapshot.targetHealth;
        return true;
    }
} // namespace ARPG
