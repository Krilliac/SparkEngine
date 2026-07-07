// Test_gameplay_achievement.cpp — regression tests for AchievementSystem save/load.
//
// SaveToWriter / LoadFromReader were declared but never defined (a latent link-time
// landmine and a broken advertised feature). These tests exercise the newly-implemented
// binary round-trip and the "only apply to already-registered ids" contract.

#include "../TestFramework.h"
#include "Engine/Gameplay/AchievementSystem.h"

using namespace Spark::Gameplay;

TEST(Achievement_SaveLoad_RoundTrip)
{
    auto& ach = AchievementSystem::GetInstance();
    ach.Initialize();

    AchievementDefinition d1;
    d1.id = 10;
    d1.name = "A";
    d1.targetValue = 5.0f;
    AchievementDefinition d2;
    d2.id = 20;
    d2.name = "B";
    d2.targetValue = 3.0f;
    ach.RegisterAchievement(d1);
    ach.RegisterAchievement(d2);

    ach.IncrementProgress(10, 2.0f); // partial progress
    ach.UnlockAchievement(20);       // fully unlocked

    Spark::BinaryWriter writer;
    ach.SaveToWriter(writer);

    // Wipe runtime progress, confirm it is gone, then reload from the saved buffer.
    ach.ResetAll();
    EXPECT_FALSE(ach.IsUnlocked(20));

    Spark::BinaryReader reader(writer.GetBuffer());
    ach.LoadFromReader(reader);
    EXPECT_FALSE(reader.HasError());

    EXPECT_TRUE(ach.IsUnlocked(20));
    EXPECT_FALSE(ach.IsUnlocked(10));

    const AchievementProgress* p10 = ach.GetProgress(10);
    ASSERT_TRUE(p10 != nullptr);
    EXPECT_NEAR(p10->currentValue, 2.0f, 0.001f);

    const AchievementProgress* p20 = ach.GetProgress(20);
    ASSERT_TRUE(p20 != nullptr);
    EXPECT_TRUE(p20->unlockTimestamp != 0);
    // Loading must NOT re-fire unlock notifications for already-earned achievements.
    EXPECT_TRUE(p20->notified);
}

TEST(Achievement_Load_SkipsUnregisteredIds)
{
    auto& ach = AchievementSystem::GetInstance();
    ach.Initialize();

    AchievementDefinition d;
    d.id = 1;
    d.targetValue = 1.0f;
    ach.RegisterAchievement(d);
    ach.UnlockAchievement(1);

    Spark::BinaryWriter writer;
    ach.SaveToWriter(writer); // buffer contains id=1

    // Fresh system that does NOT register id=1 — the record must be skipped, not inserted.
    ach.Initialize();
    Spark::BinaryReader reader(writer.GetBuffer());
    ach.LoadFromReader(reader);

    EXPECT_FALSE(reader.HasError());
    EXPECT_TRUE(ach.GetProgress(1) == nullptr);
}
