/**
 * @file TestMOD330ARPGDungeonReal.cpp
 * @brief MOD-330: the ARPG dungeon run is finite, the boss identity is authoritative and persisted, and hero,
 *        skill, cooldown and loot state survive a real SaveSystem save, full system rebuild and load
 *
 * Every test drives the real SparkGameARPG sources (hero, combat, loot, dungeon, skill, monster and the
 * demo encounter) through BasicAttack/UsePrimarySkill, the same entry points the Space/Q keyboard input
 * and the arpg_attack/arpg_skill console commands call. The monster RNG is global and unseeded, so the
 * assertions compare restored state against captured state rather than against specific boss names.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameARPG/Source/Combat/ARPGCombatSystem.h"
#include "../GameModules/SparkGameARPG/Source/Demo/ARPGDemoEncounter.h"
#include "../GameModules/SparkGameARPG/Source/Dungeon/ARPGDungeonSystem.h"
#include "../GameModules/SparkGameARPG/Source/Hero/ARPGHeroSystem.h"
#include "../GameModules/SparkGameARPG/Source/Loot/ARPGLootSystem.h"
#include "../GameModules/SparkGameARPG/Source/Monster/ARPGMonsterSystem.h"
#include "../GameModules/SparkGameARPG/Source/Skill/ARPGSkillSystem.h"
#include "Engine/ECS/Components.h"
#include "Engine/SaveSystem/SaveSystem.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ARPG;

namespace
{
    struct ARPGRun
    {
        ARPGHeroSystem heroes;
        ARPGCombatSystem combat;
        ARPGLootSystem loot;
        ARPGDungeonSystem dungeon;
        ARPGSkillSystem skills;
        ARPGMonsterSystem monsters;
        ARPGDemoEncounter encounter;
        bool initialized = false;

        ARPGRun()
        {
            heroes.Initialize(nullptr);
            combat.Initialize(nullptr);
            loot.Initialize(nullptr);
            dungeon.Initialize(nullptr);
            skills.Initialize(nullptr, &heroes);
            monsters.Initialize(nullptr);
            initialized = encounter.Initialize(&heroes, &combat, &loot, &dungeon, &skills, &monsters);
        }

        ~ARPGRun()
        {
            encounter.Shutdown();
            monsters.Shutdown();
            skills.Shutdown();
            dungeon.Shutdown();
            loot.Shutdown();
            combat.Shutdown();
            heroes.Shutdown();
        }

        ARPGRun(const ARPGRun&) = delete;
        ARPGRun& operator=(const ARPGRun&) = delete;

        /// One scripted input frame: cast the primary skill when possible, otherwise swing.
        bool Step()
        {
            const bool acted = encounter.UsePrimarySkill() || encounter.BasicAttack();
            skills.Update(0.25f);
            encounter.Update();
            return acted;
        }

        /// Fight until the boss of the goal floor is the current target. Returns false on a stall.
        bool AdvanceToBoss()
        {
            for (int frame = 0; frame < 5000; ++frame)
            {
                const MonsterData* target = encounter.GetTarget();
                if (dungeon.GetCurrentFloorNumber() == ARPGDemoEncounter::RunGoalFloor && target &&
                    target->rank == ARPGMonsterRank::Boss)
                    return true;
                if (!Step())
                    return false;
            }
            return false;
        }

        /// Fight the current target until the run completes. Returns false on a stall.
        bool FinishRun()
        {
            for (int frame = 0; frame < 50000 && !encounter.IsRunComplete(); ++frame)
            {
                if (!Step())
                    return false;
            }
            return encounter.IsRunComplete();
        }
    };

    void ExpectSameMonster(const MonsterData& actual, const MonsterData& expected)
    {
        EXPECT_EQ(actual.name, expected.name);
        EXPECT_TRUE(actual.rank == expected.rank);
        EXPECT_EQ(actual.level, expected.level);
        EXPECT_EQ(actual.health, expected.health);
        EXPECT_EQ(actual.maxHealth, expected.maxHealth);
        EXPECT_EQ(actual.damage, expected.damage);
        EXPECT_TRUE(actual.damageType == expected.damageType);
        EXPECT_EQ(actual.moveSpeed, expected.moveSpeed);
        EXPECT_EQ(actual.xpReward, expected.xpReward);
        EXPECT_EQ(actual.lootChance, expected.lootChance);
        EXPECT_TRUE(actual.affixes == expected.affixes);
    }

    void ExpectSameItem(const ItemData& actual, const ItemData& expected)
    {
        EXPECT_EQ(actual.itemId, expected.itemId);
        EXPECT_EQ(actual.name, expected.name);
        EXPECT_TRUE(actual.slot == expected.slot);
        EXPECT_TRUE(actual.rarity == expected.rarity);
        EXPECT_EQ(actual.itemLevel, expected.itemLevel);
        EXPECT_EQ(actual.baseDamage, expected.baseDamage);
        EXPECT_EQ(actual.baseArmor, expected.baseArmor);
        ASSERT_EQ(actual.affixes.size(), expected.affixes.size());
        for (size_t i = 0; i < actual.affixes.size(); ++i)
        {
            EXPECT_EQ(actual.affixes[i].name, expected.affixes[i].name);
            EXPECT_EQ(actual.affixes[i].statType, expected.affixes[i].statType);
            EXPECT_EQ(actual.affixes[i].minValue, expected.affixes[i].minValue);
            EXPECT_EQ(actual.affixes[i].maxValue, expected.affixes[i].maxValue);
            EXPECT_EQ(actual.affixes[i].rolledValue, expected.affixes[i].rolledValue);
        }
    }

    void ExpectSameHero(const HeroData& actual, const HeroData& expected)
    {
        EXPECT_EQ(actual.name, expected.name);
        EXPECT_TRUE(actual.heroClass == expected.heroClass);
        EXPECT_EQ(actual.level, expected.level);
        EXPECT_EQ(actual.experience, expected.experience);
        EXPECT_EQ(actual.xpToNextLevel, expected.xpToNextLevel);
        EXPECT_EQ(actual.strength, expected.strength);
        EXPECT_EQ(actual.dexterity, expected.dexterity);
        EXPECT_EQ(actual.intelligence, expected.intelligence);
        EXPECT_EQ(actual.vitality, expected.vitality);
        EXPECT_EQ(actual.health, expected.health);
        EXPECT_EQ(actual.maxHealth, expected.maxHealth);
        EXPECT_EQ(actual.mana, expected.mana);
        EXPECT_EQ(actual.maxMana, expected.maxMana);
        EXPECT_EQ(actual.moveSpeed, expected.moveSpeed);
        EXPECT_EQ(actual.freeAttributePoints, expected.freeAttributePoints);
    }

    /// Temporary SaveSystem root that restores the singleton's save directory afterwards.
    class ScopedARPGSaveDirectory
    {
      public:
        explicit ScopedARPGSaveDirectory(const char* name)
            : m_saveSystem(Spark::SaveSystem::GetInstance()), m_previousDirectory(m_saveSystem.GetSaveDirectory()),
              m_directory(std::filesystem::temp_directory_path() / (std::string("spark_mod330_") + name))
        {
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
            std::filesystem::create_directories(m_directory, error);
            m_saveSystem.SetFileCache(nullptr);
            m_initialized = m_saveSystem.Initialize(m_directory.string());
        }

        ~ScopedARPGSaveDirectory()
        {
            m_saveSystem.SetSaveDirectory(m_previousDirectory);
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
        }

        ScopedARPGSaveDirectory(const ScopedARPGSaveDirectory&) = delete;
        ScopedARPGSaveDirectory& operator=(const ScopedARPGSaveDirectory&) = delete;

        bool IsInitialized() const { return m_initialized; }
        Spark::SaveSystem& System() { return m_saveSystem; }
        const std::filesystem::path& Path() const { return m_directory; }

      private:
        Spark::SaveSystem& m_saveSystem;
        std::string m_previousDirectory;
        std::filesystem::path m_directory;
        bool m_initialized = false;
    };

    /// The customState key the module's arpg_save/arpg_load commands use (Core/Main.cpp).
    constexpr const char* DemoStateKey = "SparkGameARPG.demo.v1";

    /// Split the fixed-layout prefix of an ARPGDEMO 4 snapshot into tokens; the tail (quoted monster name and
    /// target fields) is returned unsplit so names containing spaces are preserved.
    std::vector<std::string> SplitPrefix(const std::string& snapshot, size_t prefixTokens, std::string& tail)
    {
        std::istringstream stream(snapshot);
        std::vector<std::string> tokens;
        std::string token;
        while (tokens.size() < prefixTokens && stream >> token)
            tokens.push_back(token);
        std::getline(stream, tail);
        return tokens;
    }

    std::string Join(const std::vector<std::string>& tokens, const std::string& tail)
    {
        std::string joined;
        for (const std::string& token : tokens)
            joined += (joined.empty() ? "" : " ") + token;
        return joined + tail;
    }

    // Index of each field in the ARPGDEMO 4 fixed prefix.
    constexpr size_t FloorToken = 2;
    constexpr size_t RunCompleteToken = 20;
    constexpr size_t HasTargetToken = 21;
    constexpr size_t FixedPrefixTokens = 22;
} // namespace

TEST(ARPGDungeon_ScriptedHeroClearsBossFloorAndCompletes)
{
    ARPGRun run;
    ASSERT_TRUE(run.initialized);
    EXPECT_EQ(run.dungeon.GetCurrentFloorNumber(), 1);
    EXPECT_FALSE(run.encounter.IsRunComplete());

    ASSERT_TRUE(run.AdvanceToBoss());
    EXPECT_EQ(run.dungeon.GetCurrentFloorNumber(), ARPGDemoEncounter::RunGoalFloor);
    EXPECT_TRUE(run.dungeon.GetCurrentFloor()->hasBoss);
    EXPECT_EQ(run.encounter.GetState().killsOnFloor, 0u);

    ASSERT_TRUE(run.FinishRun());

    // Every regular floor took KillsPerFloor kills, and the boss floor ended the run with one kill.
    const uint32_t expectedKills =
        static_cast<uint32_t>(ARPGDemoEncounter::RunGoalFloor - 1) * ARPGDemoEncounter::KillsPerFloor + 1u;
    EXPECT_EQ(run.encounter.GetState().totalKills, expectedKills);
    EXPECT_EQ(run.loot.GetGeneratedItemCount(), static_cast<size_t>(expectedKills));
    EXPECT_EQ(run.dungeon.GetCurrentFloorNumber(), ARPGDemoEncounter::RunGoalFloor);
    EXPECT_TRUE(run.encounter.GetTarget() == nullptr);
    EXPECT_EQ(run.monsters.GetActiveMonsterCount(), 0u);

    // The run stays finished: input is refused and Update() spawns nothing and descends nowhere.
    EXPECT_FALSE(run.encounter.BasicAttack());
    EXPECT_FALSE(run.encounter.UsePrimarySkill());
    for (int frame = 0; frame < 10; ++frame)
        run.encounter.Update();
    EXPECT_TRUE(run.encounter.IsRunComplete());
    EXPECT_TRUE(run.encounter.GetTarget() == nullptr);
    EXPECT_EQ(run.dungeon.GetCurrentFloorNumber(), ARPGDemoEncounter::RunGoalFloor);
    EXPECT_TRUE(run.encounter.GetStatusString().find("Dungeon cleared") != std::string::npos);

    // A completed run persists and restores as completed.
    const std::string completed = run.encounter.SerializeState();
    ASSERT_FALSE(completed.empty());
    run.encounter.Restart();
    EXPECT_FALSE(run.encounter.IsRunComplete());
    EXPECT_EQ(run.dungeon.GetCurrentFloorNumber(), 1);
    EXPECT_TRUE(run.encounter.GetTarget() != nullptr);

    ASSERT_TRUE(run.encounter.RestoreState(completed));
    EXPECT_TRUE(run.encounter.IsRunComplete());
    EXPECT_TRUE(run.encounter.GetTarget() == nullptr);
    EXPECT_EQ(run.dungeon.GetCurrentFloorNumber(), ARPGDemoEncounter::RunGoalFloor);
    EXPECT_EQ(run.encounter.GetState().totalKills, expectedKills);
    EXPECT_EQ(run.encounter.SerializeState(), completed);
}

TEST(ARPGBoss_RestoredBossKeepsIdentityAndAffixes)
{
    ARPGRun run;
    ASSERT_TRUE(run.initialized);
    ASSERT_TRUE(run.AdvanceToBoss());
    ASSERT_TRUE(run.encounter.BasicAttack());

    const MonsterData* boss = run.encounter.GetTarget();
    ASSERT_TRUE(boss != nullptr);
    ASSERT_TRUE(boss->rank == ARPGMonsterRank::Boss);
    EXPECT_FALSE(boss->affixes.empty());
    EXPECT_LT(boss->health, boss->maxHealth);
    const MonsterData savedBoss = *boss;
    const std::string snapshot = run.encounter.SerializeState();
    ASSERT_FALSE(snapshot.empty());

    // Perturb live state and the global monster RNG so a re-rolled boss would be detectably different
    // in at least one of the many compared fields across repeated runs.
    run.encounter.Restart();
    for (int roll = 0; roll < 7; ++roll)
        run.monsters.SpawnBoss(1);

    ASSERT_TRUE(run.encounter.CanRestoreState(snapshot));
    ASSERT_TRUE(run.encounter.RestoreState(snapshot));
    const MonsterData* restored = run.encounter.GetTarget();
    ASSERT_TRUE(restored != nullptr);
    ExpectSameMonster(*restored, savedBoss);
    EXPECT_EQ(run.monsters.GetActiveMonsterCount(), 1u);
    EXPECT_EQ(run.dungeon.GetCurrentFloorNumber(), ARPGDemoEncounter::RunGoalFloor);
    EXPECT_EQ(run.encounter.SerializeState(), snapshot);

    // The monster system holds the same authoritative copy the encounter targets.
    const MonsterData* stored = run.monsters.GetMonster(run.encounter.GetState().targetMonsterId);
    ASSERT_TRUE(stored != nullptr);
    ExpectSameMonster(*stored, savedBoss);

    // Restoring twice yields the same boss again (no RNG on the restore path).
    ASSERT_TRUE(run.encounter.RestoreState(snapshot));
    ExpectSameMonster(*run.encounter.GetTarget(), savedBoss);

    // The restored boss is the one the run finishes against.
    ASSERT_TRUE(run.FinishRun());
    EXPECT_TRUE(run.encounter.GetState().lastDropRank == ARPGMonsterRank::Boss);
}

TEST(ARPGBoss_BossKillDropsBossRankLoot)
{
    ARPGRun run;
    ASSERT_TRUE(run.initialized);

    // Regular floors roll on the Normal-rank table.
    for (int frame = 0; frame < 100 && run.encounter.GetState().totalKills == 0; ++frame)
        ASSERT_TRUE(run.Step());
    ASSERT_EQ(run.encounter.GetState().totalKills, 1u);
    EXPECT_TRUE(run.encounter.GetState().lastDropRank == ARPGMonsterRank::Normal);
    EXPECT_NE(run.encounter.GetState().lastDropItemId, 0u);

    ASSERT_TRUE(run.AdvanceToBoss());
    const size_t itemsBeforeBoss = run.loot.GetGeneratedItemCount();
    const uint32_t lastRegularDrop = run.encounter.GetState().lastDropItemId;
    ASSERT_TRUE(run.FinishRun());

    EXPECT_EQ(run.loot.GetGeneratedItemCount(), itemsBeforeBoss + 1u);
    EXPECT_TRUE(run.encounter.GetState().lastDropRank == ARPGMonsterRank::Boss);
    EXPECT_NE(run.encounter.GetState().lastDropItemId, 0u);
    EXPECT_NE(run.encounter.GetState().lastDropItemId, lastRegularDrop);
    EXPECT_TRUE(run.encounter.GetState().lastDropRarity < ARPGItemRarity::Count);
}

TEST(ARPGBoss_RejectsLegacyOrTruncatedSnapshotWithoutMutation)
{
    ARPGRun run;
    ASSERT_TRUE(run.initialized);
    ASSERT_TRUE(run.AdvanceToBoss());
    ASSERT_TRUE(run.encounter.BasicAttack());
    const std::string bossSnapshot = run.encounter.SerializeState();
    ASSERT_FALSE(bossSnapshot.empty());

    // Park the live run on floor 1 with a damaged target, then prove every bad snapshot leaves it untouched.
    run.encounter.Restart();
    ASSERT_TRUE(run.encounter.BasicAttack());
    const std::string liveState = run.encounter.SerializeState();
    ASSERT_FALSE(liveState.empty());
    const uint32_t liveTargetId = run.encounter.GetState().targetMonsterId;

    std::vector<std::string> rejected;

    // Legacy v2 layout (target health/maxHealth only), and a v4 body mislabelled as v2 or v3.
    rejected.push_back("ARPGDEMO 2 5 0 12 0 6 100 200 30 20 10 25 150 150 50 50 5 0 1 800 1000");
    std::string relabelled = bossSnapshot;
    relabelled.replace(relabelled.find("ARPGDEMO 4"), 10, "ARPGDEMO 2");
    rejected.push_back(relabelled);
    std::string previousVersion = bossSnapshot;
    previousVersion.replace(previousVersion.find("ARPGDEMO 4"), 10, "ARPGDEMO 3");
    rejected.push_back(previousVersion);

    // Every truncation at a token boundary.
    for (size_t pos = bossSnapshot.find(' '); pos != std::string::npos; pos = bossSnapshot.find(' ', pos + 1))
        rejected.push_back(bossSnapshot.substr(0, pos));
    rejected.push_back(bossSnapshot + " 7");

    // Internally inconsistent v3 snapshots.
    std::string tail;
    const std::vector<std::string> tokens = SplitPrefix(bossSnapshot, FixedPrefixTokens, tail);
    ASSERT_EQ(tokens.size(), FixedPrefixTokens);
    {
        std::vector<std::string> bossOnRegularFloor = tokens;
        bossOnRegularFloor[FloorToken] = "4";
        rejected.push_back(Join(bossOnRegularFloor, tail));
    }
    {
        std::vector<std::string> pastGoalFloor = tokens;
        pastGoalFloor[FloorToken] = std::to_string(ARPGDemoEncounter::RunGoalFloor + 1);
        rejected.push_back(Join(pastGoalFloor, tail));
    }
    {
        std::vector<std::string> completeWithTarget = tokens;
        completeWithTarget[RunCompleteToken] = "1";
        rejected.push_back(Join(completeWithTarget, tail));
    }
    {
        std::vector<std::string> targetlessLiveRun = tokens;
        targetlessLiveRun[HasTargetToken] = "0";
        rejected.push_back(Join(targetlessLiveRun, ""));
    }
    {
        // Boss rank (4) replaced by Normal (0) on the boss floor.
        std::string demotedTail = tail;
        const size_t nameEnd = demotedTail.find('"', demotedTail.find('"') + 1);
        ASSERT_TRUE(nameEnd != std::string::npos);
        ASSERT_EQ(demotedTail.substr(nameEnd + 1, 3), std::string(" 4 "));
        demotedTail.replace(nameEnd + 1, 3, " 0 ");
        rejected.push_back(Join(tokens, demotedTail));
    }

    for (const std::string& candidate : rejected)
    {
        EXPECT_FALSE(run.encounter.CanRestoreState(candidate));
        EXPECT_FALSE(run.encounter.RestoreState(candidate));
        EXPECT_EQ(run.encounter.SerializeState(), liveState);
        EXPECT_EQ(run.encounter.GetState().targetMonsterId, liveTargetId);
    }
    EXPECT_EQ(run.dungeon.GetCurrentFloorNumber(), 1);
    EXPECT_FALSE(run.encounter.IsRunComplete());

    // The untampered snapshot still restores the boss after all the rejections.
    ASSERT_TRUE(run.encounter.RestoreState(bossSnapshot));
    EXPECT_EQ(run.encounter.SerializeState(), bossSnapshot);
}

TEST(ARPGDungeon_SaveRestartRestoresHeroSkillsLootAndBoss)
{
    ScopedARPGSaveDirectory saves("restart");
    ASSERT_TRUE(saves.IsInitialized());

    HeroData savedHero;
    std::vector<uint32_t> savedLearned;
    std::vector<SkillCooldownState> savedCooldowns;
    std::vector<ItemData> savedLoot;
    MonsterData savedBoss;
    ARPGDemoEncounterState savedState;
    std::string snapshot;
    uint32_t cooldownSkillId = 0;
    {
        auto run = std::make_unique<ARPGRun>();
        ASSERT_TRUE(run->initialized);
        ASSERT_TRUE(run->AdvanceToBoss());

        // Level the hero far enough to unlock a skill with a cooldown, learn everything the level unlocks,
        // and put the cooldown skill on cooldown with part of it already elapsed.
        const uint32_t heroId = run->encounter.GetState().heroId;
        for (int guard = 0; guard < 10 && run->heroes.GetHero(heroId)->level < 5; ++guard)
            run->heroes.GainExperience(heroId, run->heroes.GetHero(heroId)->xpToNextLevel);
        const HeroData* hero = run->heroes.GetHero(heroId);
        ASSERT_TRUE(hero->level >= 5);
        for (const SkillData* skill : run->skills.GetAvailableSkills(hero->heroClass, hero->level))
        {
            run->skills.LearnSkill(heroId, skill->skillId);
            if (skill->cooldown > 0.0f && cooldownSkillId == 0)
                cooldownSkillId = skill->skillId;
        }
        ASSERT_NE(cooldownSkillId, 0u);
        ASSERT_TRUE(run->skills.UseSkill(heroId, cooldownSkillId));
        run->skills.Update(1.25f);
        ASSERT_TRUE(run->encounter.BasicAttack());

        savedHero = *run->heroes.GetHero(heroId);
        savedLearned = run->skills.GetLearnedSkills(heroId);
        savedCooldowns = run->skills.GetCooldowns(heroId);
        savedState = run->encounter.GetState();
        savedLoot = savedState.collectedLoot;
        savedBoss = *run->encounter.GetTarget();
        ASSERT_TRUE(savedLearned.size() >= 2u);
        ASSERT_EQ(savedCooldowns.size(), 1u);
        EXPECT_GT(savedCooldowns.front().remainingCooldown, 0.0f);
        EXPECT_LT(savedCooldowns.front().remainingCooldown, run->skills.GetSkill(cooldownSkillId)->cooldown);
        // Every kill before the boss floor dropped one carried item.
        ASSERT_EQ(savedLoot.size(), static_cast<size_t>(savedState.totalKills));
        ASSERT_EQ(savedLoot.size(),
                  static_cast<size_t>(ARPGDemoEncounter::RunGoalFloor - 1) * ARPGDemoEncounter::KillsPerFloor);
        ASSERT_TRUE(savedBoss.rank == ARPGMonsterRank::Boss);

        snapshot = run->encounter.SerializeState();
        ASSERT_FALSE(snapshot.empty());
        World world;
        Spark::SaveMetadata meta;
        meta.saveName = "ARPG MOD-330 restart";
        const std::unordered_map<std::string, std::string> customState = {{DemoStateKey, snapshot}};
        ASSERT_TRUE(saves.System().Save("arpg_restart", world, meta, customState));
    }

    // Restart: every ARPG system and the World are rebuilt from scratch and SaveSystem is re-initialized.
    ASSERT_TRUE(saves.System().Initialize(saves.Path().string()));
    auto restarted = std::make_unique<ARPGRun>();
    ASSERT_TRUE(restarted->initialized);
    EXPECT_TRUE(restarted->encounter.GetState().collectedLoot.empty());
    EXPECT_EQ(restarted->dungeon.GetCurrentFloorNumber(), 1);

    World loadedWorld;
    std::unordered_map<std::string, std::string> loadedState;
    const auto validate = [&restarted](const std::unordered_map<std::string, std::string>& candidate)
    {
        const auto state = candidate.find(DemoStateKey);
        return state != candidate.end() && restarted->encounter.CanRestoreState(state->second);
    };
    ASSERT_TRUE(saves.System().Load("arpg_restart", loadedWorld, loadedState, validate));
    ASSERT_TRUE(loadedState.contains(DemoStateKey));
    EXPECT_EQ(loadedState.at(DemoStateKey), snapshot);
    ASSERT_TRUE(restarted->encounter.RestoreState(loadedState.at(DemoStateKey)));

    // Hero, run progress, skills, cooldowns, loot and boss all match field by field.
    const uint32_t heroId = restarted->encounter.GetState().heroId;
    ExpectSameHero(*restarted->heroes.GetHero(heroId), savedHero);
    EXPECT_EQ(restarted->dungeon.GetCurrentFloorNumber(), ARPGDemoEncounter::RunGoalFloor);
    EXPECT_EQ(restarted->encounter.GetState().totalKills, savedState.totalKills);
    EXPECT_EQ(restarted->encounter.GetState().killsOnFloor, savedState.killsOnFloor);
    EXPECT_EQ(restarted->encounter.GetState().primarySkillId, savedState.primarySkillId);
    EXPECT_FALSE(restarted->encounter.IsRunComplete());
    EXPECT_TRUE(restarted->skills.GetLearnedSkills(heroId) == savedLearned);
    const std::vector<SkillCooldownState> restoredCooldowns = restarted->skills.GetCooldowns(heroId);
    ASSERT_EQ(restoredCooldowns.size(), savedCooldowns.size());
    EXPECT_EQ(restoredCooldowns.front().skillId, savedCooldowns.front().skillId);
    EXPECT_EQ(restoredCooldowns.front().remainingCooldown, savedCooldowns.front().remainingCooldown);
    const std::vector<ItemData>& restoredLoot = restarted->encounter.GetState().collectedLoot;
    ASSERT_EQ(restoredLoot.size(), savedLoot.size());
    for (size_t i = 0; i < restoredLoot.size(); ++i)
        ExpectSameItem(restoredLoot[i], savedLoot[i]);
    ASSERT_TRUE(restarted->encounter.GetTarget() != nullptr);
    ExpectSameMonster(*restarted->encounter.GetTarget(), savedBoss);
    EXPECT_EQ(restarted->encounter.SerializeState(), snapshot);

    // The restored cooldown is live: the skill stays locked until the remaining time elapses.
    EXPECT_FALSE(restarted->skills.UseSkill(heroId, cooldownSkillId));
    restarted->skills.Update(savedCooldowns.front().remainingCooldown + 0.01f);
    EXPECT_TRUE(restarted->skills.GetCooldowns(heroId).empty());
    EXPECT_TRUE(restarted->skills.UseSkill(heroId, cooldownSkillId));

    // Loot IDs stay stable: the boss drop after the restart is numbered after every restored item.
    ASSERT_TRUE(restarted->FinishRun());
    const std::vector<ItemData>& finalLoot = restarted->encounter.GetState().collectedLoot;
    ASSERT_EQ(finalLoot.size(), savedLoot.size() + 1u);
    for (size_t i = 0; i < savedLoot.size(); ++i)
        EXPECT_EQ(finalLoot[i].itemId, savedLoot[i].itemId);
    const uint32_t bossDropId = finalLoot.back().itemId;
    EXPECT_EQ(bossDropId, restarted->encounter.GetState().lastDropItemId);
    for (const ItemData& item : savedLoot)
        EXPECT_GT(bossDropId, item.itemId);
    EXPECT_TRUE(restarted->encounter.GetState().lastDropRank == ARPGMonsterRank::Boss);
}

TEST(ARPGDungeon_RejectsForgedLootAndSkillStateWithoutMutation)
{
    ARPGRun run;
    ASSERT_TRUE(run.initialized);

    // Item validation: a generated item restores; any field that generation could not produce is refused.
    const ItemData rare = run.loot.GenerateItem(3, ARPGItemRarity::Rare);
    ASSERT_TRUE(rare.affixes.size() >= 3u);
    EXPECT_TRUE(run.loot.IsRestorableItem(rare));
    std::vector<ItemData> forged(9, rare);
    forged[0].itemId = 0;
    forged[1].itemId = std::numeric_limits<uint32_t>::max();
    forged[2].name = "Forged Blade";
    forged[3].baseDamage += 1.0f;
    forged[3].baseArmor += 1.0f;
    forged[4].rarity = ARPGItemRarity::Normal; // Normal items carry no affixes
    forged[5].affixes.front().rolledValue = forged[5].affixes.front().maxValue * 10.0f;
    forged[6].affixes.front().statType = "god_mode";
    forged[7].affixes.front().rolledValue = std::numeric_limits<float>::quiet_NaN();
    forged[8].itemLevel = ARPGLootSystem::MAX_RESTORABLE_ITEM_LEVEL + 1;
    for (const ItemData& item : forged)
        EXPECT_FALSE(run.loot.IsRestorableItem(item));

    // Skill validation against the snapshot's class and level.
    const uint32_t primary = run.encounter.GetState().primarySkillId;
    const std::vector<const SkillData*> barbarian = run.skills.GetAvailableSkills(ARPGHeroClass::Barbarian, 70);
    const std::vector<const SkillData*> sorceress = run.skills.GetAvailableSkills(ARPGHeroClass::Sorceress, 70);
    ASSERT_TRUE(barbarian.size() >= 2u);
    ASSERT_FALSE(sorceress.empty());
    const SkillData* leap = barbarian[1];
    ASSERT_TRUE(leap->cooldown > 0.0f && leap->requiredLevel > 1);
    EXPECT_TRUE(run.skills.CanRestoreHeroSkills(ARPGHeroClass::Barbarian, 1, {primary}, {}));
    EXPECT_TRUE(run.skills.CanRestoreHeroSkills(ARPGHeroClass::Barbarian, leap->requiredLevel, {primary, leap->skillId},
                                                {{leap->skillId, leap->cooldown}}));
    EXPECT_FALSE(run.skills.CanRestoreHeroSkills(ARPGHeroClass::Barbarian, 70, {sorceress[0]->skillId}, {}));
    EXPECT_FALSE(run.skills.CanRestoreHeroSkills(ARPGHeroClass::Barbarian, 1, {primary, leap->skillId}, {}));
    EXPECT_FALSE(run.skills.CanRestoreHeroSkills(ARPGHeroClass::Barbarian, 1, {primary, primary}, {}));
    EXPECT_FALSE(run.skills.CanRestoreHeroSkills(ARPGHeroClass::Barbarian, 70, {9999u}, {}));
    const int leapLevel = leap->requiredLevel;
    const std::vector<uint32_t> both = {primary, leap->skillId};
    EXPECT_FALSE(run.skills.CanRestoreHeroSkills(ARPGHeroClass::Barbarian, leapLevel, {primary},
                                                 {{leap->skillId, 1.0f}})); // cooldown on an unlearned skill
    EXPECT_FALSE(run.skills.CanRestoreHeroSkills(ARPGHeroClass::Barbarian, leapLevel, both,
                                                 {{leap->skillId, leap->cooldown + 1.0f}}));
    EXPECT_FALSE(run.skills.CanRestoreHeroSkills(ARPGHeroClass::Barbarian, leapLevel, both, {{leap->skillId, 0.0f}}));
    EXPECT_FALSE(run.skills.CanRestoreHeroSkills(ARPGHeroClass::Barbarian, leapLevel, both,
                                                 {{leap->skillId, std::numeric_limits<float>::infinity()}}));
    EXPECT_FALSE(run.skills.CanRestoreHeroSkills(ARPGHeroClass::Barbarian, leapLevel, both,
                                                 {{leap->skillId, 1.0f}, {leap->skillId, 2.0f}}));

    // Snapshot level: collect some loot, then a snapshot whose carried item was renamed is refused whole.
    for (int frame = 0; frame < 200 && run.encounter.GetState().totalKills < 4; ++frame)
        ASSERT_TRUE(run.Step());
    ASSERT_EQ(run.encounter.GetState().collectedLoot.size(), 4u);
    const std::string liveState = run.encounter.SerializeState();
    ASSERT_FALSE(liveState.empty());

    const std::string lastItemName = "\"" + run.encounter.GetState().collectedLoot.back().name + "\"";
    const size_t namePos = liveState.rfind(lastItemName);
    ASSERT_TRUE(namePos != std::string::npos);
    std::string renamed = liveState;
    renamed.replace(namePos, lastItemName.size(), "\"Forged Blade\"");
    EXPECT_FALSE(run.encounter.CanRestoreState(renamed));

    run.encounter.Restart();
    // Restart ends the run but the hero keeps the carried loot.
    EXPECT_EQ(run.encounter.GetState().collectedLoot.size(), 4u);
    const std::string restartedState = run.encounter.SerializeState();
    EXPECT_FALSE(run.encounter.RestoreState(renamed));
    EXPECT_EQ(run.encounter.SerializeState(), restartedState);

    ASSERT_TRUE(run.encounter.RestoreState(liveState));
    EXPECT_EQ(run.encounter.SerializeState(), liveState);
}

#endif // SPARK_TEST_HAS_IMGUI
