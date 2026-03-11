/**
 * @file TestAchievementSystem.cpp
 * @brief Tests for Spark::AchievementSystem
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Engine/Stats/AchievementSystem.h"

// ============================================================================
// Achievement Tests
// ============================================================================

TEST(Achievement_DefineAndGet)
{
    auto& sys = Spark::AchievementSystem::Get();
    sys.ResetAll();

    sys.DefineAchievement("test_ach", "Test Achievement", "A test achievement");
    const auto* ach = sys.GetAchievement("test_ach");
    EXPECT_TRUE(ach != nullptr);
    EXPECT_EQ(ach->name, std::string("Test Achievement"));
    EXPECT_FALSE(ach->unlocked);
}

TEST(Achievement_ManualUnlock)
{
    auto& sys = Spark::AchievementSystem::Get();
    sys.ResetAll();

    sys.DefineAchievement("manual_ach", "Manual", "Manually unlocked");
    EXPECT_TRUE(sys.UnlockAchievement("manual_ach"));
    EXPECT_FALSE(sys.UnlockAchievement("manual_ach")); // Already unlocked

    const auto* ach = sys.GetAchievement("manual_ach");
    EXPECT_TRUE(ach->unlocked);
    EXPECT_NEAR(ach->progress, 1.0f, 0.001f);
}

TEST(Achievement_StatCondition)
{
    auto& sys = Spark::AchievementSystem::Get();
    sys.ResetAll();

    sys.DefineAchievement("kill_ach", "First Blood", "Get first kill");
    sys.DefineStatistic("kills", Spark::StatType::Integer);
    sys.SetCondition("kill_ach", "kills", 5);

    sys.IncrementStat("kills", 3);
    const auto* ach = sys.GetAchievement("kill_ach");
    EXPECT_FALSE(ach->unlocked);
    EXPECT_NEAR(ach->progress, 0.6f, 0.001f);

    sys.IncrementStat("kills", 2);
    ach = sys.GetAchievement("kill_ach");
    EXPECT_TRUE(ach->unlocked);
}

TEST(Achievement_StatFloat)
{
    auto& sys = Spark::AchievementSystem::Get();
    sys.ResetAll();

    sys.DefineStatistic("play_time", Spark::StatType::Float);
    sys.AddStatFloat("play_time", 10.5);
    sys.AddStatFloat("play_time", 5.3);
    EXPECT_NEAR(sys.GetStatFloat("play_time"), 15.8, 0.001);
}

TEST(Achievement_UnlockedCount)
{
    auto& sys = Spark::AchievementSystem::Get();
    sys.ResetAll();

    sys.DefineAchievement("a1", "A1", "desc1");
    sys.DefineAchievement("a2", "A2", "desc2");
    sys.DefineAchievement("a3", "A3", "desc3");

    EXPECT_EQ(sys.GetUnlockedCount(), static_cast<size_t>(0));
    EXPECT_EQ(sys.GetTotalCount(), static_cast<size_t>(3));

    sys.UnlockAchievement("a1");
    sys.UnlockAchievement("a3");
    EXPECT_EQ(sys.GetUnlockedCount(), static_cast<size_t>(2));
}
