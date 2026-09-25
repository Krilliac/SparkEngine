// TestOnlineServices.cpp - Tests for Spark::OnlineServices
#include "TestFramework.h"
#include "Engine/OnlineServices/OnlineServices.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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
    EXPECT_TRUE(steam.GetLastError().find("Steamworks SDK unavailable") != std::string::npos);
    const auto caps = steam.GetCapabilities();
    EXPECT_FALSE(caps.authentication);
    EXPECT_FALSE(caps.sessions);
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
    const auto caps = mgr.GetPlatform()->GetCapabilities();
    EXPECT_TRUE(caps.authentication);
    EXPECT_TRUE(caps.sessions);
    mgr.Shutdown();
}

// ============================================================================
// OnlineServices_Contract — IOnlinePlatform conformance (docs/specs/online-services.md
// section 5.1 and the section 6 adapter register). Every adapter shipped in
// OnlineServices.h runs the same suite: a capability reported as true must work,
// clear GetLastError() on success, and read back where the interface has a getter
// (friends and presence have none on the Null adapter); a capability reported as
// false must fail every call with a non-empty GetLastError() and fabricate nothing;
// and no failure may echo the login token. Registered under the online-services ctest label with an exact
// count so a dropped adapter test fails the label instead of shrinking it.
// ============================================================================

namespace
{
    using Spark::OnlineServices::IOnlinePlatform;
    using Spark::OnlineServices::PlatformCapabilities;

    constexpr const char* kContractToken = "contract-secret-token-7f3a";

    bool HasAnyCapability(const PlatformCapabilities& caps)
    {
        return caps.authentication || caps.sessions || caps.leaderboards || caps.achievements || caps.cloudSave ||
               caps.friends || caps.presence;
    }

    bool SameCapabilities(const PlatformCapabilities& a, const PlatformCapabilities& b)
    {
        return a.authentication == b.authentication && a.sessions == b.sessions && a.leaderboards == b.leaderboards &&
               a.achievements == b.achievements && a.cloudSave == b.cloudSave && a.friends == b.friends &&
               a.presence == b.presence;
    }

    // A failed call must leave a human-readable, secret-free reason behind. When the
    // adapter is capable (today only the Null adapter), expectedError is the exact reason
    // that call must set. The Null adapter clears GetLastError() on entry to every fallible
    // call and each failing call has its own message, so a stale error left by an earlier
    // call cannot satisfy the check. The capability-less stubs return one constant string
    // from GetLastError(), so for them (expectedError empty) only its presence is checked.
    void ExpectFailureReported(const IOnlinePlatform& platform, const bool callSucceeded, const char* call,
                               const std::string& expectedError = {})
    {
        const std::string error = platform.GetLastError();
        const bool wrongReason = !expectedError.empty() && error != expectedError;
        if (callSucceeded || error.empty() || wrongReason || error.find(kContractToken) != std::string::npos)
        {
            std::cerr << "  contract violation in " << platform.GetPlatformName() << "::" << call
                      << " (succeeded=" << callSucceeded << ", lastError='" << error << "', expected='" << expectedError
                      << "')\n";
        }
        EXPECT_FALSE(callSucceeded);
        EXPECT_FALSE(error.empty());
        EXPECT_FALSE(wrongReason);
        EXPECT_TRUE(error.find(kContractToken) == std::string::npos);
    }

    // A successful call on a capable adapter must succeed and leave no failure reason behind.
    void ExpectSucceeded(const IOnlinePlatform& platform, const bool callSucceeded, const char* call)
    {
        const std::string error = platform.GetLastError();
        if (!callSucceeded || !error.empty())
        {
            std::cerr << "  contract violation in " << platform.GetPlatformName() << "::" << call
                      << " (succeeded=" << callSucceeded << ", lastError='" << error << "')\n";
        }
        EXPECT_TRUE(callSucceeded);
        EXPECT_TRUE(error.empty());
    }

    void CheckAuthentication(IOnlinePlatform& platform, const bool capable)
    {
        const bool loggedIn = platform.Login("ContractPlayer", kContractToken);
        const auto player = platform.GetLocalPlayer();
        if (capable)
        {
            ExpectSucceeded(platform, loggedIn, "Login");
            EXPECT_TRUE(platform.IsLoggedIn());
            EXPECT_FALSE(player.playerId.empty());
            EXPECT_EQ(player.displayName, std::string("ContractPlayer"));
            return;
        }
        ExpectFailureReported(platform, loggedIn, "Login");
        EXPECT_FALSE(platform.IsLoggedIn());
        EXPECT_TRUE(player.playerId.empty());
        EXPECT_TRUE(player.displayName.empty());
        EXPECT_FALSE(player.isOnline);
    }

    void CheckSessions(IOnlinePlatform& platform, const bool capable)
    {
        Spark::OnlineServices::SessionInfo settings;
        settings.hostName = "ContractHost";
        settings.mapName = "ContractMap";
        settings.gameMode = "Contract";
        settings.maxPlayers = 4;

        const bool created = platform.CreateSession(settings);
        if (!capable)
        {
            ExpectFailureReported(platform, created, "CreateSession");
            EXPECT_TRUE(platform.GetCurrentSession().sessionId.empty());
            EXPECT_TRUE(platform.FindSessions("").empty());
            ExpectFailureReported(platform, platform.JoinSession("contract-session"), "JoinSession");
            EXPECT_TRUE(platform.GetCurrentSession().sessionId.empty());
            platform.LeaveSession();
            EXPECT_TRUE(platform.GetCurrentSession().sessionId.empty());
            return;
        }

        ExpectSucceeded(platform, created, "CreateSession");
        const auto current = platform.GetCurrentSession();
        EXPECT_FALSE(current.sessionId.empty());
        EXPECT_EQ(current.mapName, std::string("ContractMap"));

        const auto found = platform.FindSessions("");
        const bool listed = std::any_of(found.begin(), found.end(),
                                        [&](const auto& session) { return session.sessionId == current.sessionId; });
        EXPECT_TRUE(listed);

        ExpectFailureReported(platform, platform.JoinSession("contract-missing-session"), "JoinSession",
                              "Session not found: contract-missing-session");
        ExpectSucceeded(platform, platform.JoinSession(current.sessionId), "JoinSession");
        EXPECT_EQ(platform.GetCurrentSession().sessionId, current.sessionId);

        platform.LeaveSession();
        EXPECT_TRUE(platform.GetCurrentSession().sessionId.empty());
        platform.LeaveSession();
        EXPECT_TRUE(platform.GetCurrentSession().sessionId.empty());
    }

    void CheckLeaderboards(IOnlinePlatform& platform, const bool capable)
    {
        const bool submitted = platform.SubmitScore("ContractBoard", 42);
        if (!capable)
        {
            ExpectFailureReported(platform, submitted, "SubmitScore");
            EXPECT_TRUE(platform.QueryScores("ContractBoard", 10).empty());
            return;
        }
        ExpectSucceeded(platform, submitted, "SubmitScore");
        const auto scores = platform.QueryScores("ContractBoard", 10);
        ASSERT_EQ(scores.size(), static_cast<size_t>(1));
        EXPECT_EQ(scores[0].score, static_cast<int64_t>(42));
        EXPECT_EQ(scores[0].rank, static_cast<uint32_t>(1));
        EXPECT_TRUE(platform.QueryScores("ContractBoardNeverWritten", 10).empty());
    }

    void CheckAchievements(IOnlinePlatform& platform, const bool capable)
    {
        const bool unlocked = platform.UnlockAchievement("contract_unlock");
        if (!capable)
        {
            ExpectFailureReported(platform, unlocked, "UnlockAchievement");
            ExpectFailureReported(platform, platform.SetAchievementProgress("contract_progress", 0.5f),
                                  "SetAchievementProgress");
            EXPECT_TRUE(platform.QueryAchievements().empty());
            return;
        }
        ExpectSucceeded(platform, unlocked, "UnlockAchievement");
        ExpectSucceeded(platform, platform.SetAchievementProgress("contract_progress", 0.5f), "SetAchievementProgress");
        const auto achievements = platform.QueryAchievements();
        EXPECT_EQ(achievements.size(), static_cast<size_t>(2));
        for (const auto& achievement : achievements)
        {
            if (achievement.id == "contract_unlock")
            {
                EXPECT_TRUE(achievement.unlocked);
            }
            else
            {
                EXPECT_EQ(achievement.id, std::string("contract_progress"));
                EXPECT_NEAR(achievement.progress, 0.5f, 1e-6f);
                EXPECT_FALSE(achievement.unlocked);
            }
        }
    }

    void CheckCloudSave(IOnlinePlatform& platform, const bool capable)
    {
        const std::vector<uint8_t> payload = {1, 2, 3};
        const bool saved = platform.SaveToCloud("contract_slot", payload);
        if (!capable)
        {
            ExpectFailureReported(platform, saved, "SaveToCloud");
            const auto loaded = platform.LoadFromCloud("contract_slot");
            ExpectFailureReported(platform, !loaded.empty(), "LoadFromCloud");
            ExpectFailureReported(platform, platform.DeleteCloudSave("contract_slot"), "DeleteCloudSave");
            EXPECT_TRUE(platform.ListCloudSaves().empty());
            return;
        }
        ExpectSucceeded(platform, saved, "SaveToCloud");
        const auto loaded = platform.LoadFromCloud("contract_slot");
        ExpectSucceeded(platform, loaded == payload, "LoadFromCloud");
        const auto slots = platform.ListCloudSaves();
        ASSERT_EQ(slots.size(), static_cast<size_t>(1));
        EXPECT_EQ(slots[0].slotName, std::string("contract_slot"));
        EXPECT_EQ(slots[0].sizeBytes, static_cast<uint64_t>(payload.size()));

        ExpectSucceeded(platform, platform.DeleteCloudSave("contract_slot"), "DeleteCloudSave");
        const auto afterDelete = platform.LoadFromCloud("contract_slot");
        ExpectFailureReported(platform, !afterDelete.empty(), "LoadFromCloud", "Cloud slot not found: contract_slot");
        ExpectFailureReported(platform, platform.DeleteCloudSave("contract_slot"), "DeleteCloudSave",
                              "Cannot delete missing cloud slot: contract_slot");
        EXPECT_TRUE(platform.ListCloudSaves().empty());
    }

    // Friends and presence have no read-back through IOnlinePlatform on the Null adapter:
    // offline mode has an empty friends list and SetPresence has no getter. So a capable
    // adapter is checked for SetPresence succeeding, and invites are checked to succeed only
    // for a recipient the adapter itself lists as a friend.
    void CheckSocial(IOnlinePlatform& platform, const PlatformCapabilities& caps)
    {
        const auto friends = platform.GetFriendsList();
        if (!caps.friends)
        {
            EXPECT_TRUE(friends.empty());
        }

        const bool presenceSet = platform.SetPresence("In contract");
        if (caps.presence)
        {
            ExpectSucceeded(platform, presenceSet, "SetPresence");
        }
        else
        {
            ExpectFailureReported(platform, presenceSet, "SetPresence");
        }

        // No session is active here (CheckSessions left it), so an invite has nothing to
        // invite into and must fail on every adapter, capable or not.
        const bool capable = caps.friends && caps.sessions;
        EXPECT_TRUE(platform.GetCurrentSession().sessionId.empty());
        ExpectFailureReported(platform, platform.InviteToSession("contract_friend"), "InviteToSession",
                              capable ? "Invite requires an active session" : "");
        if (!capable)
        {
            return;
        }

        Spark::OnlineServices::SessionInfo settings;
        settings.mapName = "ContractInviteMap";
        ExpectSucceeded(platform, platform.CreateSession(settings), "CreateSession");
        ExpectFailureReported(platform, platform.InviteToSession(""), "InviteToSession", "Invite requires a friend ID");
        ExpectFailureReported(platform, platform.InviteToSession("contract_stranger"), "InviteToSession",
                              "Invite recipient is not a friend: contract_stranger");
        if (!friends.empty())
        {
            ExpectSucceeded(platform, platform.InviteToSession(friends.front().playerId), "InviteToSession");
        }
        platform.LeaveSession();
    }

    void CheckManagerObservability(std::unique_ptr<IOnlinePlatform> platform, const bool capable)
    {
        const std::string name = platform->GetPlatformName();
        auto& manager = Spark::OnlineServices::OnlineServiceManager::GetInstance();
        manager.Initialize();
        manager.SetPlatform(std::move(platform));
        const std::string status = manager.Console_GetStatus();
        EXPECT_STR_CONTAINS(status, name);
        EXPECT_STR_CONTAINS(status, std::string(capable ? "Capabilities: active" : "Capabilities: none"));
        if (!capable)
        {
            EXPECT_STR_CONTAINS(status, std::string("LastError: "));
        }
        EXPECT_TRUE(status.find(kContractToken) == std::string::npos);
        manager.Shutdown();
    }

    // Runs the full IOnlinePlatform contract against one fresh adapter instance.
    template <typename Adapter> void RunOnlinePlatformContract()
    {
        Adapter platform;
        const std::string name = platform.GetPlatformName();
        EXPECT_FALSE(name.empty());

        const PlatformCapabilities caps = platform.GetCapabilities();
        EXPECT_TRUE(SameCapabilities(caps, platform.GetCapabilities()));

        // Adapter register (spec section 6): nothing in the tree is production. An adapter
        // with no capability must say it is a stub; the only capable adapter is the local one.
        EXPECT_TRUE(name.find("roduction") == std::string::npos);
        if (HasAnyCapability(caps))
        {
            EXPECT_STR_CONTAINS(name, std::string("Offline"));
        }
        else
        {
            EXPECT_STR_CONTAINS(name, std::string("(Stub)"));
        }

        // Logout and LeaveSession are safe in any state, including before login.
        platform.Logout();
        platform.LeaveSession();
        EXPECT_FALSE(platform.IsLoggedIn());
        EXPECT_TRUE(platform.GetCurrentSession().sessionId.empty());

        CheckAuthentication(platform, caps.authentication);
        CheckSessions(platform, caps.sessions);
        CheckLeaderboards(platform, caps.leaderboards);
        CheckAchievements(platform, caps.achievements);
        CheckCloudSave(platform, caps.cloudSave);
        CheckSocial(platform, caps);

        platform.Logout();
        EXPECT_FALSE(platform.IsLoggedIn());
        platform.Logout();
        EXPECT_FALSE(platform.IsLoggedIn());
        EXPECT_TRUE(platform.GetLastError().find(kContractToken) == std::string::npos);

        CheckManagerObservability(std::make_unique<Adapter>(), HasAnyCapability(caps));
    }

    // Drives a fixed script and records every observable result, so two fresh runs can be compared.
    std::string RecordNullPlatformTranscript()
    {
        Spark::OnlineServices::NullOnlinePlatform platform;
        std::string transcript;
        const auto record = [&](const std::string& line) { transcript += line + "\n"; };

        record("login=" + std::to_string(platform.Login("Alpha", kContractToken)));
        record("player=" + platform.GetLocalPlayer().playerId);

        Spark::OnlineServices::SessionInfo settings;
        settings.mapName = "DeterminismMap";
        for (int i = 0; i < 3; ++i)
        {
            // Each call is sequenced before its read-back: operand order in one expression is unspecified.
            const bool created = platform.CreateSession(settings);
            record("create=" + std::to_string(created) + ":" + platform.GetCurrentSession().sessionId);
        }
        for (const auto& session : platform.FindSessions(""))
        {
            record("session=" + session.sessionId);
        }
        const bool joined = platform.JoinSession("local_2");
        record("join=" + std::to_string(joined) + ":" + platform.GetCurrentSession().sessionId);

        for (const int64_t score : {30, 10, 50, 20})
        {
            record("submit=" + std::to_string(platform.SubmitScore("Board", score)));
        }
        for (const auto& entry : platform.QueryScores("Board", 3))
        {
            record("score=" + std::to_string(entry.rank) + ":" + std::to_string(entry.score));
        }

        platform.UnlockAchievement("a");
        platform.SetAchievementProgress("b", 0.25f);
        platform.SetAchievementProgress("c", 2.0f);
        for (const auto& achievement : platform.QueryAchievements())
        {
            record("achievement=" + achievement.id + ":" + std::to_string(achievement.progress) + ":" +
                   std::to_string(achievement.unlocked));
        }

        platform.SaveToCloud("slot_a", {9, 8});
        platform.SaveToCloud("slot_b", {7});
        for (const auto& slot : platform.ListCloudSaves())
        {
            record("slot=" + slot.slotName + ":" + std::to_string(slot.sizeBytes));
        }
        const bool deletedMissing = platform.DeleteCloudSave("slot_missing");
        record("missing=" + std::to_string(deletedMissing) + ":" + platform.GetLastError());
        return transcript;
    }
} // namespace

TEST(OnlineServices_Contract_NullAdapter)
{
    RunOnlinePlatformContract<Spark::OnlineServices::NullOnlinePlatform>();
}

TEST(OnlineServices_Contract_SteamAdapter)
{
    RunOnlinePlatformContract<Spark::OnlineServices::SteamPlatform>();
}

TEST(OnlineServices_Contract_EpicAdapter)
{
    RunOnlinePlatformContract<Spark::OnlineServices::EpicPlatform>();
}

TEST(OnlineServices_Contract_ConsoleAdapter)
{
    RunOnlinePlatformContract<Spark::OnlineServices::ConsolePlatform>();
}

TEST(OnlineServices_Contract_NullAdapterDeterministic)
{
    // The Null adapter keeps its boards, achievements and cloud slots in ordered maps and
    // ranks ties by submission order, so its results are a fixed transcript on every
    // standard library and every run, not just repeatable within one process.
    const std::string expected = "login=1\n"
                                 "player=local_Alpha\n"
                                 "create=1:local_1\n"
                                 "create=1:local_2\n"
                                 "create=1:local_3\n"
                                 "session=local_1\n"
                                 "session=local_2\n"
                                 "session=local_3\n"
                                 "join=1:local_2\n"
                                 "submit=1\n"
                                 "submit=1\n"
                                 "submit=1\n"
                                 "submit=1\n"
                                 "score=1:50\n"
                                 "score=2:30\n"
                                 "score=3:20\n"
                                 "achievement=a:1.000000:1\n"
                                 "achievement=b:0.250000:0\n"
                                 "achievement=c:1.000000:1\n"
                                 "slot=slot_a:2\n"
                                 "slot=slot_b:1\n"
                                 "missing=0:Cannot delete missing cloud slot: slot_missing\n";
    const std::string first = RecordNullPlatformTranscript();
    const std::string second = RecordNullPlatformTranscript();
    if (first != expected)
    {
        std::cerr << "  Null adapter transcript:\n" << first;
    }
    EXPECT_EQ(first, expected);
    EXPECT_EQ(second, expected);
}
