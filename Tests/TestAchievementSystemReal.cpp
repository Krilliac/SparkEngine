/**
 * @file TestAchievementSystemReal.cpp
 * @brief Real-class tests for Spark::Gameplay::AchievementSystem
 */

#include "TestFramework.h"
#include "Engine/Gameplay/AchievementSystem.h"
#include "Utils/Serializer.h"

#include <cmath>
#include <limits>
#include <vector>

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

// ---------------------------------------------------------------------------
// LoadFromReader takes save-file bytes (SEC-120 achievement-definitions target)
// ---------------------------------------------------------------------------

namespace
{
    std::vector<uint8_t> ProgressRecord(uint32_t id, float value, uint8_t unlocked)
    {
        Spark::BinaryWriter writer;
        writer.Write<uint32_t>(1);
        writer.Write<uint32_t>(id);
        writer.Write<float>(value);
        writer.Write<uint8_t>(unlocked);
        writer.Write<uint64_t>(0);
        return writer.GetBuffer();
    }
} // namespace

TEST(AchievementSystemReal_LoadSkipsNonFiniteProgress)
{
    ResetAchievements();
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.RegisterAchievement(MakeDef(7, "Three Kills", 3.0f));

    const auto bytes = ProgressRecord(7, std::numeric_limits<float>::quiet_NaN(), 0);
    Spark::BinaryReader reader(bytes);
    sys.LoadFromReader(reader);

    const auto* prog = sys.GetProgress(7);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(std::isfinite(prog->currentValue));
    // NaN progress never compares >= the target, so these increments could not unlock it.
    sys.IncrementProgress(7);
    sys.IncrementProgress(7);
    sys.IncrementProgress(7);
    EXPECT_TRUE(sys.IsUnlocked(7));
}

TEST(AchievementSystemReal_LoadClampsProgressIntoTarget)
{
    ResetAchievements();
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.RegisterAchievement(MakeDef(8, "Collector", 5.0f));
    sys.RegisterAchievement(MakeDef(9, "Wanderer", 5.0f));

    const auto over = ProgressRecord(8, 1.0e9f, 0);
    Spark::BinaryReader overReader(over);
    sys.LoadFromReader(overReader);
    const auto under = ProgressRecord(9, -4.0f, 0);
    Spark::BinaryReader underReader(under);
    sys.LoadFromReader(underReader);

    ASSERT_TRUE(sys.GetProgress(8) != nullptr);
    ASSERT_TRUE(sys.GetProgress(9) != nullptr);
    EXPECT_NEAR(sys.GetProgress(8)->currentValue, 5.0f, 0.0001f);
    EXPECT_NEAR(sys.GetProgress(9)->currentValue, 0.0f, 0.0001f);
}

TEST(AchievementSystemReal_TruncatedCountReturnsImmediately)
{
    ResetAchievements();
    auto& sys = Spark::Gameplay::AchievementSystem::GetInstance();
    sys.RegisterAchievement(MakeDef(10, "Count Guard", 5.0f));

    Spark::BinaryWriter writer;
    writer.Write<uint32_t>(0xFFFFFFFFu);
    Spark::BinaryReader reader(writer.GetBuffer());
    sys.LoadFromReader(reader);

    ASSERT_TRUE(sys.GetProgress(10) != nullptr);
    EXPECT_NEAR(sys.GetProgress(10)->currentValue, 0.0f, 0.0001f);
    EXPECT_FALSE(sys.IsUnlocked(10));
}
