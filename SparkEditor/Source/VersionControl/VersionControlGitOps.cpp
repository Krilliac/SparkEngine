/**
 * @file VersionControlGitOps.cpp
 * @brief Git operations: staging, committing, pushing, pulling, branching, history, and diffs
 */

#include "VersionControlSystem.h"

namespace SparkEditor
{

    // ============================================================================
    // Shell safety helper
    // ============================================================================

    static bool ContainsShellMetachars(const std::string& str)
    {
        for (char c : str)
        {
            switch (c)
            {
            case ';':
            case '|':
            case '&':
            case '$':
            case '`':
            case '\n':
            case '\r':
                return true;
            default:
                break;
            }
        }
        return false;
    }

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
            if (ContainsShellMetachars(file))
            {
                lastResult.success = false;
                lastResult.errorMessage = "File path contains unsafe characters: " + file;
                lastResult.exitCode = -1;
                return lastResult;
            }
            std::string cmd = "git add \"" + file + "\"";
            lastResult = ExecuteCommand(cmd, m_repositoryInfo->path);
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
            if (ContainsShellMetachars(file))
            {
                lastResult.success = false;
                lastResult.errorMessage = "File path contains unsafe characters: " + file;
                lastResult.exitCode = -1;
                return lastResult;
            }
            std::string cmd = "git reset HEAD \"" + file + "\"";
            lastResult = ExecuteCommand(cmd, m_repositoryInfo->path);
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

        // Escape single quotes in message
        std::string escapedMsg = message;
        std::string::size_type pos = 0;
        while ((pos = escapedMsg.find('\'', pos)) != std::string::npos)
        {
            escapedMsg.replace(pos, 1, "'\\''");
            pos += 4;
        }

        std::string cmd = "git commit -m '" + escapedMsg + "'";
        if (!description.empty())
        {
            std::string escapedDesc = description;
            pos = 0;
            while ((pos = escapedDesc.find('\'', pos)) != std::string::npos)
            {
                escapedDesc.replace(pos, 1, "'\\''");
                pos += 4;
            }
            cmd += " -m '" + escapedDesc + "'";
        }

        VCSOperationResult result = ExecuteCommand(cmd, m_repositoryInfo->path);
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

        std::string branch = branchName.empty() ? m_repositoryInfo->currentBranch.name : branchName;
        std::string cmd = "git push " + remoteName + " " + branch;
        if (progressCallback)
            progressCallback(0.0f);
        VCSOperationResult result = ExecuteCommand(cmd, m_repositoryInfo->path);
        if (progressCallback)
            progressCallback(1.0f);
        return result;
    }

    VCSOperationResult VersionControlSystem::Pull(const std::string& remoteName, const std::string& branchName,
                                                  std::function<void(float)> progressCallback)
    {
        if (!m_repositoryInfo)
            return {false, "No repository open", "", -1, 0.0f, {}};

        std::string branch = branchName.empty() ? m_repositoryInfo->currentBranch.name : branchName;
        std::string cmd = "git pull " + remoteName + " " + branch;
        if (progressCallback)
            progressCallback(0.0f);
        VCSOperationResult result = ExecuteCommand(cmd, m_repositoryInfo->path);
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

        std::string cmd = "git fetch " + remoteName;
        return ExecuteCommand(cmd, m_repositoryInfo->path);
    }

    // ============================================================================
    // Branch Operations
    // ============================================================================

    VCSOperationResult VersionControlSystem::CreateBranch(const std::string& branchName, const std::string& baseBranch)
    {
        if (!m_repositoryInfo)
            return {false, "No repository open", "", -1, 0.0f, {}};

        std::string cmd;
        if (baseBranch.empty())
        {
            cmd = "git branch " + branchName;
        }
        else
        {
            cmd = "git checkout -b " + branchName + " " + baseBranch;
        }
        VCSOperationResult result = ExecuteCommand(cmd, m_repositoryInfo->path);
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

        std::string cmd = "git checkout " + branchName;
        VCSOperationResult result = ExecuteCommand(cmd, m_repositoryInfo->path);
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

        std::string cmd = "git merge " + branchName;
        VCSOperationResult result = ExecuteCommand(cmd, m_repositoryInfo->path);
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

        std::string flag = force ? "-D" : "-d";
        std::string cmd = "git branch " + flag + " " + branchName;
        return ExecuteCommand(cmd, m_repositoryInfo->path);
    }

    // ============================================================================
    // History and Diff
    // ============================================================================

    std::vector<CommitInfo> VersionControlSystem::GetCommitHistory(int maxCommits, const std::string& branchName)
    {
        if (!m_repositoryInfo)
            return {};

        std::string cmd =
            "git log --format='COMMIT_SEP%nHash:%H%nShortHash:%h%nAuthor:%an%nEmail:%ae%nDate:%aI%nMessage:%s'";
        cmd += " -n " + std::to_string(maxCommits);
        if (!branchName.empty())
        {
            cmd += " " + branchName;
        }

        VCSOperationResult result = ExecuteCommand(cmd, m_repositoryInfo->path);
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

        std::string cmd;
        if (!commitHash1.empty() && !commitHash2.empty())
        {
            cmd = "git diff " + commitHash1 + " " + commitHash2 + " -- \"" + filePath + "\"";
        }
        else if (!commitHash1.empty())
        {
            cmd = "git diff " + commitHash1 + " -- \"" + filePath + "\"";
        }
        else
        {
            cmd = "git diff -- \"" + filePath + "\"";
        }

        VCSOperationResult result = ExecuteCommand(cmd, m_repositoryInfo->path);
        return result.success ? result.output : "";
    }

    VCSOperationResult VersionControlSystem::RevertFile(const std::string& filePath)
    {
        if (!m_repositoryInfo)
            return {false, "No repository open", "", -1, 0.0f, {}};

        std::string cmd = "git checkout -- \"" + filePath + "\"";
        VCSOperationResult result = ExecuteCommand(cmd, m_repositoryInfo->path);
        if (result.success)
        {
            RefreshStatus();
        }
        return result;
    }

} // namespace SparkEditor
