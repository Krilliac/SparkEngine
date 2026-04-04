// TestAchievementSystem.cpp - Tests for Spark::Gameplay::AchievementSystem
#include "TestFramework.h"
#include "Engine/Gameplay/AchievementSystem.h"

// Helper to create a simple achievement definition
static Spark::Gameplay::AchievementDefinition MakeDef(uint32_t id, const std::string& name, float target = 1.0f,
                                                      uint32_t points = 10)
{
    Spark::Gameplay::AchievementDefinition def;
    def.id = id;
    def.name = name;
    def.description = name + " description";
    def.type = Spark::Gameplay::AchievementType::Progressive;
    def.targetValue = target;
    def.pointValue = points;
    return def;
}

// ============================================================================
// Registration
// ============================================================================

TEST(AchievementSystem_RegisterAchievement)
{
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAchievement(MakeDef(1, "First Blood"));
    EXPECT_FALSE(sys.IsUnlocked(1));
    sys.Shutdown();
}

// ============================================================================
// Progress tracking
// ============================================================================

TEST(AchievementSystem_UpdateProgress)
{
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAchievement(MakeDef(1, "Kill10", 10.0f));
    sys.UpdateProgress(1, 5.0f);
    const auto* prog = sys.GetProgress(1);
    EXPECT_TRUE(prog != nullptr);
    EXPECT_NEAR(prog->currentValue, 5.0f, 0.001f);
    EXPECT_FALSE(prog->unlocked);
    sys.Shutdown();
}

TEST(AchievementSystem_GetProgress_Nullptr_Unknown)
{
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.Initialize();
    EXPECT_TRUE(sys.GetProgress(999) == nullptr);
    sys.Shutdown();
}

TEST(AchievementSystem_IncrementProgress)
{
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAchievement(MakeDef(1, "Collect5", 5.0f));
    sys.IncrementProgress(1, 2.0f);
    sys.IncrementProgress(1, 1.0f);
    const auto* prog = sys.GetProgress(1);
    EXPECT_TRUE(prog != nullptr);
    EXPECT_NEAR(prog->currentValue, 3.0f, 0.001f);
    sys.Shutdown();
}

TEST(AchievementSystem_IncrementProgress_AutoUnlocks)
{
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAchievement(MakeDef(1, "Walk100", 100.0f));
    sys.IncrementProgress(1, 100.0f);
    EXPECT_TRUE(sys.IsUnlocked(1));
    sys.Shutdown();
}

// ============================================================================
// Unlock
// ============================================================================

TEST(AchievementSystem_UnlockAchievement)
{
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAchievement(MakeDef(1, "First Blood"));
    sys.UnlockAchievement(1);
    EXPECT_TRUE(sys.IsUnlocked(1));
    const auto* prog = sys.GetProgress(1);
    EXPECT_TRUE(prog != nullptr);
    EXPECT_TRUE(prog->unlocked);
    EXPECT_GT(prog->unlockTimestamp, 0u);
    sys.Shutdown();
}

TEST(AchievementSystem_IsUnlocked_False_Initially)
{
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAchievement(MakeDef(1, "First Blood"));
    EXPECT_FALSE(sys.IsUnlocked(1));
    sys.Shutdown();
}

// ============================================================================
// Counts and points
// ============================================================================

TEST(AchievementSystem_GetUnlockedCount)
{
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAchievement(MakeDef(1, "A", 1.0f, 10));
    sys.RegisterAchievement(MakeDef(2, "B", 1.0f, 20));
    sys.RegisterAchievement(MakeDef(3, "C", 1.0f, 30));
    sys.UnlockAchievement(1);
    sys.UnlockAchievement(3);
    EXPECT_EQ(sys.GetUnlockedCount(), 2u);
    sys.Shutdown();
}

TEST(AchievementSystem_GetTotalPoints)
{
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAchievement(MakeDef(1, "A", 1.0f, 10));
    sys.RegisterAchievement(MakeDef(2, "B", 1.0f, 20));
    EXPECT_EQ(sys.GetTotalPoints(), 30u);
    sys.Shutdown();
}

TEST(AchievementSystem_GetEarnedPoints)
{
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAchievement(MakeDef(1, "A", 1.0f, 10));
    sys.RegisterAchievement(MakeDef(2, "B", 1.0f, 20));
    sys.UnlockAchievement(2);
    EXPECT_EQ(sys.GetEarnedPoints(), 20u);
    sys.Shutdown();
}

// ============================================================================
// Reset
// ============================================================================

TEST(AchievementSystem_ResetProgress)
{
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAchievement(MakeDef(1, "A", 10.0f));
    sys.UpdateProgress(1, 5.0f);
    sys.ResetProgress(1);
    const auto* prog = sys.GetProgress(1);
    EXPECT_TRUE(prog != nullptr);
    EXPECT_NEAR(prog->currentValue, 0.0f, 0.001f);
    EXPECT_FALSE(prog->unlocked);
    sys.Shutdown();
}

TEST(AchievementSystem_ResetAll)
{
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAchievement(MakeDef(1, "A"));
    sys.RegisterAchievement(MakeDef(2, "B"));
    sys.UnlockAchievement(1);
    sys.UnlockAchievement(2);
    EXPECT_EQ(sys.GetUnlockedCount(), 2u);
    sys.ResetAll();
    EXPECT_EQ(sys.GetUnlockedCount(), 0u);
    sys.Shutdown();
}

// ============================================================================
// Callback
// ============================================================================

TEST(AchievementSystem_Callback_FiresOnUnlock)
{
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAchievement(MakeDef(1, "First Blood", 1.0f, 10));

    bool callbackFired = false;
    uint32_t capturedId = 0;
    std::string capturedName;
    sys.OnAchievementUnlocked(
        [&](const Spark::Gameplay::AchievementUnlockEvent& e)
        {
            callbackFired = true;
            capturedId = e.achievementId;
            capturedName = e.name;
        });

    sys.UnlockAchievement(1);
    EXPECT_TRUE(callbackFired);
    EXPECT_EQ(capturedId, 1u);
    EXPECT_TRUE(capturedName == "First Blood");
    sys.Shutdown();
}

TEST(AchievementSystem_Callback_NotFired_WhenAlreadyUnlocked)
{
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAchievement(MakeDef(1, "A"));
    sys.UnlockAchievement(1);

    int callCount = 0;
    sys.OnAchievementUnlocked([&](const Spark::Gameplay::AchievementUnlockEvent&) { ++callCount; });

    sys.UnlockAchievement(1); // Second unlock attempt
    EXPECT_EQ(callCount, 0);
    sys.Shutdown();
}
