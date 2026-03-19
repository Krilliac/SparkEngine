/**
 * @file VersionControlConflicts.cpp
 * @brief Locking, conflict resolution, user settings, file status queries, and ignore patterns
 */

#include "VersionControlSystem.h"
#include <fstream>
#include <sstream>
#include <filesystem>

namespace SparkEditor
{

    // ============================================================================
    // Locking
    // ============================================================================

    VCSOperationResult VersionControlSystem::LockFile(const std::string& filePath)
    {
        if (!m_repositoryInfo)
            return {false, "No repository open", "", -1, 0.0f, {}};

        std::string cmd = "git lfs lock \"" + filePath + "\"";
        VCSOperationResult result = ExecuteCommand(cmd, m_repositoryInfo->path);
        if (result.success)
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_fileStatusCache[filePath] = FileStatus::LOCKED;
        }
        return result;
    }

    VCSOperationResult VersionControlSystem::UnlockFile(const std::string& filePath)
    {
        if (!m_repositoryInfo)
            return {false, "No repository open", "", -1, 0.0f, {}};

        std::string cmd = "git lfs unlock \"" + filePath + "\"";
        VCSOperationResult result = ExecuteCommand(cmd, m_repositoryInfo->path);
        if (result.success)
        {
            RefreshStatus();
        }
        return result;
    }

    // ============================================================================
    // Conflict Resolution
    // ============================================================================

    bool VersionControlSystem::ResolveMergeConflict(MergeConflict& conflict, const std::string& resolution)
    {
        if (!m_repositoryInfo)
            return false;

        conflict.resolution = resolution;
        conflict.isResolved = true;

        // Stage the resolved file
        std::string cmd = "git add \"" + conflict.filePath + "\"";
        VCSOperationResult result = ExecuteCommand(cmd, m_repositoryInfo->path);
        if (result.success)
        {
            // Remove from conflicts list
            auto& conflicts = m_repositoryInfo->conflicts;
            conflicts.erase(std::remove_if(conflicts.begin(), conflicts.end(),
                                           [&](const MergeConflict& c) { return c.filePath == conflict.filePath; }),
                            conflicts.end());
            return true;
        }
        return false;
    }

    void VersionControlSystem::RegisterMergeHandler(std::unique_ptr<AssetMergeHandler> handler)
    {
        if (handler)
        {
            m_mergeHandlers.push_back(std::move(handler));
        }
    }

    void VersionControlSystem::SetUserInfo(const UserInfo& userInfo)
    {
        m_userInfo = userInfo;
    }

    void VersionControlSystem::SetCollaborationSettings(const CollaborationSettings& settings)
    {
        m_collaborationSettings = settings;
    }

    // ============================================================================
    // File Status Queries
    // ============================================================================

    FileStatus VersionControlSystem::GetFileStatus(const std::string& filePath) const
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        auto it = m_fileStatusCache.find(filePath);
        if (it != m_fileStatusCache.end())
        {
            return it->second;
        }
        return FileStatus::UP_TO_DATE;
    }

    bool VersionControlSystem::IsFileTracked(const std::string& filePath) const
    {
        FileStatus status = GetFileStatus(filePath);
        return status != FileStatus::UNTRACKED;
    }

    bool VersionControlSystem::IsFileLocked(const std::string& filePath) const
    {
        FileStatus status = GetFileStatus(filePath);
        return status == FileStatus::LOCKED;
    }

    std::vector<std::string> VersionControlSystem::GetActiveUsers() const
    {
        // Would require server integration; return empty for local git
        return {};
    }

    // ============================================================================
    // Ignore Patterns
    // ============================================================================

    bool VersionControlSystem::AddIgnorePattern(const std::string& pattern)
    {
        if (!m_repositoryInfo)
            return false;

        std::filesystem::path gitignorePath = std::filesystem::path(m_repositoryInfo->path) / ".gitignore";

        // Read existing content
        std::string content;
        if (std::filesystem::exists(gitignorePath))
        {
            std::ifstream in(gitignorePath);
            if (in.is_open())
            {
                std::ostringstream ss;
                ss << in.rdbuf();
                content = ss.str();
                in.close();
            }
        }

        // Check if pattern already exists
        std::istringstream lineStream(content);
        std::string line;
        while (std::getline(lineStream, line))
        {
            if (line == pattern)
                return true; // Already present
        }

        // Append pattern
        std::ofstream out(gitignorePath, std::ios::app);
        if (!out.is_open())
            return false;

        if (!content.empty() && content.back() != '\n')
        {
            out << "\n";
        }
        out << pattern << "\n";
        out.close();
        return true;
    }

    bool VersionControlSystem::RemoveIgnorePattern(const std::string& pattern)
    {
        if (!m_repositoryInfo)
            return false;

        std::filesystem::path gitignorePath = std::filesystem::path(m_repositoryInfo->path) / ".gitignore";
        if (!std::filesystem::exists(gitignorePath))
            return false;

        std::ifstream in(gitignorePath);
        if (!in.is_open())
            return false;

        std::vector<std::string> lines;
        std::string line;
        bool found = false;
        while (std::getline(in, line))
        {
            if (line == pattern)
            {
                found = true;
            }
            else
            {
                lines.push_back(line);
            }
        }
        in.close();

        if (!found)
            return false;

        std::ofstream out(gitignorePath, std::ios::trunc);
        if (!out.is_open())
            return false;

        for (const auto& l : lines)
        {
            out << l << "\n";
        }
        out.close();
        return true;
    }

    std::vector<std::string> VersionControlSystem::GetIgnorePatterns() const
    {
        std::vector<std::string> patterns;
        if (!m_repositoryInfo)
            return patterns;

        std::filesystem::path gitignorePath = std::filesystem::path(m_repositoryInfo->path) / ".gitignore";
        if (!std::filesystem::exists(gitignorePath))
            return patterns;

        std::ifstream in(gitignorePath);
        if (!in.is_open())
            return patterns;

        std::string line;
        while (std::getline(in, line))
        {
            // Skip empty lines and comments
            if (!line.empty() && line[0] != '#')
            {
                patterns.push_back(line);
            }
        }
        return patterns;
    }

} // namespace SparkEditor
