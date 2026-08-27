/**
 * @file TestAchievementSystemReal.cpp
 * @brief Real-class tests for Spark::Gameplay::AchievementSystem
 */

#include "TestFramework.h"
#include "Engine/Gameplay/AchievementSystem.h"

namespace
{
    void ResetAchievements()
    {
        auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
        sys.Shutdown();
        sys.Initialize();
    }

    Spark::Gameplay::AchievementDefinition MakeDef(uint32_t id, const std::string& name, float target = 1.0f,
                                                   uint32_t points = 10)
    {
        Spark::Gameplay::AchievementDefinition def;
        def.id = id;
        def.name = name;
        def.description = "Test achievement";
        def.targetValue = target;
        def.pointValue = points;
        return def;
    }
} // namespace

TEST(AchievementSystemReal_SingletonStable)
{
    auto& a = Spark::Gameplay::AchievementSystem::GetInstance();
    auto& b = Spark::Gameplay::AchievementSystem::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(AchievementSystemReal_RegisterAndQuery)
{
    ResetAchievements();
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.RegisterAchievement(MakeDef(1, "First Blood"));
    EXPECT_FALSE(sys.IsUnlocked(1));

    auto* prog = sys.GetProgress(1);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_NEAR(prog->currentValue, 0.0f, 0.01f);
}

TEST(AchievementSystemReal_UnlockAchievement)
{
    ResetAchievements();
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.RegisterAchievement(MakeDef(1, "First Blood", 1.0f, 10));
    sys.UnlockAchievement(1);

    EXPECT_TRUE(sys.IsUnlocked(1));
    EXPECT_EQ(sys.GetUnlockedCount(), uint32_t(1));
    EXPECT_EQ(sys.GetEarnedPoints(), uint32_t(10));
}

TEST(AchievementSystemReal_ProgressiveUnlock)
{
    ResetAchievements();
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.RegisterAchievement(MakeDef(2, "Collector", 10.0f, 25));

    sys.IncrementProgress(2, 5.0f);
    EXPECT_FALSE(sys.IsUnlocked(2));
    EXPECT_NEAR(sys.GetProgress(2)->currentValue, 5.0f, 0.01f);

    sys.IncrementProgress(2, 5.0f);
    EXPECT_TRUE(sys.IsUnlocked(2));
}

TEST(AchievementSystemReal_DoubleUnlockIgnored)
{
    ResetAchievements();
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.RegisterAchievement(MakeDef(1, "Test"));
    sys.UnlockAchievement(1);
    sys.UnlockAchievement(1); // should not crash or change state
    EXPECT_EQ(sys.GetUnlockedCount(), uint32_t(1));
}

TEST(AchievementSystemReal_ResetProgress)
{
    ResetAchievements();
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.RegisterAchievement(MakeDef(1, "Test"));
    sys.UnlockAchievement(1);
    sys.ResetProgress(1);
    EXPECT_FALSE(sys.IsUnlocked(1));
}

TEST(AchievementSystemReal_ResetAll)
{
    ResetAchievements();
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.RegisterAchievement(MakeDef(1, "A", 1.0f, 10));
    sys.RegisterAchievement(MakeDef(2, "B", 1.0f, 20));
    sys.UnlockAchievement(1);
    sys.UnlockAchievement(2);
    EXPECT_EQ(sys.GetUnlockedCount(), uint32_t(2));

    sys.ResetAll();
    EXPECT_EQ(sys.GetUnlockedCount(), uint32_t(0));
    EXPECT_EQ(sys.GetEarnedPoints(), uint32_t(0));
}

TEST(AchievementSystemReal_CallbackFired)
{
    ResetAchievements();
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.RegisterAchievement(MakeDef(1, "Triggered", 1.0f, 5));

    bool fired = false;
    sys.OnAchievementUnlocked(
        [&](const Spark::Gameplay::AchievementUnlockEvent& e)
        {
            fired = true;
            EXPECT_EQ(e.achievementId, uint32_t(1));
            EXPECT_EQ(e.pointValue, uint32_t(5));
        });

    sys.UnlockAchievement(1);
    EXPECT_TRUE(fired);
}

TEST(AchievementSystemReal_TotalPoints)
{
    ResetAchievements();
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.RegisterAchievement(MakeDef(1, "A", 1.0f, 10));
    sys.RegisterAchievement(MakeDef(2, "B", 1.0f, 20));
    EXPECT_EQ(sys.GetTotalPoints(), uint32_t(30));
}

TEST(AchievementSystemReal_ConsoleStatus)
{
    ResetAchievements();
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.RegisterAchievement(MakeDef(1, "A"));
    std::string status = sys.Console_GetStatus();
    EXPECT_STR_CONTAINS(status, "AchievementSystem");
}
