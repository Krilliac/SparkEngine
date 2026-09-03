/**
 * @file TestGameModeReal.cpp
 * @brief Real-class tests for Spark::GameMode, PlayerScore, GameModeRules, SpawnPoint
 */

#include "TestFramework.h"
#include "Game/GameMode.h"

using namespace Spark;

// ============================================================================
// PlayerScore
// ============================================================================

TEST(GameModeReal_PlayerScoreDefaults)
{
    PlayerScore ps;
    EXPECT_EQ(ps.kills, 0);
    EXPECT_EQ(ps.deaths, 0);
    EXPECT_EQ(ps.assists, 0);
    EXPECT_EQ(ps.totalScore, 0);
    EXPECT_EQ(ps.headshots, 0);
    EXPECT_EQ(ps.currentStreak, 0);
    EXPECT_NEAR(ps.damageDealt, 0.0f, 0.001f);
}

TEST(GameModeReal_KDRatioZeroDeaths)
{
    PlayerScore ps;
    ps.kills = 5;
    ps.deaths = 0;
    EXPECT_NEAR(ps.GetKDRatio(), 5.0f, 0.001f);
}

TEST(GameModeReal_KDRatioNormal)
{
    PlayerScore ps;
    ps.kills = 10;
    ps.deaths = 4;
    EXPECT_NEAR(ps.GetKDRatio(), 2.5f, 0.001f);
}

// ============================================================================
// GameModeRules
// ============================================================================

TEST(GameModeReal_RulesDefaults)
{
    GameModeRules rules;
    EXPECT_TRUE(rules.type == GameModeType::FreePlay);
    EXPECT_EQ(rules.scoreLimit, 50);
    EXPECT_EQ(rules.roundLimit, 1);
    EXPECT_NEAR(rules.respawnDelay, 3.0f, 0.001f);
    EXPECT_TRUE(rules.autoRespawn);
    EXPECT_TRUE(rules.headshots);
    EXPECT_FALSE(rules.friendlyFire);
    EXPECT_TRUE(rules.allWeaponsAvailable);
    EXPECT_EQ(rules.killPoints, 100);
}

// ============================================================================
// SpawnPoint
// ============================================================================

TEST(GameModeReal_SpawnPointDefaultCtor)
{
    SpawnPoint sp;
    EXPECT_TRUE(sp.isActive);
    EXPECT_TRUE(sp.team == Team::None);
}

TEST(GameModeReal_SpawnPointPositionCtor)
{
    SpawnPoint sp(1.0f, 2.0f, 3.0f);
    EXPECT_NEAR(sp.position.x, 1.0f, 0.001f);
    EXPECT_NEAR(sp.position.y, 2.0f, 0.001f);
    EXPECT_NEAR(sp.position.z, 3.0f, 0.001f);
}

// ============================================================================
// GameMode full API (requires GameMode.cpp linked)
// ============================================================================

TEST(GameModeReal_InitializeAndState)
{
    GameMode gm;
    GameModeRules rules;
    rules.type = GameModeType::Deathmatch;
    rules.modeName = "DM Test";
    EXPECT_TRUE(gm.Initialize(rules));
    EXPECT_FALSE(gm.IsMatchActive());
    EXPECT_TRUE(gm.GetRoundState() == RoundState::WaitingForPlayers);
}

TEST(GameModeReal_AddPlayerAndScore)
{
    GameMode gm;
    gm.Initialize(GameModeRules{});
    gm.AddPlayer("Alice");
    gm.AddPlayer("Bob");

    gm.RecordKill("Alice", "Bob", false);
    const auto* score = gm.GetPlayerScore("Alice");
    ASSERT_TRUE(score != nullptr);
    EXPECT_EQ(score->kills, 1);

    const auto* bScore = gm.GetPlayerScore("Bob");
    ASSERT_TRUE(bScore != nullptr);
    EXPECT_EQ(bScore->deaths, 1);
}

TEST(GameModeReal_GetPresetDeathmatch)
{
    auto rules = GameMode::GetPreset(GameModeType::Deathmatch);
    EXPECT_EQ(rules.scoreLimit, 30);
    EXPECT_FALSE(rules.teamsEnabled);
    EXPECT_EQ(rules.modeName, std::string("Deathmatch"));
}

TEST(GameModeReal_TypeToString)
{
    EXPECT_EQ(std::string(GameMode::GameModeTypeToString(GameModeType::Survival)), std::string("Survival"));
    EXPECT_EQ(std::string(GameMode::GameModeTypeToString(GameModeType::CaptureTheFlag)),
              std::string("Capture the Flag"));
}

TEST(GameModeReal_SpawnPointManagement)
{
    GameMode gm;
    gm.Initialize(GameModeRules{});
    gm.AddSpawnPoint(SpawnPoint(10.0f, 0.0f, 20.0f));
    gm.AddSpawnPoint(SpawnPoint(30.0f, 0.0f, 40.0f));
    EXPECT_EQ(gm.GetSpawnPoints().size(), 2u);
    gm.ClearSpawnPoints();
    EXPECT_EQ(gm.GetSpawnPoints().size(), 0u);
}
