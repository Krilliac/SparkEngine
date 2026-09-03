/**
 * @file TestFPSGameplayIntegration.cpp
 * @brief Integration tests for the FPS game module's core gameplay systems
 *
 * Tests the production GameMode lifecycle (init → countdown → scoring →
 * intermission → next round), validation, and deterministic scoreboard rules.
 * All tests are CPU-only — no GPU or DLL loading.
 */

#include "TestFramework.h"
#include "Game/GameMode.h"

#include <limits>

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
    ASSERT_TRUE(score != nullptr);
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

TEST(FPSInteg_GameMode_RejectsInvalidRulesWithoutStarting)
{
    GameMode mode;
    GameModeRules rules;
    rules.roundLimit = 0;
    EXPECT_FALSE(mode.Initialize(rules));
    EXPECT_FALSE(mode.IsMatchActive());

    rules.roundLimit = 1;
    rules.timeLimit = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(mode.Initialize(rules));
    EXPECT_FALSE(mode.IsMatchActive());
}

TEST(FPSInteg_GameMode_TwoRoundMatchTransitionsDeterministically)
{
    GameMode mode;
    auto rules = GameMode::GetPreset(GameModeType::Deathmatch);
    rules.scoreLimit = 1;
    rules.roundLimit = 2;
    rules.timeLimit = 0.0f;
    EXPECT_TRUE(mode.Initialize(rules));
    mode.AddPlayer("Ranger");
    mode.AddPlayer("Target");

    mode.StartMatch();
    EXPECT_TRUE(mode.IsMatchActive());
    EXPECT_EQ(mode.GetCurrentRound(), 1);
    EXPECT_TRUE(mode.GetRoundState() == RoundState::Countdown);

    mode.Update(3.0f);
    EXPECT_TRUE(mode.GetRoundState() == RoundState::InProgress);
    mode.RecordKill("Ranger", "Target");
    mode.Update(0.016f);
    EXPECT_TRUE(mode.GetRoundState() == RoundState::RoundEnd);
    EXPECT_EQ(mode.GetRoundResults().size(), static_cast<size_t>(1));
    EXPECT_NEAR(mode.GetRoundTransitionTime(), 3.0f, 0.001f);

    mode.Update(3.0f);
    EXPECT_EQ(mode.GetCurrentRound(), 2);
    EXPECT_TRUE(mode.GetRoundState() == RoundState::Countdown);
    mode.Update(3.0f);
    mode.Update(0.016f);
    EXPECT_TRUE(mode.IsMatchActive());
    EXPECT_TRUE(mode.GetRoundState() == RoundState::InProgress);
    mode.RecordKill("Ranger", "Target");
    mode.Update(0.016f);

    EXPECT_FALSE(mode.IsMatchActive());
    EXPECT_TRUE(mode.GetRoundState() == RoundState::MatchEnd);
    EXPECT_EQ(mode.GetRoundResults().size(), static_cast<size_t>(2));
}

TEST(FPSInteg_GameMode_SurvivalDoesNotAutoCompleteAtZeroScoreLimit)
{
    GameMode mode;
    const auto rules = GameMode::GetPreset(GameModeType::Survival);
    EXPECT_TRUE(mode.Initialize(rules));
    mode.AddPlayer("Player1");
    mode.StartMatch();

    mode.Update(3.0f);
    mode.Update(5.0f);

    EXPECT_TRUE(mode.IsMatchActive());
    EXPECT_TRUE(mode.GetRoundState() == RoundState::InProgress);
}

TEST(FPSInteg_GameMode_InvalidDeltaDoesNotAdvanceCountdown)
{
    GameMode mode;
    EXPECT_TRUE(mode.Initialize(GameMode::GetPreset(GameModeType::Deathmatch)));
    mode.AddPlayer("Player1");
    mode.StartMatch();

    mode.Update(-1.0f);
    mode.Update(std::numeric_limits<float>::quiet_NaN());

    EXPECT_NEAR(mode.GetCountdownTime(), 3.0f, 0.001f);
    EXPECT_TRUE(mode.GetRoundState() == RoundState::Countdown);
}

TEST(FPSInteg_GameMode_SuicideRecordsDeathWithoutKillRewards)
{
    GameMode mode;
    GameModeRules rules;
    rules.deathPenalty = 25;
    EXPECT_TRUE(mode.Initialize(rules));
    mode.AddPlayer("Player1");

    mode.RecordKill("Player1", "Player1", true);

    const auto* score = mode.GetPlayerScore("Player1");
    ASSERT_TRUE(score != nullptr);
    EXPECT_EQ(score->kills, 0);
    EXPECT_EQ(score->deaths, 1);
    EXPECT_EQ(score->headshots, 0);
    EXPECT_EQ(score->totalScore, -25);
}

TEST(FPSInteg_GameMode_ScoreboardTieOrderIsStable)
{
    GameMode mode;
    EXPECT_TRUE(mode.Initialize(GameModeRules{}));
    mode.AddPlayer("Charlie");
    mode.AddPlayer("Alice");
    mode.AddPlayer("Bob");

    const auto scoreboard = mode.GetScoreboard();
    EXPECT_EQ(scoreboard.size(), static_cast<size_t>(3));
    EXPECT_EQ(scoreboard[0].playerName, std::string("Alice"));
    EXPECT_EQ(scoreboard[1].playerName, std::string("Bob"));
    EXPECT_EQ(scoreboard[2].playerName, std::string("Charlie"));
}

TEST(FPSInteg_GameMode_CaptureTheFlagRequiresConfiguredCaptureLimit)
{
    GameMode mode;
    const auto rules = GameMode::GetPreset(GameModeType::CaptureTheFlag);
    EXPECT_TRUE(mode.Initialize(rules));
    mode.AddPlayer("Carrier", Team::Alpha);
    mode.StartMatch();
    mode.Update(3.0f);

    mode.RecordObjectiveScore("Carrier", rules.objectivePoints);
    mode.Update(0.016f);
    EXPECT_TRUE(mode.IsMatchActive());

    mode.RecordObjectiveScore("Carrier", rules.objectivePoints);
    mode.RecordObjectiveScore("Carrier", rules.objectivePoints);
    mode.Update(0.016f);
    EXPECT_FALSE(mode.IsMatchActive());
    EXPECT_TRUE(mode.GetRoundState() == RoundState::MatchEnd);
}
