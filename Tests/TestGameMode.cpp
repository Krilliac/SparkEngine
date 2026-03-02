// TestGameMode.cpp - Tests for game mode scoring and K/D ratio

#include <string>
#include <cmath>

// Standalone PlayerScore test (doesn't need full GameMode system)
struct TestPlayerScore
{
    std::string playerName;
    int kills = 0;
    int deaths = 0;
    int assists = 0;
    int totalScore = 0;
    int currentStreak = 0;
    int headshots = 0;

    float GetKDRatio() const {
        return deaths > 0 ? static_cast<float>(kills) / deaths : static_cast<float>(kills);
    }
};

TEST(PlayerScore_InitialState)
{
    TestPlayerScore score;
    score.playerName = "TestPlayer";
    EXPECT_EQ(score.kills, 0);
    EXPECT_EQ(score.deaths, 0);
    EXPECT_EQ(score.assists, 0);
    EXPECT_NEAR(score.GetKDRatio(), 0.0f, 0.001f);
}

TEST(PlayerScore_KDRatio_PositiveKD)
{
    TestPlayerScore score;
    score.kills = 10;
    score.deaths = 5;
    EXPECT_NEAR(score.GetKDRatio(), 2.0f, 0.001f);
}

TEST(PlayerScore_KDRatio_ZeroDeaths)
{
    TestPlayerScore score;
    score.kills = 5;
    score.deaths = 0;
    EXPECT_NEAR(score.GetKDRatio(), 5.0f, 0.001f);
}

TEST(PlayerScore_KDRatio_MoreDeathsThanKills)
{
    TestPlayerScore score;
    score.kills = 3;
    score.deaths = 9;
    EXPECT_NEAR(score.GetKDRatio(), 0.333f, 0.01f);
}

TEST(PlayerScore_KDRatio_Equal)
{
    TestPlayerScore score;
    score.kills = 10;
    score.deaths = 10;
    EXPECT_NEAR(score.GetKDRatio(), 1.0f, 0.001f);
}
