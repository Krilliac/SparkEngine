/**
 * @file TestMOD330ARPGDungeonReal.cpp
 * @brief MOD-330: the ARPG dungeon run is finite and the boss identity is authoritative and persisted
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

#include <sstream>
#include <string>
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

    /// Split the fixed-layout prefix of an ARPGDEMO 3 snapshot into tokens; the tail (quoted monster name and
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

    // Index of each field in the ARPGDEMO 3 fixed prefix.
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

    // Legacy v2 layout (target health/maxHealth only) and a v3 body mislabelled as v2.
    rejected.push_back("ARPGDEMO 2 5 0 12 0 6 100 200 30 20 10 25 150 150 50 50 5 0 1 800 1000");
    std::string relabelled = bossSnapshot;
    relabelled.replace(relabelled.find("ARPGDEMO 3"), 10, "ARPGDEMO 2");
    rejected.push_back(relabelled);

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

#endif // SPARK_TEST_HAS_IMGUI
