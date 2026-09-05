/**
 * @file VersionControlGitOps.cpp
 * @brief Git operations: staging, committing, pushing, pulling, branching, history, and diffs
 *
 * Every git invocation here is built as an argument vector and handed straight to the process builder.
 * No shell is involved, so nothing on this path needs quoting or metacharacter filtering: a commit message
 * with spaces and apostrophes, or a path under "C:/Rock & Roll/", reaches git as a single argv element.
 *
 * Removing the shell does not remove git's own option parsing. A branch, remote or path that begins with
 * '-' is still read as an option (e.g. "--upload-pack=<program>"), so every user-supplied operand is
 * screened with IsOptionLike() and pathspecs are closed off with a "--" separator.
 */

#include "VersionControlSystem.h"
#include "Utils/LogMacros.h"

namespace SparkEditor
{

    // ============================================================================
    // Staging and Committing
    // ============================================================================

    VCSOperationResult VersionControlSystem::StageFiles(const std::vector<std::string>& filePaths)
    {
        if (!m_repositoryInfo)
            return {false, "No repository open", "", -1, 0.0f, {}};

        VCSOperationResult lastResult;
        for (const auto& file : filePaths)
        {
            if (IsOptionLike(file))
                return {false, "File path may not start with '-': " + file, "", -1, 0.0f, {}};

            lastResult = ExecuteGit({"add", "--", file}, m_repositoryInfo->path);
            if (!lastResult.success)
            {
                return lastResult;
            }
        }
        RefreshStatus();
        return lastResult;
    }

    VCSOperationResult VersionControlSystem::UnstageFiles(const std::vector<std::string>& filePaths)
    {
        if (!m_repositoryInfo)
            return {false, "No repository open", "", -1, 0.0f, {}};

        VCSOperationResult lastResult;
        for (const auto& file : filePaths)
        {
            if (IsOptionLike(file))
                return {false, "File path may not start with '-': " + file, "", -1, 0.0f, {}};

            lastResult = ExecuteGit({"reset", "HEAD", "--", file}, m_repositoryInfo->path);
            if (!lastResult.success)
            {
                return lastResult;
            }
        }
        RefreshStatus();
        return lastResult;
    }

    VCSOperationResult VersionControlSystem::Commit(const std::string& message, const std::string& description)
    {
        if (!m_repositoryInfo)
            return {false, "No repository open", "", -1, 0.0f, {}};

        if (message.empty())
            return {false, "Commit message is empty", "", -1, 0.0f, {}};

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Committing with message: %s", message.c_str());
        std::vector<std::string> args = {"commit", "-m", message};
        if (!description.empty())
        {
            args.push_back("-m");
            args.push_back(description);
        }

        VCSOperationResult result = ExecuteGit(args, m_repositoryInfo->path);
        if (result.success)
        {
            RefreshStatus();
        }
        return result;
    }

    // ============================================================================
    // Remote Operations
    // ============================================================================

    VCSOperationResult VersionControlSystem::Push(const std::string& remoteName, const std::string& branchName,
                                                  std::function<void(float)> progressCallback)
    {
        if (!m_repositoryInfo)
            return {false, "No repository open", "", -1, 0.0f, {}};

        if (IsOptionLike(remoteName) || IsOptionLike(branchName))
            return {false, "Remote and branch names may not start with '-'", "", -1, 0.0f, {}};

        std::string branch = branchName.empty() ? m_repositoryInfo->currentBranch.name : branchName;
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Pushing to %s/%s", remoteName.c_str(), branch.c_str());
        if (progressCallback)
            progressCallback(0.0f);
        VCSOperationResult result = ExecuteGit({"push", remoteName, branch}, m_repositoryInfo->path);
        if (progressCallback)
            progressCallback(1.0f);
        return result;
    }

    VCSOperationResult VersionControlSystem::Pull(const std::string& remoteName, const std::string& branchName,
                                                  std::function<void(float)> progressCallback)
    {
        if (!m_repositoryInfo)
            return {false, "No repository open", "", -1, 0.0f, {}};

        if (IsOptionLike(remoteName) || IsOptionLike(branchName))
            return {false, "Remote and branch names may not start with '-'", "", -1, 0.0f, {}};

        std::string branch = branchName.empty() ? m_repositoryInfo->currentBranch.name : branchName;
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Pulling from %s/%s", remoteName.c_str(), branch.c_str());
        if (progressCallback)
            progressCallback(0.0f);
        VCSOperationResult result = ExecuteGit({"pull", remoteName, branch}, m_repositoryInfo->path);
        if (progressCallback)
            progressCallback(1.0f);
        if (result.success)
        {
            RefreshStatus();
            DetectMergeConflicts();
        }
        return result;
    }

    VCSOperationResult VersionControlSystem::Fetch(const std::string& remoteName)
    {
        if (!m_repositoryInfo)
            return {false, "No repository open", "", -1, 0.0f, {}};

        if (IsOptionLike(remoteName))
            return {false, "Remote name may not start with '-'", "", -1, 0.0f, {}};

        return ExecuteGit({"fetch", remoteName}, m_repositoryInfo->path);
    }

    // ============================================================================
    // Branch Operations
    // ============================================================================

    VCSOperationResult VersionControlSystem::CreateBranch(const std::string& branchName, const std::string& baseBranch)
    {
        if (!m_repositoryInfo)
            return {false, "No repository open", "", -1, 0.0f, {}};

        if (branchName.empty())
            return {false, "Branch name is empty", "", -1, 0.0f, {}};

        if (IsOptionLike(branchName) || IsOptionLike(baseBranch))
            return {false, "Branch name may not start with '-'", "", -1, 0.0f, {}};

        std::vector<std::string> args;
        if (baseBranch.empty())
        {
            args = {"branch", branchName};
        }
        else
        {
            args = {"checkout", "-b", branchName, baseBranch};
        }
        VCSOperationResult result = ExecuteGit(args, m_repositoryInfo->path);
        if (result.success)
        {
            RefreshStatus();
        }
        return result;
    }

    VCSOperationResult VersionControlSystem::SwitchBranch(const std::string& branchName)
    {
        if (!m_repositoryInfo)
            return {false, "No repository open", "", -1, 0.0f, {}};

        if (branchName.empty())
            return {false, "Branch name is empty", "", -1, 0.0f, {}};

        if (IsOptionLike(branchName))
            return {false, "Branch name may not start with '-'", "", -1, 0.0f, {}};

        VCSOperationResult result = ExecuteGit({"checkout", branchName}, m_repositoryInfo->path);
        if (result.success)
        {
            m_repositoryInfo->currentBranch.name = branchName;
            RefreshStatus();
        }
        return result;
    }

    VCSOperationResult VersionControlSystem::MergeBranch(const std::string& branchName)
    {
        if (!m_repositoryInfo)
            return {false, "No repository open", "", -1, 0.0f, {}};

        if (branchName.empty())
            return {false, "Branch name is empty", "", -1, 0.0f, {}};

        if (IsOptionLike(branchName))
            return {false, "Branch name may not start with '-'", "", -1, 0.0f, {}};

        VCSOperationResult result = ExecuteGit({"merge", branchName}, m_repositoryInfo->path);
        if (result.success)
        {
            RefreshStatus();
            DetectMergeConflicts();
        }
        return result;
    }

    VCSOperationResult VersionControlSystem::DeleteBranch(const std::string& branchName, bool force)
    {
        if (!m_repositoryInfo)
            return {false, "No repository open", "", -1, 0.0f, {}};

        if (branchName.empty())
            return {false, "Branch name is empty", "", -1, 0.0f, {}};

        if (IsOptionLike(branchName))
            return {false, "Branch name may not start with '-'", "", -1, 0.0f, {}};

        return ExecuteGit({"branch", force ? "-D" : "-d", "--", branchName}, m_repositoryInfo->path);
    }

    // ============================================================================
    // History and Diff
    // ============================================================================

    std::vector<CommitInfo> VersionControlSystem::GetCommitHistory(int maxCommits, const std::string& branchName)
    {
        if (!m_repositoryInfo)
            return {};

        // The format string is a single argv element; it must not carry shell quotes, or git emits them
        // verbatim as part of the record separator and appends a stray quote to every commit subject.
        std::vector<std::string> args = {
            "log", "--format=COMMIT_SEP%nHash:%H%nShortHash:%h%nAuthor:%an%nEmail:%ae%nDate:%aI%nMessage:%s", "-n",
            std::to_string(maxCommits)};
        if (!branchName.empty())
        {
            if (IsOptionLike(branchName))
                return {};
            args.push_back(branchName);
        }

        VCSOperationResult result = ExecuteGit(args, m_repositoryInfo->path);
        if (result.success)
        {
            m_commitHistory = ParseGitLog(result.output);
            return m_commitHistory;
        }
        return {};
    }

    std::string VersionControlSystem::GetFileDiff(const std::string& filePath, const std::string& commitHash1,
                                                  const std::string& commitHash2)
    {
        if (!m_repositoryInfo)
            return "";

        if (IsOptionLike(filePath) || IsOptionLike(commitHash1) || IsOptionLike(commitHash2))
            return "";

        std::vector<std::string> args = {"diff"};
        if (!commitHash1.empty())
        {
            args.push_back(commitHash1);
            if (!commitHash2.empty())
            {
                args.push_back(commitHash2);
            }
        }
        args.push_back("--");
        args.push_back(filePath);

        VCSOperationResult result = ExecuteGit(args, m_repositoryInfo->path);
        return result.success ? result.output : "";
    }

    VCSOperationResult VersionControlSystem::RevertFile(const std::string& filePath)
    {
        if (!m_repositoryInfo)
            return {false, "No repository open", "", -1, 0.0f, {}};

        if (IsOptionLike(filePath))
            return {false, "File path may not start with '-': " + filePath, "", -1, 0.0f, {}};

        VCSOperationResult result = ExecuteGit({"checkout", "--", filePath}, m_repositoryInfo->path);
        if (result.success)
        {
            RefreshStatus();
        }
        return result;
    }

} // namespace SparkEditor
