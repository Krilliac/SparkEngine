/**
 * @file VersionControlHelpers.cpp
 * @brief Parsing and utility helpers for VersionControlSystem
 */

#include "VersionControlSystem.h"
#include "Utils/LogMacros.h"
#include <sstream>
#include <filesystem>

namespace SparkEditor
{

    // ============================================================================
    // Git Output Parsing
    // ============================================================================

    std::vector<FileChange> VersionControlSystem::ParseGitStatus(const std::string& output)
    {
        std::vector<FileChange> changes;
        std::istringstream stream(output);
        std::string line;

        while (std::getline(stream, line))
        {
            if (line.size() < 4)
                continue; // Minimum: "XY filename"

            char indexStatus = line[0];
            char workTreeStatus = line[1];
            // line[2] is a space
            std::string filePath = line.substr(3);

            // Remove leading/trailing whitespace from path
            filePath.erase(0, filePath.find_first_not_of(' '));
            filePath.erase(filePath.find_last_not_of(" \r\n") + 1);

            // Handle renamed files: "R  old -> new"
            auto arrowPos = filePath.find(" -> ");
            if (arrowPos != std::string::npos)
            {
                filePath = filePath.substr(arrowPos + 4);
            }

            FileChange change;
            change.filePath = filePath;

            if (indexStatus == '?' && workTreeStatus == '?')
            {
                change.status = FileStatus::UNTRACKED;
            }
            else if (indexStatus == 'A' || workTreeStatus == 'A')
            {
                change.status = FileStatus::ADDED;
            }
            else if (indexStatus == 'D' || workTreeStatus == 'D')
            {
                change.status = FileStatus::DELETED;
            }
            else if (indexStatus == 'R' || workTreeStatus == 'R')
            {
                change.status = FileStatus::RENAMED;
            }
            else if (indexStatus == 'M' || workTreeStatus == 'M')
            {
                change.status = FileStatus::MODIFIED;
            }
            else if (indexStatus == 'U' || workTreeStatus == 'U')
            {
                change.status = FileStatus::CONFLICTED;
                change.hasConflictMarkers = true;
            }
            else if (indexStatus == 'C' || workTreeStatus == 'C')
            {
                change.status = FileStatus::COPIED;
            }
            else
            {
                change.status = FileStatus::MODIFIED;
            }

            // Check if file should use LFS
            change.isLFS = ShouldUseLFS(filePath);

            changes.push_back(change);
        }

        return changes;
    }

    std::vector<CommitInfo> VersionControlSystem::ParseGitLog(const std::string& output)
    {
        std::vector<CommitInfo> commits;
        std::istringstream stream(output);
        std::string line;
        CommitInfo current;
        bool hasCommit = false;

        while (std::getline(stream, line))
        {
            // Remove trailing CR
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.contains("COMMIT_SEP"))
            {
                if (hasCommit && !current.hash.empty())
                {
                    commits.push_back(current);
                }
                current = CommitInfo{};
                hasCommit = true;
                continue;
            }

            auto colonPos = line.find(':');
            if (colonPos == std::string::npos)
                continue;

            std::string key = line.substr(0, colonPos);
            std::string value = line.substr(colonPos + 1);

            if (key == "Hash")
            {
                current.hash = value;
            }
            else if (key == "ShortHash")
            {
                current.shortHash = value;
            }
            else if (key == "Author")
            {
                current.author = value;
            }
            else if (key == "Email")
            {
                current.authorEmail = value;
            }
            else if (key == "Date")
            {
                // ISO 8601 date - store as-is; full parsing would require more code
                // Just leave timestamp at epoch for simplicity
            }
            else if (key == "Message")
            {
                current.message = value;
            }
        }

        // Don't forget the last commit
        if (hasCommit && !current.hash.empty())
        {
            commits.push_back(current);
        }

        return commits;
    }

    std::vector<BranchInfo> VersionControlSystem::ParseGitBranches(const std::string& output)
    {
        std::vector<BranchInfo> branches;
        std::istringstream stream(output);
        std::string line;

        while (std::getline(stream, line))
        {
            if (line.empty())
                continue;
            // Remove trailing CR
            if (line.back() == '\r')
                line.pop_back();

            BranchInfo info;
            info.isCurrent = false;
            info.isRemote = false;

            // Check for current branch marker
            if (line.size() >= 2 && line[0] == '*')
            {
                info.isCurrent = true;
                line = line.substr(2);
            }
            else
            {
                // Remove leading whitespace
                line.erase(0, line.find_first_not_of(' '));
            }

            // Skip HEAD pointer entries
            if (line.contains("->"))
                continue;

            // Check for remote branches
            if (line.find("remotes/") == 0)
            {
                info.isRemote = true;
                line = line.substr(8); // Remove "remotes/"
            }

            info.name = line;
            branches.push_back(info);
        }

        return branches;
    }

    // ============================================================================
    // File System Watching and Auto-Sync
    // ============================================================================

    void VersionControlSystem::UpdateFileSystemWatcher()
    {
        // Simplified: just track elapsed time since last status update
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - m_lastStatusUpdate).count();
        if (elapsed >= m_statusUpdateInterval)
        {
            RefreshStatus();
        }
    }

    void VersionControlSystem::HandleFileSystemChanges(const std::vector<std::string>& changedFiles)
    {
        if (changedFiles.empty())
            return;
        RefreshStatus();
    }

    void VersionControlSystem::AutoSync()
    {
        if (!m_repositoryInfo)
            return;
        if (m_collaborationSettings.autoSyncInterval <= 0.0f)
            return;

        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - m_lastAutoSync).count();
        if (elapsed >= m_collaborationSettings.autoSyncInterval)
        {
            Fetch();
            m_lastAutoSync = now;
        }
    }

    // ============================================================================
    // Conflict Detection and Auto-Resolution
    // ============================================================================

    void VersionControlSystem::DetectMergeConflicts()
    {
        if (!m_repositoryInfo)
            return;

        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Detecting merge conflicts");
        m_repositoryInfo->conflicts.clear();
        for (const auto& change : m_repositoryInfo->changedFiles)
        {
            if (change.status == FileStatus::CONFLICTED)
            {
                MergeConflict conflict;
                conflict.filePath = change.filePath;
                conflict.conflictType = change.conflictType;
                conflict.description = "Merge conflict in " + change.filePath;
                conflict.isResolved = false;
                m_repositoryInfo->conflicts.push_back(conflict);
            }
        }

        if (m_collaborationSettings.enableAutoMerge)
        {
            AutoResolveConflicts();
        }
    }

    void VersionControlSystem::AutoResolveConflicts()
    {
        if (!m_repositoryInfo)
            return;

        for (auto& conflict : m_repositoryInfo->conflicts)
        {
            if (conflict.isResolved)
                continue;
            AssetMergeHandler* handler = GetMergeHandler(conflict.filePath);
            if (handler && handler->CanMerge(conflict.filePath))
            {
                handler->AutoMerge(conflict);
            }
        }
    }

    AssetMergeHandler* VersionControlSystem::GetMergeHandler(const std::string& filePath)
    {
        for (auto& handler : m_mergeHandlers)
        {
            if (handler->CanMerge(filePath))
            {
                return handler.get();
            }
        }
        return nullptr;
    }

    // ============================================================================
    // LFS Helpers
    // ============================================================================

    bool VersionControlSystem::InitializeLFS()
    {
        if (!m_repositoryInfo)
            return false;

        VCSOperationResult result = ExecuteCommand("git lfs install", m_repositoryInfo->path);
        if (result.success)
        {
            m_repositoryInfo->hasLFS = true;
            // Re-check version
            VCSOperationResult verResult = ExecuteCommand("git lfs version", m_repositoryInfo->path);
            if (verResult.success)
            {
                std::string ver = verResult.output;
                ver.erase(ver.find_last_not_of(" \n\r\t") + 1);
                m_repositoryInfo->lfsVersion = ver;
            }
            return true;
        }
        return false;
    }

    bool VersionControlSystem::ShouldUseLFS(const std::string& filePath) const
    {
        std::filesystem::path p(filePath);
        std::string ext = p.extension().string();
        if (ext.empty())
            return false;

        // Build a wildcard pattern like "*.png" from the extension
        std::string wildcard = "*" + ext;

        for (const auto& pattern : m_lfsPatterns)
        {
            if (pattern == wildcard)
                return true;
        }
        return false;
    }

} // namespace SparkEditor
