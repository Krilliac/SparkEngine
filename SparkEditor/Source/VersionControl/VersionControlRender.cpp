/**
 * @file VersionControlRender.cpp
 * @brief UI rendering methods, operation queue processing, and command execution
 */

#include "VersionControlSystem.h"
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
    } // namespace

    // ============================================================================
    // UI Rendering
    // ============================================================================

    void VersionControlSystem::RenderRepositoryOverview()
    {
        if (!m_repositoryInfo)
        {
            ImGui::Text("No repository open.");
            if (ImGui::Button("Open Repository..."))
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
