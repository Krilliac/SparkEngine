// TestAnimNotify.cpp - Tests for Spark::Animation::AnimNotifyManager
#include "TestFramework.h"
#include "Engine/Animation/AnimNotify.h"

// ============================================================================
// Initialization
// ============================================================================

TEST(AnimNotify_Initialize)
{
    auto& mgr = Spark::Animation::AnimNotifyManager::GetInstance();
    mgr.Initialize();
    EXPECT_EQ(mgr.GetTrackCount(), static_cast<size_t>(0));
    mgr.Shutdown();
}

// ============================================================================
// Add and retrieve notifies
// ============================================================================

TEST(AnimNotify_AddNotify)
{
    auto& mgr = Spark::Animation::AnimNotifyManager::GetInstance();
    mgr.Initialize();

    Spark::Animation::AnimNotifyEvent event;
    event.triggerTime = 0.3f;
    event.type = Spark::Animation::AnimNotifyType::Sound;
    event.name = "Footstep";
    event.payload = "footstep_concrete";
    mgr.AddNotify("Walk", event);

    EXPECT_EQ(mgr.GetTrackCount(), static_cast<size_t>(1));
    const auto* track = mgr.GetTrack("Walk");
    EXPECT_TRUE(track != nullptr);
    EXPECT_EQ(track->events.size(), static_cast<size_t>(1));
    EXPECT_NEAR(track->events[0].triggerTime, 0.3f, 0.001f);
    mgr.Shutdown();
}

TEST(AnimNotify_GetTrack_Null_ForUnknown)
{
    auto& mgr = Spark::Animation::AnimNotifyManager::GetInstance();
    mgr.Initialize();
    EXPECT_TRUE(mgr.GetTrack("NonExistent") == nullptr);
    mgr.Shutdown();
}

TEST(AnimNotify_ClearNotifies)
{
    auto& mgr = Spark::Animation::AnimNotifyManager::GetInstance();
    mgr.Initialize();

    Spark::Animation::AnimNotifyEvent event;
    event.triggerTime = 0.5f;
    event.type = Spark::Animation::AnimNotifyType::Particle;
    mgr.AddNotify("Attack", event);
    EXPECT_EQ(mgr.GetTrackCount(), static_cast<size_t>(1));

    mgr.ClearNotifies("Attack");
    EXPECT_EQ(mgr.GetTrackCount(), static_cast<size_t>(0));
    mgr.Shutdown();
}

// ============================================================================
// Evaluation
// ============================================================================

TEST(AnimNotify_Evaluate_FiresInRange)
{
    auto& mgr = Spark::Animation::AnimNotifyManager::GetInstance();
    mgr.Initialize();

    Spark::Animation::AnimNotifyEvent event;
    event.triggerTime = 0.5f;
    event.type = Spark::Animation::AnimNotifyType::Sound;
    event.name = "Hit";
    mgr.AddNotify("Attack", event);

    int firedCount = 0;
    mgr.OnNotifyTriggered(
        [&firedCount](const Spark::Animation::AnimNotifyTrigger& trigger)
        {
            EXPECT_EQ(trigger.name, std::string("Hit"));
            ++firedCount;
        });

    // Should fire: 0.5 is in (0.0, 1.0]
    int result = mgr.Evaluate("Attack", 1, 0.0f, 1.0f);
    EXPECT_EQ(result, 1);
    EXPECT_EQ(firedCount, 1);
    mgr.Shutdown();
}

TEST(AnimNotify_Evaluate_DoesNotFireOutOfRange)
{
    auto& mgr = Spark::Animation::AnimNotifyManager::GetInstance();
    mgr.Initialize();

    Spark::Animation::AnimNotifyEvent event;
    event.triggerTime = 0.5f;
    event.type = Spark::Animation::AnimNotifyType::Sound;
    mgr.AddNotify("Walk", event);

    // Should NOT fire: 0.5 is not in (0.6, 0.9]
    int result = mgr.Evaluate("Walk", 1, 0.6f, 0.9f);
    EXPECT_EQ(result, 0);
    mgr.Shutdown();
}

TEST(AnimNotify_Evaluate_LoopWrap)
{
    auto& mgr = Spark::Animation::AnimNotifyManager::GetInstance();
    mgr.Initialize();

    Spark::Animation::AnimNotifyEvent event;
    event.triggerTime = 0.1f;
    event.type = Spark::Animation::AnimNotifyType::FootPlant;
    event.name = "LeftFoot";
    mgr.AddNotify("Run", event);

    int firedCount = 0;
    mgr.OnNotifyTriggered([&firedCount](const Spark::Animation::AnimNotifyTrigger&) { ++firedCount; });

    // Loop wrap: previousTime=0.9, currentTime=0.2, duration=1.0
    int result = mgr.Evaluate("Run", 1, 0.9f, 0.2f, 1.0f);
    EXPECT_EQ(result, 1);
    EXPECT_EQ(firedCount, 1);
    mgr.Shutdown();
}

TEST(AnimNotify_Evaluate_UnknownClip_ReturnsZero)
{
    auto& mgr = Spark::Animation::AnimNotifyManager::GetInstance();
    mgr.Initialize();
    int result = mgr.Evaluate("UnknownClip", 1, 0.0f, 1.0f);
    EXPECT_EQ(result, 0);
    mgr.Shutdown();
}

TEST(AnimNotify_MultipleNotifies_SortedByTime)
{
    auto& mgr = Spark::Animation::AnimNotifyManager::GetInstance();
    mgr.Initialize();

    // Add out of order
    Spark::Animation::AnimNotifyEvent e1;
    e1.triggerTime = 0.8f;
    e1.name = "Second";
    mgr.AddNotify("Combo", e1);

    Spark::Animation::AnimNotifyEvent e2;
    e2.triggerTime = 0.2f;
    e2.name = "First";
    mgr.AddNotify("Combo", e2);

    const auto* track = mgr.GetTrack("Combo");
    EXPECT_TRUE(track != nullptr);
    EXPECT_EQ(track->events.size(), static_cast<size_t>(2));
    // Should be sorted by time
    EXPECT_TRUE(track->events[0].triggerTime <= track->events[1].triggerTime);
    mgr.Shutdown();
}

TEST(AnimNotify_ConsoleStatus)
{
    auto& mgr = Spark::Animation::AnimNotifyManager::GetInstance();
    mgr.Initialize();

    Spark::Animation::AnimNotifyEvent event;
    event.triggerTime = 0.5f;
    mgr.AddNotify("Test", event);

    std::string status = mgr.Console_GetStatus();
    EXPECT_TRUE(status.find("1 tracks") != std::string::npos);
    mgr.Shutdown();
}
