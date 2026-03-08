/**
 * @file VersionControlSystem.cpp
 * @brief Implementation of VersionControlSystem, SceneMergeHandler, MaterialMergeHandler
 */

#include "VersionControlSystem.h"
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <array>
#include <filesystem>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace SparkEditor
{

    // ============================================================================
    // SceneMergeHandler
    // ============================================================================

    std::vector<std::string> SceneMergeHandler::GetSupportedExtensions() const
    {
        return {".sparkscene"};
    }

    bool SceneMergeHandler::CanMerge(const std::string& filePath) const
    {
        std::filesystem::path p(filePath);
        std::string ext = p.extension().string();
        for (const auto& supported : GetSupportedExtensions())
        {
            if (ext == supported)
                return true;
        }
        return false;
    }

    bool SceneMergeHandler::AutoMerge(MergeConflict& /*conflict*/)
    {
        // Binary/structured scene assets require manual merge
        return false;
    }

    bool SceneMergeHandler::ShowMergeUI(MergeConflict& conflict)
    {
        bool resolved = false;
        ImGui::OpenPopup("Scene Merge Conflict");
        if (ImGui::BeginPopupModal("Scene Merge Conflict", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Conflict in scene file:");
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", conflict.filePath.c_str());
            ImGui::Separator();
            ImGui::Text("Description: %s", conflict.description.c_str());
            ImGui::Spacing();

            if (ImGui::Button("Accept Local", ImVec2(150, 0)))
            {
                conflict.resolution = "local";
                conflict.isResolved = true;
                resolved = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Accept Remote", ImVec2(150, 0)))
            {
                conflict.resolution = "remote";
                conflict.isResolved = true;
                resolved = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Manual Merge", ImVec2(150, 0)))
            {
                conflict.resolution = "manual";
                resolved = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        return resolved;
    }

    bool SceneMergeHandler::ValidateMerge(const std::string& filePath)
    {
        return std::filesystem::exists(filePath) && std::filesystem::file_size(filePath) > 0;
    }

    // ============================================================================
    // MaterialMergeHandler
    // ============================================================================

    std::vector<std::string> MaterialMergeHandler::GetSupportedExtensions() const
    {
        return {".sparkmat", ".sparkshader"};
    }

    bool MaterialMergeHandler::CanMerge(const std::string& filePath) const
    {
        std::filesystem::path p(filePath);
        std::string ext = p.extension().string();
        for (const auto& supported : GetSupportedExtensions())
        {
            if (ext == supported)
                return true;
        }
        return false;
    }

    bool MaterialMergeHandler::AutoMerge(MergeConflict& /*conflict*/)
    {
        // Binary/structured material assets require manual merge
        return false;
    }

    bool MaterialMergeHandler::ShowMergeUI(MergeConflict& conflict)
    {
        bool resolved = false;
        ImGui::OpenPopup("Material Merge Conflict");
        if (ImGui::BeginPopupModal("Material Merge Conflict", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Conflict in material/shader file:");
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", conflict.filePath.c_str());
            ImGui::Separator();
            ImGui::Text("Description: %s", conflict.description.c_str());
            ImGui::Spacing();

            if (ImGui::Button("Accept Local", ImVec2(150, 0)))
            {
                conflict.resolution = "local";
                conflict.isResolved = true;
                resolved = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Accept Remote", ImVec2(150, 0)))
            {
                conflict.resolution = "remote";
                conflict.isResolved = true;
                resolved = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Manual Merge", ImVec2(150, 0)))
            {
                conflict.resolution = "manual";
                resolved = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        return resolved;
    }

    bool MaterialMergeHandler::ValidateMerge(const std::string& filePath)
    {
        return std::filesystem::exists(filePath) && std::filesystem::file_size(filePath) > 0;
    }

    // ============================================================================
    // VersionControlSystem
    // ============================================================================

    VersionControlSystem::VersionControlSystem() : EditorPanel("Version Control", "version_control") {}

    VersionControlSystem::~VersionControlSystem()
    {
        Shutdown();
    }

    bool VersionControlSystem::Initialize()
    {
        m_isEnabled = true;

        // Register default merge handlers
        RegisterMergeHandler(std::make_unique<SceneMergeHandler>());
        RegisterMergeHandler(std::make_unique<MaterialMergeHandler>());

        // Default LFS patterns for common binary asset types
        m_lfsPatterns = {"*.png", "*.jpg",   "*.jpeg",       "*.tga",      "*.bmp",        "*.psd", "*.fbx",
                         "*.obj", "*.blend", "*.dae",        "*.wav",      "*.mp3",        "*.ogg", "*.mp4",
                         "*.avi", "*.mov",   "*.sparkscene", "*.sparkmat", "*.sparkshader"};

        // Detect git availability
        VCSOperationResult gitCheck = ExecuteCommand("git --version");
        if (!gitCheck.success)
        {
            m_isEnabled = false;
            return false;
        }

        m_lastAutoSync = std::chrono::steady_clock::now();
        m_lastStatusUpdate = std::chrono::steady_clock::now();
        m_isInitialized = true;
        return true;
    }

    void VersionControlSystem::Shutdown()
    {
        // Signal threads to stop
        m_shouldStopOperations.store(true);
        m_shouldStopWatcher.store(true);
        m_operationCondition.notify_all();

        // Join threads
        if (m_operationThread.joinable())
        {
            m_operationThread.join();
        }
        if (m_fileWatcherThread.joinable())
        {
            m_fileWatcherThread.join();
        }

        m_repositoryInfo.reset();
        m_mergeHandlers.clear();
        m_fileStatusCache.clear();
        m_isEnabled = false;
    }

    void VersionControlSystem::Update(float deltaTime)
    {
        if (!m_isEnabled)
            return;

        ProcessOperationQueue();
        UpdateFileSystemWatcher();
        AutoSync();
    }

    void VersionControlSystem::Render()
    {
        if (!m_isVisible || !m_isEnabled)
            return;

        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        if (ImGui::BeginTabBar("VCSTabBar"))
        {
            if (ImGui::BeginTabItem("Repository"))
            {
                RenderRepositoryOverview();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Changes"))
            {
                RenderChangesPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("History"))
            {
                RenderHistoryPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Branches"))
            {
                RenderBranchesPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Conflicts"))
            {
                RenderConflictsPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings"))
            {
                RenderSettingsPanel();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        EndPanel();
    }

    bool VersionControlSystem::HandleEvent(const std::string& eventType, void* eventData)
    {
        if (eventType == "file_saved" && m_collaborationSettings.autoSyncOnSave)
        {
            RefreshStatus();
            return true;
        }
        if (eventType == "refresh_status")
        {
            RefreshStatus();
            return true;
        }
        return false;
    }

    // ============================================================================
    // Repository Operations
    // ============================================================================

    VCSOperationResult VersionControlSystem::InitializeRepository(const std::string& directoryPath, VCSType vcsType)
    {
        m_vcsType = vcsType;
        VCSOperationResult result = ExecuteCommand("git init", directoryPath);
        if (result.success)
        {
            InitializeLFS();
            OpenRepository(directoryPath);
        }
        return result;
    }

    VCSOperationResult VersionControlSystem::CloneRepository(const std::string& repositoryURL,
                                                             const std::string& localPath,
                                                             std::function<void(float)> progressCallback)
    {
        std::string cmd = "git clone \"" + repositoryURL + "\" \"" + localPath + "\" --progress";
        if (progressCallback)
            progressCallback(0.0f);
        VCSOperationResult result = ExecuteCommand(cmd);
        if (progressCallback)
            progressCallback(1.0f);
        if (result.success)
        {
            OpenRepository(localPath);
        }
        return result;
    }

    bool VersionControlSystem::OpenRepository(const std::string& repositoryPath)
    {
        std::filesystem::path gitDir = std::filesystem::path(repositoryPath) / ".git";
        if (!std::filesystem::exists(gitDir))
        {
            return false;
        }

        m_repositoryInfo = std::make_unique<RepositoryInfo>();
        m_repositoryInfo->path = repositoryPath;
        m_repositoryInfo->type = VCSType::GIT;

        // Get remote URL
        VCSOperationResult remoteResult = ExecuteCommand("git remote get-url origin", repositoryPath);
        if (remoteResult.success)
        {
            std::string url = remoteResult.output;
            // Trim whitespace/newline
            url.erase(url.find_last_not_of(" \n\r\t") + 1);
            m_repositoryInfo->remoteURL = url;
        }

        // Get current branch
        VCSOperationResult branchResult = ExecuteCommand("git branch --show-current", repositoryPath);
        if (branchResult.success)
        {
            std::string branch = branchResult.output;
            branch.erase(branch.find_last_not_of(" \n\r\t") + 1);
            m_repositoryInfo->currentBranch.name = branch;
            m_repositoryInfo->currentBranch.isCurrent = true;
        }

        // Check LFS
        VCSOperationResult lfsResult = ExecuteCommand("git lfs version", repositoryPath);
        m_repositoryInfo->hasLFS = lfsResult.success;
        if (lfsResult.success)
        {
            std::string ver = lfsResult.output;
            ver.erase(ver.find_last_not_of(" \n\r\t") + 1);
            m_repositoryInfo->lfsVersion = ver;
        }

        // Refresh status
        RefreshStatus();

        return true;
    }

    void VersionControlSystem::CloseRepository()
    {
        m_repositoryInfo.reset();
        m_fileStatusCache.clear();
        m_commitHistory.clear();
        m_stagedFiles.clear();
        m_commitMessage.clear();
        m_commitDescription.clear();
        m_selectedCommit.clear();
    }

    const RepositoryInfo* VersionControlSystem::GetRepositoryInfo() const
    {
        return m_repositoryInfo.get();
    }

    void VersionControlSystem::RefreshStatus(std::function<void()> callback)
    {
        if (!m_repositoryInfo)
            return;

        VCSOperationResult result = ExecuteCommand("git status --porcelain", m_repositoryInfo->path);
        if (result.success)
        {
            auto changes = ParseGitStatus(result.output);
            m_repositoryInfo->changedFiles = changes;
            m_repositoryInfo->hasUncommittedChanges = !changes.empty();
            m_repositoryInfo->isClean = changes.empty();

            // Update status cache
            {
                std::lock_guard<std::mutex> lock(m_statusMutex);
                m_fileStatusCache.clear();
                for (const auto& change : changes)
                {
                    m_fileStatusCache[change.filePath] = change.status;
                }
            }

            // Detect merge conflicts
            DetectMergeConflicts();
        }

        m_lastStatusUpdate = std::chrono::steady_clock::now();

        if (callback)
        {
            callback();
        }
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

    // ============================================================================
    // Private Render Methods
    // ============================================================================

    void VersionControlSystem::RenderRepositoryOverview()
    {
        if (!m_repositoryInfo)
        {
            ImGui::Text("No repository open.");
            if (ImGui::Button("Open Repository..."))
            {
                // Placeholder: would open a file dialog
            }
            return;
        }

        ImGui::Text("Repository Path:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", m_repositoryInfo->path.c_str());

        ImGui::Text("Current Branch:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", m_repositoryInfo->currentBranch.name.c_str());

        ImGui::Text("Remote URL:");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", m_repositoryInfo->remoteURL.empty() ? "(none)" : m_repositoryInfo->remoteURL.c_str());

        ImGui::Text("Status:");
        ImGui::SameLine();
        if (m_repositoryInfo->isClean)
        {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Clean");
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "Dirty (%zu changed files)",
                               m_repositoryInfo->changedFiles.size());
        }

        if (m_repositoryInfo->hasLFS)
        {
            ImGui::Text("LFS:");
            ImGui::SameLine();
            ImGui::Text("%s", m_repositoryInfo->lfsVersion.c_str());
        }

        ImGui::Separator();
        if (ImGui::Button("Refresh"))
        {
            RefreshStatus();
        }
        ImGui::SameLine();
        if (ImGui::Button("Fetch"))
        {
            Fetch();
        }
        ImGui::SameLine();
        if (ImGui::Button("Pull"))
        {
            Pull();
        }
        ImGui::SameLine();
        if (ImGui::Button("Push"))
        {
            Push();
        }
    }

    void VersionControlSystem::RenderChangesPanel()
    {
        if (!m_repositoryInfo)
        {
            ImGui::Text("No repository open.");
            return;
        }

        ImGui::Text("Changed Files:");
        ImGui::Separator();

        // Track staging selection per file
        static std::unordered_map<std::string, bool> selectedFiles;

        for (auto& change : m_repositoryInfo->changedFiles)
        {
            bool& selected = selectedFiles[change.filePath];

            const char* statusLabel = "";
            ImVec4 statusColor(1.0f, 1.0f, 1.0f, 1.0f);
            switch (change.status)
            {
            case FileStatus::MODIFIED:
                statusLabel = "[M]";
                statusColor = ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
                break;
            case FileStatus::ADDED:
                statusLabel = "[A]";
                statusColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
                break;
            case FileStatus::DELETED:
                statusLabel = "[D]";
                statusColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
                break;
            case FileStatus::RENAMED:
                statusLabel = "[R]";
                statusColor = ImVec4(0.5f, 0.5f, 1.0f, 1.0f);
                break;
            case FileStatus::UNTRACKED:
                statusLabel = "[?]";
                statusColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
                break;
            case FileStatus::CONFLICTED:
                statusLabel = "[C]";
                statusColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
                break;
            default:
                statusLabel = "[ ]";
                break;
            }

            ImGui::Checkbox(("##stage_" + change.filePath).c_str(), &selected);
            ImGui::SameLine();
            ImGui::TextColored(statusColor, "%s", statusLabel);
            ImGui::SameLine();
            ImGui::Text("%s", change.filePath.c_str());
        }

        ImGui::Separator();

        // Stage/unstage buttons
        if (ImGui::Button("Stage Selected"))
        {
            std::vector<std::string> toStage;
            for (auto& [path, sel] : selectedFiles)
            {
                if (sel)
                    toStage.push_back(path);
            }
            if (!toStage.empty())
            {
                StageFiles(toStage);
                for (auto& [path, sel] : selectedFiles)
                    sel = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Unstage Selected"))
        {
            std::vector<std::string> toUnstage;
            for (auto& [path, sel] : selectedFiles)
            {
                if (sel)
                    toUnstage.push_back(path);
            }
            if (!toUnstage.empty())
            {
                UnstageFiles(toUnstage);
                for (auto& [path, sel] : selectedFiles)
                    sel = false;
            }
        }

        ImGui::Separator();
        ImGui::Text("Commit Message:");

        static char commitMsgBuf[1024] = "";
        ImGui::InputTextMultiline("##commit_msg", commitMsgBuf, sizeof(commitMsgBuf), ImVec2(-1, 80));

        if (ImGui::Button("Commit"))
        {
            std::string msg(commitMsgBuf);
            if (!msg.empty())
            {
                Commit(msg);
                commitMsgBuf[0] = '\0';
            }
        }
    }

    void VersionControlSystem::RenderHistoryPanel()
    {
        if (!m_repositoryInfo)
        {
            ImGui::Text("No repository open.");
            return;
        }

        if (ImGui::Button("Refresh History"))
        {
            GetCommitHistory(100);
        }

        ImGui::Separator();

        for (const auto& commit : m_commitHistory)
        {
            bool isSelected = (m_selectedCommit == commit.hash);
            std::string label = commit.shortHash + " - " + commit.message + " (" + commit.author + ")";
            if (ImGui::Selectable(label.c_str(), isSelected))
            {
                m_selectedCommit = commit.hash;
            }
        }
    }

    void VersionControlSystem::RenderBranchesPanel()
    {
        if (!m_repositoryInfo)
        {
            ImGui::Text("No repository open.");
            return;
        }

        // Create branch UI
        static char newBranchName[256] = "";
        ImGui::InputText("New Branch", newBranchName, sizeof(newBranchName));
        ImGui::SameLine();
        if (ImGui::Button("Create"))
        {
            std::string name(newBranchName);
            if (!name.empty())
            {
                CreateBranch(name);
                newBranchName[0] = '\0';
            }
        }

        ImGui::Separator();

        // List branches
        if (ImGui::Button("Refresh Branches"))
        {
            VCSOperationResult result = ExecuteCommand("git branch -a", m_repositoryInfo->path);
            if (result.success)
            {
                m_repositoryInfo->branches = ParseGitBranches(result.output);
            }
        }

        ImGui::Separator();

        for (const auto& branch : m_repositoryInfo->branches)
        {
            ImGui::PushID(branch.name.c_str());

            if (branch.isCurrent)
            {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "* %s", branch.name.c_str());
            }
            else
            {
                ImGui::Text("  %s", branch.name.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Switch"))
                {
                    SwitchBranch(branch.name);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Merge"))
                {
                    MergeBranch(branch.name);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete"))
                {
                    DeleteBranch(branch.name);
                }
            }

            ImGui::PopID();
        }
    }

    void VersionControlSystem::RenderConflictsPanel()
    {
        if (!m_repositoryInfo)
        {
            ImGui::Text("No repository open.");
            return;
        }

        if (m_repositoryInfo->conflicts.empty())
        {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "No merge conflicts.");
            return;
        }

        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                           "%zu conflict(s) detected:", m_repositoryInfo->conflicts.size());
        ImGui::Separator();

        for (auto& conflict : m_repositoryInfo->conflicts)
        {
            ImGui::PushID(conflict.filePath.c_str());

            ImGui::Text("%s", conflict.filePath.c_str());
            ImGui::SameLine();

            if (conflict.isResolved)
            {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "(Resolved: %s)", conflict.resolution.c_str());
            }
            else
            {
                if (ImGui::SmallButton("Accept Local"))
                {
                    ResolveMergeConflict(conflict, "local");
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Accept Remote"))
                {
                    ResolveMergeConflict(conflict, "remote");
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Manual"))
                {
                    AssetMergeHandler* handler = GetMergeHandler(conflict.filePath);
                    if (handler)
                    {
                        handler->ShowMergeUI(conflict);
                    }
                }
            }

            ImGui::PopID();
        }
    }

    void VersionControlSystem::RenderSettingsPanel()
    {
        ImGui::Text("User Information");
        ImGui::Separator();

        static char userName[256] = "";
        static char userEmail[256] = "";
        static bool initialized = false;
        if (!initialized)
        {
            strncpy(userName, m_userInfo.name.c_str(), sizeof(userName) - 1);
            strncpy(userEmail, m_userInfo.email.c_str(), sizeof(userEmail) - 1);
            initialized = true;
        }

        if (ImGui::InputText("Name", userName, sizeof(userName)))
        {
            m_userInfo.name = userName;
        }
        if (ImGui::InputText("Email", userEmail, sizeof(userEmail)))
        {
            m_userInfo.email = userEmail;
        }
        ImGui::Checkbox("Sign Commits", &m_userInfo.signCommits);

        ImGui::Spacing();
        ImGui::Text("Collaboration Settings");
        ImGui::Separator();

        ImGui::Checkbox("Real-time Sync", &m_collaborationSettings.enableRealtimeSync);
        ImGui::Checkbox("File Locking", &m_collaborationSettings.enableFileLocking);
        ImGui::Checkbox("Auto Merge", &m_collaborationSettings.enableAutoMerge);
        ImGui::Checkbox("Conflict Resolution UI", &m_collaborationSettings.enableConflictResolution);
        ImGui::Checkbox("Activity Feed", &m_collaborationSettings.enableActivityFeed);

        ImGui::SliderFloat("Auto-Sync Interval (s)", &m_collaborationSettings.autoSyncInterval, 10.0f, 600.0f);
        ImGui::Checkbox("Auto-Sync on Save", &m_collaborationSettings.autoSyncOnSave);
        ImGui::Checkbox("Auto-Sync on Idle", &m_collaborationSettings.autoSyncOnIdle);

        ImGui::Spacing();
        ImGui::Text("Notifications");
        ImGui::Separator();
        ImGui::Checkbox("Notify on Conflicts", &m_collaborationSettings.notifyOnConflicts);
        ImGui::Checkbox("Notify on Updates", &m_collaborationSettings.notifyOnUpdates);
        ImGui::Checkbox("Notify on Locks", &m_collaborationSettings.notifyOnLocks);
        ImGui::Checkbox("Desktop Notifications", &m_collaborationSettings.showDesktopNotifications);

        ImGui::Spacing();
        ImGui::Text("LFS Settings");
        ImGui::Separator();

        if (m_repositoryInfo && m_repositoryInfo->hasLFS)
        {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "LFS Enabled: %s", m_repositoryInfo->lfsVersion.c_str());
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "LFS not detected");
            if (ImGui::Button("Initialize LFS"))
            {
                InitializeLFS();
            }
        }

        ImGui::Text("LFS Patterns:");
        for (const auto& pattern : m_lfsPatterns)
        {
            ImGui::BulletText("%s", pattern.c_str());
        }
    }

    // ============================================================================
    // Private Helper Methods
    // ============================================================================

    void VersionControlSystem::ProcessOperationQueue()
    {
        std::lock_guard<std::mutex> lock(m_operationMutex);
        while (!m_operationQueue.empty())
        {
            VCSOperation op = std::move(m_operationQueue.front());
            m_operationQueue.pop();

            if (op.function)
            {
                VCSOperationResult result = op.function();
                if (op.callback)
                {
                    op.callback(result);
                }
            }
        }
    }

    VCSOperationResult VersionControlSystem::ExecuteCommand(const std::string& command,
                                                            const std::string& workingDirectory)
    {
        VCSOperationResult result;
        auto startTime = std::chrono::steady_clock::now();

        std::string fullCommand = command;
        if (!workingDirectory.empty())
        {
            fullCommand = "cd \"" + workingDirectory + "\" && " + command;
        }
        // Redirect stderr to stdout so we capture all output
        fullCommand += " 2>&1";

        FILE* pipe = popen(fullCommand.c_str(), "r");
        if (!pipe)
        {
            result.success = false;
            result.errorMessage = "Failed to execute command: " + command;
            result.exitCode = -1;
            return result;
        }

        std::array<char, 256> buffer;
        std::string output;
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
        {
            output += buffer.data();
        }

        int status = pclose(pipe);
#ifdef _WIN32
        result.exitCode = status;
#else
        result.exitCode = WEXITSTATUS(status);
#endif

        result.output = output;
        result.success = (result.exitCode == 0);
        if (!result.success)
        {
            result.errorMessage = output;
        }

        auto endTime = std::chrono::steady_clock::now();
        result.duration = std::chrono::duration<float>(endTime - startTime).count();

        return result;
    }

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

            if (line.find("COMMIT_SEP") != std::string::npos)
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
            if (line.find("->") != std::string::npos)
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

    void VersionControlSystem::DetectMergeConflicts()
    {
        if (!m_repositoryInfo)
            return;

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
