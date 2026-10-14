/**
 * @file VersionControlRender.cpp
 * @brief UI rendering methods, operation queue processing, and command execution
 */

#include "VersionControlSystem.h"
#include "../Core/EditorIcons.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"
#include "Utils/Process.h"
#include "Utils/LogMacros.h"
#include <imgui.h>
#include <cstdlib>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#include <shlobj.h>
#endif

namespace SparkEditor
{
    namespace
    {
#ifndef _WIN32
        bool RunFolderPicker(const std::string& executable, const std::vector<std::string>& arguments,
                             std::string& selectedPath)
        {
            Spark::Process::Builder builder(executable);
            for (const auto& argument : arguments)
                builder.Arg(argument);
            builder.CaptureStdout();

            auto launched = builder.Launch();
            if (!launched)
                return false;

            auto process = std::move(*launched);
            selectedPath = process.ReadAllStdout();
            const int status = process.WaitForExit();
            while (!selectedPath.empty() && (selectedPath.back() == '\n' || selectedPath.back() == '\r'))
                selectedPath.pop_back();
            return status == 0 && !selectedPath.empty() && std::filesystem::is_directory(selectedPath);
        }
#endif

        /// Shared empty state of every tab that needs an open repository.
        void NoRepositoryState()
        {
            Spark::Ui::EmptyState(ICON_FA_CODE_BRANCH, "No repository open",
                                  "Open a repository from the Overview tab.");
        }

        /// Text role of a change's status letter; the letter ([M], [A], ...) carries the meaning.
        Spark::Ui::Role ChangeStatusRole(FileStatus status)
        {
            switch (status)
            {
            case FileStatus::MODIFIED:
                return Spark::Ui::Role::WarningText;
            case FileStatus::ADDED:
                return Spark::Ui::Role::SuccessText;
            case FileStatus::DELETED:
            case FileStatus::CONFLICTED:
                return Spark::Ui::Role::ErrorText;
            case FileStatus::RENAMED:
                return Spark::Ui::Role::InfoText;
            case FileStatus::UNTRACKED:
                return Spark::Ui::Role::TextSecondary;
            default:
                return Spark::Ui::Role::TextPrimary;
            }
        }
    } // namespace

    // ============================================================================
    // UI Rendering
    // ============================================================================

    void VersionControlSystem::RenderRepositoryOverview()
    {
        if (!m_repositoryInfo)
        {
            if (Spark::Ui::EmptyState(ICON_FA_CODE_BRANCH, "No repository open",
                                      "Open a Git working copy to see its status, history and branches.",
                                      ICON_FA_FOLDER_OPEN " Open Repository..."))
            {
                std::string selectedPath;
                bool selected = false;
#ifdef _WIN32
                BROWSEINFOA bi = {};
                bi.lpszTitle = "Select Repository Folder";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
                if (pidl)
                {
                    char path[MAX_PATH];
                    if (SHGetPathFromIDListA(pidl, path))
                    {
                        selectedPath = path;
                        selected = true;
                    }
                    CoTaskMemFree(pidl);
                }
#else
                std::vector<std::string> zenityArgs = {"--file-selection", "--directory", "--title",
                                                       "Select Repository"};
                selected = RunFolderPicker("zenity", zenityArgs, selectedPath);
                if (!selected)
                {
                    const char* home = std::getenv("HOME");
                    selected = RunFolderPicker("kdialog", {"--getexistingdirectory", home ? home : "."}, selectedPath);
                }
#endif
                if (selected)
                    OpenRepository(selectedPath);
            }
            return;
        }

        if (Spark::Ui::BeginPropertyGrid("RepositoryOverview"))
        {
            Spark::Ui::PropertyLabel("Repository Path");
            Spark::Ui::MonoText("%s", m_repositoryInfo->path.c_str());

            Spark::Ui::PropertyLabel("Current Branch");
            Spark::Ui::TextRole(Spark::Ui::Role::AccentText, ICON_FA_CODE_BRANCH " %s",
                                m_repositoryInfo->currentBranch.name.c_str());

            Spark::Ui::PropertyLabel("Remote URL");
            ImGui::PushTextWrapPos(0.0f);
            Spark::Ui::MonoText("%s",
                                m_repositoryInfo->remoteURL.empty() ? "(none)" : m_repositoryInfo->remoteURL.c_str());
            ImGui::PopTextWrapPos();

            Spark::Ui::PropertyLabel("Status");
            if (m_repositoryInfo->isClean)
            {
                Spark::Ui::StatusText(Spark::Ui::Status::Success, "Clean");
            }
            else
            {
                Spark::Ui::StatusText(Spark::Ui::Status::Warning, "Dirty (%zu changed files)",
                                      m_repositoryInfo->changedFiles.size());
            }

            if (m_repositoryInfo->hasLFS)
            {
                Spark::Ui::PropertyLabel("LFS");
                Spark::Ui::MonoText("%s", m_repositoryInfo->lfsVersion.c_str());
            }
            Spark::Ui::EndPropertyGrid();
        }

        ImGui::Separator();
        if (Spark::Ui::SecondaryButton(ICON_FA_SYNC_ALT " Refresh"))
        {
            RefreshStatus();
        }
        ImGui::SameLine();
        if (Spark::Ui::SecondaryButton(ICON_FA_DOWNLOAD " Fetch"))
        {
            Fetch();
        }
        ImGui::SameLine();
        if (Spark::Ui::SecondaryButton(ICON_FA_ARROW_DOWN " Pull"))
        {
            Pull();
        }
        ImGui::SameLine();
        if (Spark::Ui::SecondaryButton(ICON_FA_ARROW_UP " Push"))
        {
            Push();
        }
    }

    void VersionControlSystem::RenderChangesPanel()
    {
        if (!m_repositoryInfo)
        {
            NoRepositoryState();
            return;
        }

        Spark::Ui::SubHeader("Changed Files");
        if (m_repositoryInfo->changedFiles.empty())
            Spark::Ui::SecondaryText("Working tree clean.");

        // Track staging selection per file
        static std::unordered_map<std::string, bool> selectedFiles;

        for (auto& change : m_repositoryInfo->changedFiles)
        {
            bool& selected = selectedFiles[change.filePath];

            const char* statusLabel = "";
            switch (change.status)
            {
            case FileStatus::MODIFIED:
                statusLabel = "[M]";
                break;
            case FileStatus::ADDED:
                statusLabel = "[A]";
                break;
            case FileStatus::DELETED:
                statusLabel = "[D]";
                break;
            case FileStatus::RENAMED:
                statusLabel = "[R]";
                break;
            case FileStatus::UNTRACKED:
                statusLabel = "[?]";
                break;
            case FileStatus::CONFLICTED:
                statusLabel = "[C]";
                break;
            default:
                statusLabel = "[ ]";
                break;
            }

            ImGui::Checkbox(("##stage_" + change.filePath).c_str(), &selected);
            ImGui::SameLine();
            {
                Spark::Ui::ScopedRoleColor statusColor(ImGuiCol_Text, ChangeStatusRole(change.status));
                Spark::Ui::MonoText("%s", statusLabel);
            }
            ImGui::SameLine();
            Spark::Ui::MonoText("%s", change.filePath.c_str());
        }

        ImGui::Separator();

        // Stage/unstage buttons
        if (Spark::Ui::SecondaryButton(ICON_FA_PLUS " Stage Selected"))
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
        if (Spark::Ui::SecondaryButton(ICON_FA_MINUS " Unstage Selected"))
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

        Spark::Ui::SubHeader("Commit Message");

        static char commitMsgBuf[1024] = "";
        ImGui::InputTextMultiline("##commit_msg", commitMsgBuf, sizeof(commitMsgBuf), ImVec2(-1, Spark::Ui::Px(80)));

        if (Spark::Ui::PrimaryButton(ICON_FA_CHECK " Commit"))
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
            NoRepositoryState();
            return;
        }

        if (Spark::Ui::SecondaryButton(ICON_FA_SYNC_ALT " Refresh History"))
        {
            GetCommitHistory(100);
        }

        ImGui::Separator();

        if (m_commitHistory.empty())
        {
            Spark::Ui::EmptyState(ICON_FA_HISTORY, "No history loaded", "Refresh to load the last 100 commits.");
            return;
        }

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
            NoRepositoryState();
            return;
        }

        // Create branch UI
        static char newBranchName[256] = "";
        ImGui::InputText("New Branch", newBranchName, sizeof(newBranchName));
        ImGui::SameLine();
        if (Spark::Ui::SecondaryButton(ICON_FA_PLUS " Create"))
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
        if (Spark::Ui::SecondaryButton(ICON_FA_SYNC_ALT " Refresh Branches"))
        {
            VCSOperationResult result = ExecuteGit({"branch", "-a"}, m_repositoryInfo->path);
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
                Spark::Ui::TextRole(Spark::Ui::Role::AccentText, ICON_FA_CODE_BRANCH " %s (current)",
                                    branch.name.c_str());
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
                bool deleteClicked = false;
                {
                    Spark::Ui::ScopedRoleColor dangerText(ImGuiCol_Text, Spark::Ui::Role::ErrorText);
                    deleteClicked = ImGui::SmallButton("Delete");
                }
                if (deleteClicked)
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
            NoRepositoryState();
            return;
        }

        if (m_repositoryInfo->conflicts.empty())
        {
            Spark::Ui::StatusText(Spark::Ui::Status::Success, "No merge conflicts.");
            return;
        }

        Spark::Ui::StatusText(Spark::Ui::Status::Error,
                              "%zu conflict(s) detected:", m_repositoryInfo->conflicts.size());
        ImGui::Separator();

        for (auto& conflict : m_repositoryInfo->conflicts)
        {
            ImGui::PushID(conflict.filePath.c_str());

            Spark::Ui::MonoText("%s", conflict.filePath.c_str());
            ImGui::SameLine();

            if (conflict.isResolved)
            {
                Spark::Ui::StatusText(Spark::Ui::Status::Success, "Resolved: %s", conflict.resolution.c_str());
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
        Spark::Ui::SubHeader("User Information");

        static char userName[256] = "";
        static char userEmail[256] = "";
        static bool initialized = false;
        if (!initialized)
        {
            strncpy(userName, m_userInfo.name.c_str(), sizeof(userName) - 1);
            userName[sizeof(userName) - 1] = '\0';
            strncpy(userEmail, m_userInfo.email.c_str(), sizeof(userEmail) - 1);
            userEmail[sizeof(userEmail) - 1] = '\0';
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
        Spark::Ui::SubHeader("Collaboration Settings");

        ImGui::Checkbox("Real-time Sync", &m_collaborationSettings.enableRealtimeSync);
        ImGui::Checkbox("File Locking", &m_collaborationSettings.enableFileLocking);
        ImGui::Checkbox("Auto Merge", &m_collaborationSettings.enableAutoMerge);
        ImGui::Checkbox("Conflict Resolution UI", &m_collaborationSettings.enableConflictResolution);
        ImGui::Checkbox("Activity Feed", &m_collaborationSettings.enableActivityFeed);

        ImGui::SliderFloat("Auto-Sync Interval (s)", &m_collaborationSettings.autoSyncInterval, 10.0f, 600.0f);
        ImGui::Checkbox("Auto-Sync on Save", &m_collaborationSettings.autoSyncOnSave);
        ImGui::Checkbox("Auto-Sync on Idle", &m_collaborationSettings.autoSyncOnIdle);

        ImGui::Spacing();
        Spark::Ui::SubHeader("Notifications");
        ImGui::Checkbox("Notify on Conflicts", &m_collaborationSettings.notifyOnConflicts);
        ImGui::Checkbox("Notify on Updates", &m_collaborationSettings.notifyOnUpdates);
        ImGui::Checkbox("Notify on Locks", &m_collaborationSettings.notifyOnLocks);
        ImGui::Checkbox("Desktop Notifications", &m_collaborationSettings.showDesktopNotifications);

        ImGui::Spacing();
        Spark::Ui::SubHeader("LFS Settings");

        if (m_repositoryInfo && m_repositoryInfo->hasLFS)
        {
            Spark::Ui::StatusText(Spark::Ui::Status::Success, "LFS Enabled: %s", m_repositoryInfo->lfsVersion.c_str());
        }
        else
        {
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, "LFS not detected");
            if (Spark::Ui::SecondaryButton("Initialize LFS"))
            {
                InitializeLFS();
            }
        }

        Spark::Ui::SecondaryText("LFS Patterns:");
        for (const auto& pattern : m_lfsPatterns)
        {
            ImGui::BulletText("%s", pattern.c_str());
        }
    }

    // ============================================================================
    // Operation Queue and Command Execution
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

    VCSOperationResult VersionControlSystem::ExecuteGit(const std::vector<std::string>& args,
                                                        const std::string& workingDirectory)
    {
        VCSOperationResult result;
        if (args.empty())
        {
            result.success = false;
            result.errorMessage = "No git arguments supplied";
            result.exitCode = -1;
            return result;
        }

        std::string display = "git";
        for (const auto& arg : args)
        {
            display += ' ';
            display += arg;
        }
        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Executing VCS command: %s", display.c_str());

        auto startTime = std::chrono::steady_clock::now();

        Spark::Process::Builder builder("git");
        if (!workingDirectory.empty())
        {
            builder.Arg("-C").Arg(workingDirectory);
        }
        for (const auto& arg : args)
            builder.Arg(arg);
        builder.CaptureStdout();

        auto procResult = builder.Launch();
        if (!procResult)
        {
            result.success = false;
            result.errorMessage = "Failed to execute command: " + procResult.error();
            result.exitCode = -1;
            return result;
        }

        auto proc = std::move(*procResult);
        result.output = proc.ReadAllStdout();
        result.exitCode = proc.WaitForExit();
        result.success = (result.exitCode == 0);
        if (!result.success)
        {
            result.errorMessage = result.output;
        }

        auto endTime = std::chrono::steady_clock::now();
        result.duration = std::chrono::duration<float>(endTime - startTime).count();

        return result;
    }

} // namespace SparkEditor
