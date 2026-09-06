/**
 * @file TestVersionControlSystemGitReal.cpp
 * @brief Tests that actually run git through SparkEditor::VersionControlSystem
 *
 * TestVersionControlSystemPhaseAA.cpp only exercises the "no repository open" short-circuits, so every git
 * command, argument-building path and output parser had zero coverage. These tests create a real repository
 * in a scratch directory and drive Commit / StageFiles / GetCommitHistory / RefreshStatus / GetFileDiff
 * against it, which is where the argv-versus-shell-quoting defects lived.
 *
 * A test skips only when git itself is missing (`git --version` fails). Every other failure —
 * repository creation, identity configuration, or a null RepositoryInfo — is a hard failure, so a
 * regression in the git argv path can never masquerade as "git is not available".
 */

#include "TestFramework.h"
#include "VersionControl/VersionControlSystem.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    /// @brief A throwaway git repository with a deterministic local identity.
    class ScratchRepo
    {
      public:
        explicit ScratchRepo(const std::string& tag)
        {
            static int counter = 0;
            m_path = std::filesystem::temp_directory_path() / ("spark_vcs_" + tag + "_" + std::to_string(++counter));
            std::error_code ec;
            std::filesystem::remove_all(m_path, ec);
            std::filesystem::create_directories(m_path, ec);
        }

        ~ScratchRepo()
        {
            std::error_code ec;
            std::filesystem::remove_all(m_path, ec);
        }

        ScratchRepo(const ScratchRepo&) = delete;
        ScratchRepo& operator=(const ScratchRepo&) = delete;

        const std::filesystem::path& Path() const { return m_path; }
        std::string PathString() const { return m_path.string(); }

        void WriteFile(const std::string& relativePath, const std::string& contents) const
        {
            const std::filesystem::path target = m_path / relativePath;
            std::error_code ec;
            if (target.has_parent_path())
                std::filesystem::create_directories(target.parent_path(), ec);
            std::ofstream out(target, std::ios::binary);
            out << contents;
        }

        /// @brief Give the repo a local identity so commits do not depend on the machine's global git config.
        bool ConfigureIdentity() const
        {
            const std::filesystem::path config = m_path / ".git" / "config";
            std::error_code ec;
            if (!std::filesystem::exists(config, ec))
                return false;
            std::ofstream out(config, std::ios::app);
            if (!out.is_open())
                return false;
            out << "\n[user]\n\tname = Spark Test\n\temail = spark-test@example.invalid\n";
            out << "[commit]\n\tgpgsign = false\n";
            return out.good();
        }

      private:
        std::filesystem::path m_path;
    };

    /// @brief Owns one VersionControlSystem for the duration of a test.
    ///
    /// Two things this class exists to guarantee:
    ///  1. Shutdown() runs even when a fatal assertion unwinds the test, so the watcher/queue threads
    ///     Initialize() started never outlive the test.
    ///  2. Only a failing `git --version` is skippable. Folding InitializeRepository / ConfigureIdentity /
    ///     GetRepositoryInfo into the same "git is not available" skip turned a real regression in
    ///     ExecuteGit's -C handling into five green skips — a check that stops checking.
    class VcsSession
    {
      public:
        VcsSession() = default;

        ~VcsSession()
        {
            if (m_initialized)
                m_vcs.Shutdown();
        }

        VcsSession(const VcsSession&) = delete;
        VcsSession& operator=(const VcsSession&) = delete;

        SparkEditor::VersionControlSystem& Vcs() { return m_vcs; }

        /// @brief True only when git itself could not be run. This is the sole skippable condition.
        bool GitIsMissing()
        {
            m_initialized = m_vcs.Initialize();
            return !m_initialized;
        }

        /// @brief Create the repository under test. Every failure here is a hard test failure.
        void OpenRepository(const ScratchRepo& repo)
        {
            const SparkEditor::VCSOperationResult init = m_vcs.InitializeRepository(repo.PathString());
            if (!init.success)
                std::cerr << "  git init failed: " << init.errorMessage << " (exit " << init.exitCode << ")\n";
            ASSERT_TRUE(init.success);
            ASSERT_TRUE(repo.ConfigureIdentity());
            ASSERT_TRUE(m_vcs.GetRepositoryInfo() != nullptr);
        }

      private:
        SparkEditor::VersionControlSystem m_vcs;
        bool m_initialized = false;
    };
} // namespace

TEST(VersionControlGitReal_CommitKeepsAMultiWordMessageWithAnApostrophe)
{
    ScratchRepo repo("commit_msg");
    VcsSession session;
    if (session.GitIsMissing())
    {
        SKIP_TEST("git --version failed: git is not installed");
    }
    session.OpenRepository(repo);
    SparkEditor::VersionControlSystem& vc = session.Vcs();

    repo.WriteFile("notes.txt", "first\n");
    const auto staged = vc.StageFiles({"notes.txt"});
    EXPECT_TRUE(staged.success);

    // A message with spaces and an apostrophe must reach git as ONE argv element. Building a shell-style
    // command string split it into "-m" "'Fix" "crash" ... and git treated the tail as a pathspec.
    const std::string message = "Fix crash when the player's gun jams";
    const auto committed = vc.Commit(message);
    EXPECT_TRUE(committed.success);

    const auto history = vc.GetCommitHistory(5);
    ASSERT_EQ(history.size(), static_cast<size_t>(1));
    EXPECT_EQ(history[0].message, message);
    EXPECT_FALSE(history[0].hash.empty());
    EXPECT_EQ(history[0].author, std::string("Spark Test"));
}

TEST(VersionControlGitReal_CommitSubjectHasNoShellQuoteResidue)
{
    ScratchRepo repo("quote_residue");
    VcsSession session;
    if (session.GitIsMissing())
    {
        SKIP_TEST("git --version failed: git is not installed");
    }
    session.OpenRepository(repo);
    SparkEditor::VersionControlSystem& vc = session.Vcs();

    repo.WriteFile("a.txt", "a\n");
    EXPECT_TRUE(vc.StageFiles({"a.txt"}).success);
    EXPECT_TRUE(vc.Commit("Second commit").success);

    const auto history = vc.GetCommitHistory(5);
    ASSERT_EQ(history.size(), static_cast<size_t>(1));
    // The --format string used to carry literal single quotes, which git echoed into every record: the
    // subject came back as "Second commit'" and the separator line as "'COMMIT_SEP".
    EXPECT_EQ(history[0].message, std::string("Second commit"));
    EXPECT_TRUE(history[0].message.find('\'') == std::string::npos);
    EXPECT_TRUE(history[0].message.find("COMMIT_SEP") == std::string::npos);
}

TEST(VersionControlGitReal_PathsWithSpacesAndAmpersandsAreStageable)
{
    ScratchRepo repo("odd_paths");
    VcsSession session;
    if (session.GitIsMissing())
    {
        SKIP_TEST("git --version failed: git is not installed");
    }
    session.OpenRepository(repo);
    SparkEditor::VersionControlSystem& vc = session.Vcs();

    // '&' and ' ' are meaningless without a shell, and git is launched with no shell. The old metacharacter
    // filter rejected this path outright with "File path contains unsafe characters".
    const std::string oddPath = "Rock & Roll/track one.txt";
    repo.WriteFile(oddPath, "riff\n");

    const auto staged = vc.StageFiles({oddPath});
    EXPECT_TRUE(staged.success);
    EXPECT_TRUE(vc.Commit("Add a track under an ampersand directory").success);

    const auto history = vc.GetCommitHistory(5);
    ASSERT_EQ(history.size(), static_cast<size_t>(1));
    EXPECT_EQ(history[0].message, std::string("Add a track under an ampersand directory"));
}

TEST(VersionControlGitReal_StatusAndDiffSeeTheWorkingTree)
{
    ScratchRepo repo("status_diff");
    VcsSession session;
    if (session.GitIsMissing())
    {
        SKIP_TEST("git --version failed: git is not installed");
    }
    session.OpenRepository(repo);
    SparkEditor::VersionControlSystem& vc = session.Vcs();

    repo.WriteFile("tracked.txt", "one\n");
    EXPECT_TRUE(vc.StageFiles({"tracked.txt"}).success);
    EXPECT_TRUE(vc.Commit("Add tracked file").success);

    repo.WriteFile("tracked.txt", "one\ntwo\n");
    vc.RefreshStatus();

    const SparkEditor::RepositoryInfo* info = vc.GetRepositoryInfo();
    ASSERT_TRUE(info != nullptr);
    EXPECT_TRUE(info->hasUncommittedChanges);

    bool sawTracked = false;
    for (const auto& change : info->changedFiles)
    {
        if (change.filePath == "tracked.txt")
            sawTracked = true;
    }
    EXPECT_TRUE(sawTracked);

    const std::string diff = vc.GetFileDiff("tracked.txt");
    EXPECT_STR_CONTAINS(diff, "tracked.txt");
    EXPECT_STR_CONTAINS(diff, "+two");
}

TEST(VersionControlGitReal_UnstageRemovesTheFileFromTheIndex)
{
    ScratchRepo repo("unstage");
    VcsSession session;
    if (session.GitIsMissing())
    {
        SKIP_TEST("git --version failed: git is not installed");
    }
    session.OpenRepository(repo);
    SparkEditor::VersionControlSystem& vc = session.Vcs();

    repo.WriteFile("base.txt", "base\n");
    EXPECT_TRUE(vc.StageFiles({"base.txt"}).success);
    EXPECT_TRUE(vc.Commit("Base").success);

    repo.WriteFile("second.txt", "second\n");
    EXPECT_TRUE(vc.StageFiles({"second.txt"}).success);
    EXPECT_TRUE(vc.UnstageFiles({"second.txt"}).success);

    const SparkEditor::RepositoryInfo* info = vc.GetRepositoryInfo();
    ASSERT_TRUE(info != nullptr);

    bool secondIsUntracked = false;
    for (const auto& change : info->changedFiles)
    {
        if (change.filePath == "second.txt" && change.status == SparkEditor::FileStatus::UNTRACKED)
            secondIsUntracked = true;
    }
    EXPECT_TRUE(secondIsUntracked);
}
