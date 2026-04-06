// TestOnlineServices.cpp - Tests for Spark::OnlineServices
#include "TestFramework.h"
#include "Engine/OnlineServices/OnlineServices.h"

// ============================================================================
// Manager initialization
// ============================================================================

TEST(OnlineServices_Initialize)
{
    auto& mgr = Spark::OnlineServices::OnlineServiceManager::GetInstance();
    mgr.Initialize();
    EXPECT_TRUE(mgr.GetPlatform() != nullptr);
    auto status = mgr.Console_GetStatus();
    EXPECT_TRUE(status.find("Null") != std::string::npos);
    mgr.Shutdown();
}

// ============================================================================
// Null platform — Authentication
// ============================================================================

TEST(OnlineServices_NullLogin)
{
    auto& mgr = Spark::OnlineServices::OnlineServiceManager::GetInstance();
    mgr.Initialize();
    auto* platform = mgr.GetPlatform();
    EXPECT_TRUE(platform->Login("TestPlayer", ""));
    EXPECT_TRUE(platform->IsLoggedIn());
    auto player = platform->GetLocalPlayer();
    EXPECT_EQ(player.displayName, std::string("TestPlayer"));
    platform->Logout();
    EXPECT_FALSE(platform->IsLoggedIn());
    mgr.Shutdown();
}

// ============================================================================
// Null platform — Leaderboards
// ============================================================================

TEST(OnlineServices_NullLeaderboard)
{
    auto& mgr = Spark::OnlineServices::OnlineServiceManager::GetInstance();
    mgr.Initialize();
    auto* platform = mgr.GetPlatform();
    platform->Login("Player1", "");
    EXPECT_TRUE(platform->SubmitScore("HighScores", 100));
    EXPECT_TRUE(platform->SubmitScore("HighScores", 200));
    auto scores = platform->QueryScores("HighScores", 10);
    EXPECT_TRUE(scores.size() >= 2);
    EXPECT_TRUE(scores[0].score >= scores[1].score); // Sorted descending
    mgr.Shutdown();
}

// ============================================================================
// Null platform — Achievements
// ============================================================================

TEST(OnlineServices_NullAchievements)
{
    auto& mgr = Spark::OnlineServices::OnlineServiceManager::GetInstance();
    mgr.Initialize();
    auto* platform = mgr.GetPlatform();
    EXPECT_TRUE(platform->UnlockAchievement("first_blood"));
    EXPECT_TRUE(platform->SetAchievementProgress("collector", 0.5f));
    auto achievements = platform->QueryAchievements();
    EXPECT_EQ(achievements.size(), static_cast<size_t>(2));
    bool foundUnlocked = false;
    for (const auto& a : achievements)
    {
        if (a.id == "first_blood")
        {
            EXPECT_TRUE(a.unlocked);
            foundUnlocked = true;
        }
    }
    EXPECT_TRUE(foundUnlocked);
    mgr.Shutdown();
}

// ============================================================================
// Null platform — Cloud Saves
// ============================================================================

TEST(OnlineServices_NullCloudSaves)
{
    auto& mgr = Spark::OnlineServices::OnlineServiceManager::GetInstance();
    mgr.Initialize();
    auto* platform = mgr.GetPlatform();
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    EXPECT_TRUE(platform->SaveToCloud("save1", data));
    auto loaded = platform->LoadFromCloud("save1");
    EXPECT_EQ(loaded.size(), static_cast<size_t>(5));
    EXPECT_EQ(loaded[0], static_cast<uint8_t>(1));
    auto saves = platform->ListCloudSaves();
    EXPECT_EQ(saves.size(), static_cast<size_t>(1));
    EXPECT_TRUE(platform->DeleteCloudSave("save1"));
    saves = platform->ListCloudSaves();
    EXPECT_EQ(saves.size(), static_cast<size_t>(0));
    mgr.Shutdown();
}

// ============================================================================
// Null platform — Sessions
// ============================================================================

TEST(OnlineServices_NullSessions)
{
    auto& mgr = Spark::OnlineServices::OnlineServiceManager::GetInstance();
    mgr.Initialize();
    auto* platform = mgr.GetPlatform();

    Spark::OnlineServices::SessionInfo session;
    session.hostName = "Host";
    session.mapName = "Level1";
    session.maxPlayers = 8;
    EXPECT_TRUE(platform->CreateSession(session));

    auto sessions = platform->FindSessions();
    EXPECT_TRUE(sessions.size() >= 1);

    auto current = platform->GetCurrentSession();
    EXPECT_EQ(current.mapName, std::string("Level1"));
    mgr.Shutdown();
}

// ============================================================================
// Platform stubs
// ============================================================================

TEST(OnlineServices_SteamStub)
{
    Spark::OnlineServices::SteamPlatform steam;
    EXPECT_EQ(steam.GetPlatformName(), std::string("Steam (Stub)"));
    EXPECT_FALSE(steam.Login("test", ""));
    EXPECT_FALSE(steam.IsLoggedIn());
}

TEST(OnlineServices_EpicStub)
{
    Spark::OnlineServices::EpicPlatform epic;
    EXPECT_EQ(epic.GetPlatformName(), std::string("Epic (Stub)"));
    EXPECT_FALSE(epic.Login("test", ""));
}

TEST(OnlineServices_ConsoleStub)
{
    Spark::OnlineServices::ConsolePlatform console;
    EXPECT_EQ(console.GetPlatformName(), std::string("Console (Stub)"));
    EXPECT_FALSE(console.Login("test", ""));
}

TEST(OnlineServices_SetPlatform)
{
    auto& mgr = Spark::OnlineServices::OnlineServiceManager::GetInstance();
    mgr.Initialize();

    mgr.SetPlatform(std::make_unique<Spark::OnlineServices::SteamPlatform>());
    EXPECT_EQ(mgr.GetPlatform()->GetPlatformName(), std::string("Steam (Stub)"));

    mgr.ResetToNullPlatform();
    EXPECT_TRUE(mgr.GetPlatform()->GetPlatformName().find("Null") != std::string::npos);
    mgr.Shutdown();
}
