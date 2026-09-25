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
#include <utility>
#include <vector>

namespace ARPG
{
    namespace
    {
        /// Snapshot format version. v3 added the full target instance (rank, name, affixes, stats) and the
        /// run-complete flag; v4 adds every learned skill, remaining cooldowns and the carried loot. Earlier
        /// versions lack that state and are rejected rather than migrated.
        constexpr int SnapshotVersion = 4;
        constexpr size_t MaxPersistedMonsterNameLength = 64;
        constexpr size_t MaxPersistedLootTextLength = 64;
        /// Parse-time allocation bound only; ARPGLootSystem::IsRestorableItem enforces the per-rarity count.
        constexpr size_t MaxPersistedItemAffixes = 16;

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
            bool runComplete = false;
            bool hasTarget = false;
            MonsterData target;
            std::vector<uint32_t> learnedSkills;
            std::vector<SkillCooldownState> cooldowns;
            std::vector<ItemData> loot;
        };

        /// Reads a non-negative element count no larger than @p maxCount. Counts are parsed signed so a
        /// negative value is rejected instead of wrapping through unsigned extraction.
        bool ParseCount(std::istringstream& snapshot, size_t maxCount, size_t& count)
        {
            long long parsed = 0;
            if (!(snapshot >> parsed) || parsed < 0 || static_cast<unsigned long long>(parsed) > maxCount)
                return false;
            count = static_cast<size_t>(parsed);
            return true;
        }

        bool ParseSkillState(std::istringstream& snapshot, const ARPGSkillSystem& skills, ARPGDemoSnapshot& result)
        {
            size_t learnedCount = 0;
            if (!ParseCount(snapshot, skills.GetTotalSkillCount(), learnedCount))
                return false;
            result.learnedSkills.resize(learnedCount);
            for (uint32_t& skillId : result.learnedSkills)
            {
                if (!(snapshot >> skillId))
                    return false;
            }

            size_t cooldownCount = 0;
            if (!ParseCount(snapshot, learnedCount, cooldownCount))
                return false;
            result.cooldowns.resize(cooldownCount);
            for (SkillCooldownState& cooldown : result.cooldowns)
            {
                if (!(snapshot >> cooldown.skillId >> cooldown.remainingCooldown))
                    return false;
            }

            // The primary skill is always one of the learned skills.
            return std::ranges::find(result.learnedSkills, result.primarySkillId) != result.learnedSkills.end() &&
                   skills.CanRestoreHeroSkills(result.heroClass, result.level, result.learnedSkills, result.cooldowns);
        }

        bool ParseItem(std::istringstream& snapshot, const ARPGLootSystem& loot, ItemData& item)
        {
            int slot = 0;
            int rarity = 0;
            size_t affixCount = 0;
            if (!(snapshot >> item.itemId >> std::quoted(item.name) >> slot >> rarity >> item.itemLevel >>
                  item.baseDamage >> item.baseArmor) ||
                item.name.size() > MaxPersistedLootTextLength || slot < 0 ||
                slot >= static_cast<int>(ARPGItemSlot::Count) || rarity < 0 ||
                rarity >= static_cast<int>(ARPGItemRarity::Count) ||
                !ParseCount(snapshot, MaxPersistedItemAffixes, affixCount))
                return false;

            item.slot = static_cast<ARPGItemSlot>(slot);
            item.rarity = static_cast<ARPGItemRarity>(rarity);
            item.affixes.resize(affixCount);
            for (AffixData& affix : item.affixes)
            {
                if (!(snapshot >> std::quoted(affix.name) >> std::quoted(affix.statType) >> affix.minValue >>
                      affix.maxValue >> affix.rolledValue) ||
                    affix.name.size() > MaxPersistedLootTextLength ||
                    affix.statType.size() > MaxPersistedLootTextLength)
                    return false;
            }
            return loot.IsRestorableItem(item);
        }

        bool ParseLoot(std::istringstream& snapshot, const ARPGLootSystem& loot, ARPGDemoSnapshot& result)
        {
            size_t itemCount = 0;
            if (!ParseCount(snapshot, ARPGDemoEncounter::MaxCarriedLoot, itemCount))
                return false;
            result.loot.resize(itemCount);
            for (size_t i = 0; i < itemCount; ++i)
            {
                if (!ParseItem(snapshot, loot, result.loot[i]))
                    return false;
                // Item IDs identify carried loot, so two carried items may never share one.
                for (size_t j = 0; j < i; ++j)
                {
                    if (result.loot[j].itemId == result.loot[i].itemId)
                        return false;
                }
            }
            return true;
        }

        bool IsBossFloor(int floor)
        {
            return floor > 0 && floor % ARPGDungeonSystem::BOSS_FLOOR_INTERVAL == 0;
        }

        bool ParseTarget(std::istringstream& snapshot, MonsterData& target)
        {
            int rank = 0;
            int damageType = 0;
            int affixCount = 0;
            if (!(snapshot >> std::quoted(target.name) >> rank >> target.level >> target.health >> target.maxHealth >>
                  target.damage >> damageType >> target.moveSpeed >> target.xpReward >> target.lootChance >>
                  affixCount) ||
                target.name.size() > MaxPersistedMonsterNameLength || rank < 0 ||
                rank >= static_cast<int>(ARPGMonsterRank::Count) || damageType < 0 ||
                damageType >= static_cast<int>(ARPGDamageType::Count) || affixCount < 0 ||
                affixCount > static_cast<int>(ChampionAffix::Count))
                return false;

            target.rank = static_cast<ARPGMonsterRank>(rank);
            target.damageType = static_cast<ARPGDamageType>(damageType);
            target.affixes.clear();
            for (int i = 0; i < affixCount; ++i)
            {
                int affix = 0;
                if (!(snapshot >> affix) || affix < 0 || affix >= static_cast<int>(ChampionAffix::Count))
                    return false;
                target.affixes.push_back(static_cast<ChampionAffix>(affix));
            }
            return ARPGMonsterSystem::IsRestorableMonster(target);
        }

        bool ParseARPGDemoSnapshot(const std::string& serializedState, const ARPGSkillSystem* skills,
                                   const ARPGLootSystem* loot, ARPGDemoSnapshot& result)
        {
            if (!skills || !loot)
                return false;

            std::istringstream snapshot(serializedState);
            std::string magic;
            int version = 0;
            int heroClass = 0;
            int runComplete = 0;
            int hasTarget = 0;
            if (!(snapshot >> magic >> version) || magic != "ARPGDEMO" || version != SnapshotVersion)
                return false;

            if (!(snapshot >> result.floor >> result.killsOnFloor >> result.totalKills >> heroClass >> result.level >>
                  result.experience >> result.xpToNextLevel >> result.strength >> result.dexterity >>
                  result.intelligence >> result.vitality >> result.health >> result.maxHealth >> result.mana >>
                  result.maxMana >> result.moveSpeed >> result.freeAttributePoints >> result.primarySkillId >>
                  runComplete >> hasTarget) ||
                result.floor < 1 || result.floor > ARPGDemoEncounter::RunGoalFloor ||
                result.killsOnFloor >= ARPGDemoEncounter::KillsPerFloor || heroClass < 0 ||
                heroClass >= static_cast<int>(ARPGHeroClass::Count) || result.level < 1 || result.level > 70 ||
                result.xpToNextLevel == 0 || !std::isfinite(result.strength) || !std::isfinite(result.dexterity) ||
                !std::isfinite(result.intelligence) || !std::isfinite(result.vitality) ||
                !std::isfinite(result.health) || !std::isfinite(result.maxHealth) || !std::isfinite(result.mana) ||
                !std::isfinite(result.maxMana) || !std::isfinite(result.moveSpeed) || result.maxHealth <= 0.0f ||
                result.maxMana < 0.0f || result.moveSpeed <= 0.0f || result.freeAttributePoints < 0 ||
                result.health < 0.0f || result.health > result.maxHealth || result.mana < 0.0f ||
                result.mana > result.maxMana || (runComplete != 0 && runComplete != 1) ||
                (hasTarget != 0 && hasTarget != 1))
                return false;

            result.runComplete = runComplete == 1;
            result.hasTarget = hasTarget == 1;

            // A live run always has exactly one target; a completed run has none and sits on the goal floor
            // with the boss recorded as the floor's single kill.
            if (result.runComplete == result.hasTarget)
                return false;
            if (result.runComplete && (result.floor != ARPGDemoEncounter::RunGoalFloor || result.killsOnFloor != 1))
                return false;
            if (!result.runComplete && IsBossFloor(result.floor) && result.killsOnFloor != 0)
                return false;

            if (result.hasTarget)
            {
                if (!ParseTarget(snapshot, result.target))
                    return false;
                // The encounter spawns exactly one Boss on the boss floor and Normal monsters elsewhere.
                const ARPGMonsterRank expectedRank =
                    IsBossFloor(result.floor) ? ARPGMonsterRank::Boss : ARPGMonsterRank::Normal;
                if (result.target.rank != expectedRank)
                    return false;
            }

            result.heroClass = static_cast<ARPGHeroClass>(heroClass);
            if (!ParseSkillState(snapshot, *skills, result) || !ParseLoot(snapshot, *loot, result))
                return false;

            snapshot >> std::ws;
            return snapshot.eof();
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
        if (!m_monsters || !m_dungeon || m_state.runComplete)
            return;

        const MonsterData* target = GetTarget();
        if (!target)
        {
            m_state.targetMonsterId = 0;
            SpawnNextTarget();
            return;
        }

        if (target->health <= 0.0f)
            HandleDefeat(target->xpReward, target->rank);
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
        m_state.runComplete = false;
        m_state.lastDropRank = ARPGMonsterRank::Normal;
        m_state.lastDropRarity = ARPGItemRarity::Normal;
        m_state.lastDropItemId = 0;

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
        const ARPGMonsterRank rank = target->rank;
        if (!m_monsters->DamageMonster(target->monsterId, result.finalDamage))
            return false;

        m_state.lastDamage = result.finalDamage;
        m_state.lastAttackWasSkill = isSkill;
        if (const MonsterData* damaged = GetTarget(); damaged && damaged->health <= 0.0f)
            HandleDefeat(xpReward, rank);
        return true;
    }

    void ARPGDemoEncounter::HandleDefeat(float xpReward, ARPGMonsterRank rank)
    {
        if (m_heroes)
            m_heroes->GainExperience(m_state.heroId, static_cast<uint32_t>(std::max(1.0f, xpReward)));
        if (m_loot)
        {
            // Loot quality follows the defeated monster's rank, so a boss kill rolls on the Boss table.
            const int floor = m_dungeon ? std::max(1, m_dungeon->GetCurrentFloorNumber()) : 1;
            const ItemData drop = m_loot->GenerateRandomDrop(floor, rank);
            m_state.lastDropRank = rank;
            m_state.lastDropRarity = drop.rarity;
            m_state.lastDropItemId = drop.itemId;
            if (m_state.collectedLoot.size() < MaxCarriedLoot)
                m_state.collectedLoot.push_back(drop);
        }

        ++m_state.killsOnFloor;
        ++m_state.totalKills;
        m_monsters->Update(0.0f);
        m_state.targetMonsterId = 0;

        // Clearing the boss floor ends the run: no descent and no further spawns until Restart().
        const DungeonLevel* currentFloor = m_dungeon ? m_dungeon->GetCurrentFloor() : nullptr;
        if (currentFloor && currentFloor->hasBoss)
        {
            m_state.runComplete = true;
            return;
        }

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
        status << "Loot carried: " << m_state.collectedLoot.size() << "/" << MaxCarriedLoot << "\n";
        if (m_state.runComplete)
            status << "Dungeon cleared: boss defeated on floor " << RunGoalFloor << " (press R to run again)\n";
        else if (target)
            status << "Target: " << target->name << " Lv" << target->level << " HP " << target->health << "/"
                   << target->maxHealth << "\n";
        status << "Controls: Space basic attack, Q primary skill, R restart encounter";
        return status.str();
    }

    std::string ARPGDemoEncounter::SerializeState() const
    {
        const HeroData* hero = GetHero();
        const MonsterData* target = GetTarget();
        if (!hero || !m_dungeon || !m_skills || (!target && !m_state.runComplete))
            return {};

        std::ostringstream snapshot;
        snapshot << std::setprecision(std::numeric_limits<float>::max_digits10) << "ARPGDEMO " << SnapshotVersion << ' '
                 << m_dungeon->GetCurrentFloorNumber() << ' ' << m_state.killsOnFloor << ' ' << m_state.totalKills
                 << ' ' << static_cast<int>(hero->heroClass) << ' ' << hero->level << ' ' << hero->experience << ' '
                 << hero->xpToNextLevel << ' ' << hero->strength << ' ' << hero->dexterity << ' ' << hero->intelligence
                 << ' ' << hero->vitality << ' ' << hero->health << ' ' << hero->maxHealth << ' ' << hero->mana << ' '
                 << hero->maxMana << ' ' << hero->moveSpeed << ' ' << hero->freeAttributePoints << ' '
                 << m_state.primarySkillId << ' ' << (m_state.runComplete ? 1 : 0) << ' '
                 << (m_state.runComplete ? 0 : 1);
        if (!m_state.runComplete)
        {
            snapshot << ' ' << std::quoted(target->name) << ' ' << static_cast<int>(target->rank) << ' '
                     << target->level << ' ' << target->health << ' ' << target->maxHealth << ' ' << target->damage
                     << ' ' << static_cast<int>(target->damageType) << ' ' << target->moveSpeed << ' '
                     << target->xpReward << ' ' << target->lootChance << ' ' << target->affixes.size();
            for (const ChampionAffix affix : target->affixes)
                snapshot << ' ' << static_cast<int>(affix);
        }

        const std::vector<uint32_t> learnedSkills = m_skills->GetLearnedSkills(hero->heroId);
        snapshot << ' ' << learnedSkills.size();
        for (const uint32_t skillId : learnedSkills)
            snapshot << ' ' << skillId;
        const std::vector<SkillCooldownState> cooldowns = m_skills->GetCooldowns(hero->heroId);
        snapshot << ' ' << cooldowns.size();
        for (const SkillCooldownState& cooldown : cooldowns)
            snapshot << ' ' << cooldown.skillId << ' ' << cooldown.remainingCooldown;

        snapshot << ' ' << m_state.collectedLoot.size();
        for (const ItemData& item : m_state.collectedLoot)
        {
            snapshot << ' ' << item.itemId << ' ' << std::quoted(item.name) << ' ' << static_cast<int>(item.slot) << ' '
                     << static_cast<int>(item.rarity) << ' ' << item.itemLevel << ' ' << item.baseDamage << ' '
                     << item.baseArmor << ' ' << item.affixes.size();
            for (const AffixData& affix : item.affixes)
                snapshot << ' ' << std::quoted(affix.name) << ' ' << std::quoted(affix.statType) << ' '
                         << affix.minValue << ' ' << affix.maxValue << ' ' << affix.rolledValue;
        }
        return snapshot.str();
    }

    bool ARPGDemoEncounter::CanRestoreState(const std::string& serializedState) const
    {
        ARPGDemoSnapshot snapshot;
        return m_heroes && m_dungeon && m_monsters &&
               ParseARPGDemoSnapshot(serializedState, m_skills, m_loot, snapshot);
    }

    bool ARPGDemoEncounter::RestoreState(const std::string& serializedState)
    {
        if (!m_heroes || !m_dungeon || !m_monsters || !m_skills || !m_loot)
            return false;

        // Validate everything before touching live state so a rejected snapshot leaves the run untouched.
        ARPGDemoSnapshot snapshot;
        if (!ParseARPGDemoSnapshot(serializedState, m_skills, m_loot, snapshot))
            return false;
        HeroData* hero = m_heroes->GetHero(m_state.heroId);
        if (!hero)
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
        m_state.runComplete = snapshot.runComplete;
        m_state.targetMonsterId = 0;

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
        // The hero now carries the snapshot's class and level, which the skill state was validated against.
        if (!m_skills->RestoreHeroSkills(hero->heroId, snapshot.learnedSkills, snapshot.cooldowns))
        {
            Restart();
            return false;
        }

        // Restored items keep their IDs; new drops must be numbered after every carried item.
        m_state.collectedLoot = std::move(snapshot.loot);
        for (const ItemData& item : m_state.collectedLoot)
            m_loot->ReserveItemId(item.itemId);

        if (snapshot.runComplete)
            return true;

        // Re-register the saved instance verbatim; re-rolling SpawnBoss would change the boss's name and affixes.
        m_state.targetMonsterId = m_monsters->RestoreMonster(snapshot.target);
        if (m_state.targetMonsterId == 0)
        {
            Restart();
            return false;
        }
        return true;
    }
} // namespace ARPG
