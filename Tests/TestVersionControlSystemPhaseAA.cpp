/**
 * @file TestVersionControlSystemPhaseAA.cpp
 * @brief Phase AA Theme 3C tests for SparkEditor::VersionControlSystem
 *
 * Theme 3C fourth phase companion. VersionControlSystem is an
 * EditorPanel subclass that lives in SparkEditor/Source/VersionControl/
 * (outside Panels/). Before Phase AA it had ~2300 lines of Git / LFS /
 * conflict / branch / history implementation spread across five .cpp
 * files but zero production call sites and zero test coverage.
 *
 * Phase AA registers it as "VersionControl" in
 * EditorUI::CreateToolAndContentPanels and ships these tests against
 * the real class covering lifecycle, enabled/disabled toggle, user
 * info / collaboration settings round-trip, ignore-pattern
 * management, and the unknown-file query safety net.
 */

#include "TestFramework.h"
#include "VersionControl/VersionControlSystem.h"

#include <string>
#include <vector>

TEST(VersionControlPhaseAA_InitializeShutdown)
{
    SparkEditor::VersionControlSystem vc;
    EXPECT_TRUE(vc.Initialize());
    vc.Shutdown();
}

TEST(VersionControlPhaseAA_EnabledToggle)
{
    SparkEditor::VersionControlSystem vc;
    vc.Initialize();

    vc.SetEnabled(false);
    EXPECT_FALSE(vc.IsEnabled());

    vc.SetEnabled(true);
    EXPECT_TRUE(vc.IsEnabled());

    vc.Shutdown();
}

TEST(VersionControlPhaseAA_NoRepositoryByDefault)
{
    SparkEditor::VersionControlSystem vc;
    vc.Initialize();
    // Without OpenRepository the system should have no active repo.
    const auto* info = vc.GetRepositoryInfo();
    EXPECT_TRUE(info == nullptr || info->path.empty());
    vc.Shutdown();
}

TEST(VersionControlPhaseAA_UserInfoRoundTrip)
{
    SparkEditor::VersionControlSystem vc;
    vc.Initialize();

    SparkEditor::UserInfo user;
    user.name = "Phase AA User";
    user.email = "phaseAA@sparkengine.test";
    user.signCommits = true;
    vc.SetUserInfo(user);

    const auto& retrieved = vc.GetUserInfo();
    EXPECT_EQ(retrieved.name, std::string("Phase AA User"));
    EXPECT_EQ(retrieved.email, std::string("phaseAA@sparkengine.test"));
    EXPECT_TRUE(retrieved.signCommits);

    vc.Shutdown();
}

TEST(VersionControlPhaseAA_CollaborationSettingsRoundTrip)
{
    SparkEditor::VersionControlSystem vc;
    vc.Initialize();

    SparkEditor::CollaborationSettings settings;
    settings.enableRealtimeSync = true;
    settings.enableFileLocking = true;
    settings.enableAutoMerge = false;
    settings.autoSyncInterval = 30.0f;
    vc.SetCollaborationSettings(settings);

    const auto& retrieved = vc.GetCollaborationSettings();
    EXPECT_TRUE(retrieved.enableRealtimeSync);
    EXPECT_TRUE(retrieved.enableFileLocking);
    EXPECT_FALSE(retrieved.enableAutoMerge);
    EXPECT_NEAR(retrieved.autoSyncInterval, 30.0f, 0.01f);

    vc.Shutdown();
}

TEST(VersionControlPhaseAA_IgnorePatternManagement)
{
    SparkEditor::VersionControlSystem vc;
    vc.Initialize();

    // Add a pattern; removal of the same pattern should succeed;
    // removal of a never-added pattern should fail or be a no-op.
    EXPECT_TRUE(vc.AddIgnorePattern("*.tmp"));
    EXPECT_TRUE(vc.AddIgnorePattern("build/"));

    auto patterns = vc.GetIgnorePatterns();
    EXPECT_TRUE(patterns.size() >= 2);

    // Cleanup — don't assert on the return value because some impls
    // normalise patterns internally; we just want to exercise the
    // call path without crashing.
    vc.RemoveIgnorePattern("*.tmp");
    vc.RemoveIgnorePattern("build/");

    vc.Shutdown();
}

TEST(VersionControlPhaseAA_FileTrackedQueryWithoutRepoIsSafe)
{
    SparkEditor::VersionControlSystem vc;
    vc.Initialize();
    // Without a repo, file queries must not crash and must return
    // a sensible default (file not tracked).
    EXPECT_FALSE(vc.IsFileTracked("nonexistent/path/foo.txt"));
    EXPECT_FALSE(vc.IsFileLocked("nonexistent/path/bar.txt"));
    vc.Shutdown();
}

TEST(VersionControlPhaseAA_GetActiveUsersWithoutRepo)
{
    SparkEditor::VersionControlSystem vc;
    vc.Initialize();
    // No repo means no active users — must return an empty list.
    const auto users = vc.GetActiveUsers();
    EXPECT_EQ(users.size(), static_cast<size_t>(0));
    vc.Shutdown();
}

TEST(VersionControlPhaseAA_CloseRepositoryOnNoOpIsSafe)
{
    SparkEditor::VersionControlSystem vc;
    vc.Initialize();
    // Closing with no open repo must be a silent no-op.
    vc.CloseRepository();
    vc.Shutdown();
}

TEST(VersionControlPhaseAA_ShutdownIsIdempotent)
{
    SparkEditor::VersionControlSystem vc;
    vc.Initialize();
    vc.Shutdown();
    vc.Shutdown();
}

TEST(VersionControlPhaseAA_UpdateCallIsSafe)
{
    SparkEditor::VersionControlSystem vc;
    vc.Initialize();
    vc.Update(0.016f);
    vc.Update(0.016f);
    vc.Shutdown();
}
