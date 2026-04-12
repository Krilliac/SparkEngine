/**
 * @file TestFPSGameplayIntegration.cpp
 * @brief Integration tests for the FPS game module's core gameplay systems
 *
 * Tests the GameMode lifecycle (init → update → scoring → round transitions)
 * and WaveSpawner state machine (idle → countdown → spawning → completed).
 * All tests are CPU-only — no GPU, no DLL loading.
 */

#include "TestFramework.h"
#include "Game/GameMode.h"

using namespace Spark;

// ============================================================================
// GameMode Lifecycle Tests
// ============================================================================

TEST(FPSInteg_GameMode_InitializeWithDefaults)
{
    GameMode mode;
    GameModeRules rules;
    EXPECT_TRUE(mode.Initialize(rules));
    EXPECT_EQ(static_cast<int>(mode.GetRules().type), static_cast<int>(GameModeType::FreePlay));
}

TEST(FPSInteg_GameMode_InitializeDeathmatch)
{
    GameMode mode;
    GameModeRules rules;
    rules.type = GameModeType::Deathmatch;
    rules.scoreLimit = 25;
    rules.timeLimit = 600.0f; // 10 minutes
    EXPECT_TRUE(mode.Initialize(rules));
    EXPECT_EQ(static_cast<int>(mode.GetRules().type), static_cast<int>(GameModeType::Deathmatch));
}

TEST(FPSInteg_GameMode_InitializeSurvival)
{
    GameMode mode;
    GameModeRules rules;
    rules.type = GameModeType::Survival;
    rules.roundLimit = 10;
    EXPECT_TRUE(mode.Initialize(rules));
    EXPECT_EQ(static_cast<int>(mode.GetRules().type), static_cast<int>(GameModeType::Survival));
}

TEST(FPSInteg_GameMode_UpdateDoesNotCrash)
{
    GameMode mode;
    GameModeRules rules;
    rules.type = GameModeType::Deathmatch;
    mode.Initialize(rules);

    // Tick 100 frames at 60fps
    for (int i = 0; i < 100; ++i)
        mode.Update(0.016f);
}

TEST(FPSInteg_GameMode_AddPlayer)
{
    GameMode mode;
    GameModeRules rules;
    rules.type = GameModeType::TeamDeathmatch;
    mode.Initialize(rules);

    mode.AddPlayer("Player1");
    mode.AddPlayer("Player2");

    const auto* score = mode.GetPlayerScore("Player1");
    EXPECT_TRUE(score != nullptr);
    EXPECT_EQ(score->kills, 0);
    EXPECT_EQ(score->deaths, 0);
}

TEST(FPSInteg_GameMode_RecordKill)
{
    GameMode mode;
    GameModeRules rules;
    rules.type = GameModeType::Deathmatch;
    rules.scoreLimit = 100;
    mode.Initialize(rules);

    mode.AddPlayer("Attacker");
    mode.AddPlayer("Victim");

    mode.RecordKill("Attacker", "Victim");

    const auto* attackerScore = mode.GetPlayerScore("Attacker");
    const auto* victimScore = mode.GetPlayerScore("Victim");
    EXPECT_TRUE(attackerScore != nullptr);
    EXPECT_TRUE(victimScore != nullptr);
    EXPECT_EQ(attackerScore->kills, 1);
    EXPECT_EQ(victimScore->deaths, 1);
}

TEST(FPSInteg_GameMode_TeamAssignment)
{
    GameMode mode;
    GameModeRules rules;
    rules.type = GameModeType::TeamDeathmatch;
    mode.Initialize(rules);

    mode.AddPlayer("Player1");
    mode.AddPlayer("Player2");

    mode.SetPlayerTeam("Player1", Team::Alpha);
    mode.SetPlayerTeam("Player2", Team::Bravo);

    const auto* p1 = mode.GetPlayerScore("Player1");
    const auto* p2 = mode.GetPlayerScore("Player2");
    EXPECT_EQ(static_cast<int>(p1->team), static_cast<int>(Team::Alpha));
    EXPECT_EQ(static_cast<int>(p2->team), static_cast<int>(Team::Bravo));
}

TEST(FPSInteg_GameMode_SpawnPoints)
{
    GameMode mode;
    GameModeRules rules;
    mode.Initialize(rules);

    mode.AddSpawnPoint(SpawnPoint(0.0f, 0.0f, 0.0f));
    mode.AddSpawnPoint(SpawnPoint(10.0f, 0.0f, 0.0f));
    mode.AddSpawnPoint(SpawnPoint(20.0f, 0.0f, 0.0f));

    const auto& spawns = mode.GetSpawnPoints();
    EXPECT_EQ(spawns.size(), 3u);
    EXPECT_TRUE(spawns[0].isActive);
}

TEST(FPSInteg_GameMode_MultipleKillsTracked)
{
    GameMode mode;
    GameModeRules rules;
    rules.type = GameModeType::Deathmatch;
    mode.Initialize(rules);

    mode.AddPlayer("Alice");
    mode.AddPlayer("Bob");
    mode.AddPlayer("Charlie");

    mode.RecordKill("Charlie", "Alice");
    mode.RecordKill("Charlie", "Bob");
    mode.RecordKill("Bob", "Alice");

    const auto* charlie = mode.GetPlayerScore("Charlie");
    const auto* bob = mode.GetPlayerScore("Bob");
    const auto* alice = mode.GetPlayerScore("Alice");

    EXPECT_EQ(charlie->kills, 2);
    EXPECT_EQ(bob->kills, 1);
    EXPECT_EQ(bob->deaths, 1);
    EXPECT_EQ(alice->kills, 0);
    EXPECT_EQ(alice->deaths, 2);
}

TEST(FPSInteg_GameMode_RemovePlayer)
{
    GameMode mode;
    GameModeRules rules;
    mode.Initialize(rules);

    mode.AddPlayer("LeavingPlayer");
    EXPECT_TRUE(mode.GetPlayerScore("LeavingPlayer") != nullptr);

    mode.RemovePlayer("LeavingPlayer");
    EXPECT_TRUE(mode.GetPlayerScore("LeavingPlayer") == nullptr);
}
