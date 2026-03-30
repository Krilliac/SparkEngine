/**
 * @file VersionControlSystem.cpp
 * @brief Core VersionControlSystem lifecycle, merge handler implementations, and repository operations
 *
 * Split files:
 *   VersionControlGitOps.cpp     — Staging, committing, push/pull/fetch, branching, history, diffs
 *   VersionControlConflicts.cpp  — Locking, conflict resolution, user/collab settings, file status, ignore patterns
 *   VersionControlRender.cpp     — UI rendering, operation queue processing, command execution
 *   VersionControlHelpers.cpp    — Git output parsing, file system watcher, auto-sync, conflict detection, LFS
 */

#include "VersionControlSystem.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <imgui.h>
#include <filesystem>

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
    // VersionControlSystem — Lifecycle
    // ============================================================================

    VersionControlSystem::VersionControlSystem() : EditorPanel("Version Control", "version_control") {}

    VersionControlSystem::~VersionControlSystem()
    {
        Shutdown();
    }

    bool VersionControlSystem::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "VersionControlSystem initialized");
        return true;
    }

    void VersionControlSystem::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "VersionControlSystem shutting down");
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

} // namespace SparkEditor
