/**
 * @file LauncherApp.h
 * @brief Standalone SparkLauncher — project browser that spawns SparkEditor.
 * @author Spark Engine Team
 * @date 2025
 *
 * LauncherApp owns the launcher's state (recent projects, templates, the New Project form, the
 * status message, the active page) and performs every action. LauncherViews draws that state.
 *
 * Thread affinity: UI thread only. Ownership: LauncherApp owns its ProjectManager; the
 * references its accessors return stay valid until the next call that refreshes that state.
 */

#pragma once

#include "Core/ProjectManager.h"
#include "LauncherProcess.h"
#include "LauncherTemplates.h"
#include "LauncherViews.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace SparkLauncher
{
    /// @brief Page shown in the content area; the left rail selects it.
    enum class LauncherView
    {
        Projects,
        NewProject
    };

    /// @brief UTF-8 text buffers of the New Project form, bound to ImGui inputs.
    struct NewProjectForm
    {
        char name[256] = "MyProject";
        char location[1024] = "";
    };

    /// @brief Live validation of the New Project form, recomputed from the buffers every frame.
    struct NewProjectValidation
    {
        std::filesystem::path projectRoot; ///< location / name; empty while either field is invalid.
        std::string nameError;             ///< Why the name cannot be used; empty when it can.
        std::string locationError;         ///< Why the location cannot be used; empty when it can.

        /// @brief True when the form can be submitted.
        [[nodiscard]] bool IsValid() const
        {
            return nameError.empty() && locationError.empty() && !projectRoot.empty();
        }
        /// @brief The message to show for a disabled Create button (name first).
        [[nodiscard]] const std::string& FirstError() const { return nameError.empty() ? locationError : nameError; }
    };

    class LauncherApp
    {
      public:
        LauncherApp();
        ~LauncherApp();

        /// @brief Initialize the ProjectManager, templates, form defaults and the recent list.
        /// @return False with StatusMessage() set when the ProjectManager cannot start.
        bool Initialize();
        /// @brief Draw one frame (refreshes the recent list when the window regains focus).
        void DrawUI();
        bool ShouldClose() const { return m_shouldClose; }

        // --- status ---
        /// @brief Latest action result; empty when there is nothing to report.
        const std::string& StatusMessage() const { return m_statusMessage; }
        bool StatusIsError() const { return m_statusIsError; }
        /// @brief When the current status message was set.
        std::chrono::steady_clock::time_point StatusTime() const { return m_statusTime; }
        /// @brief Report an action result in the status banner.
        void ReportStatus(std::string message, bool isError);
        /// @brief Remove the status banner.
        void ClearStatus();

        // --- pages ---
        LauncherView ActiveView() const { return m_activeView; }
        void SetActiveView(LauncherView view) { m_activeView = view; }
        LauncherViewState& ViewState() { return m_viewState; }
        /// @brief UTF-8 path of the directory holding the launcher (the engine install).
        const std::string& EngineDirectory() const { return m_engineDirectory; }

        // --- recent projects ---
        /// @brief Cached recent projects; refreshed by RefreshRecentProjects() only.
        const std::vector<SparkEditor::RecentProject>& RecentProjects() const { return m_recentProjects; }
        /// @brief Re-check which recent projects still exist and reload the cache.
        void RefreshRecentProjects();
        /// @brief Browse for a project folder, open it and start the editor on it.
        void OpenExistingProject();
        /// @brief Start @p target for a recent project (the editor also opens it in the ProjectManager).
        void OpenRecentProject(const SparkEditor::RecentProject& project, LaunchTarget target);
        /// @brief Browse for the new folder of a missing recent project and replace its entry.
        void LocateRecentProject(const SparkEditor::RecentProject& project);
        /// @brief Remove @p path from the recent list (files on disk are not touched).
        void RemoveRecentProject(const std::string& path);

        // --- new project ---
        const std::vector<TemplateEntry>& Templates() const { return m_templates; }
        int SelectedTemplate() const { return m_selectedTemplate; }
        /// @brief Select template @p index; out-of-range indices are ignored.
        void SelectTemplate(int index);
        NewProjectForm& Form() { return m_form; }
        /// @brief Validate the form buffers (encoding, required fields, name characters, collisions).
        [[nodiscard]] NewProjectValidation ValidateNewProject() const;
        /// @brief Browse for the parent folder of the new project.
        void BrowseNewProjectLocation();
        /// @brief Create the project from the selected template and start the editor on it.
        void CreateNewProject();

      private:
        void LoadTemplates();
        void DefaultNewProjectLocation();
        /** Build and start @p target for @p projectFile. Reports every failure in the status line; never throws. */
        bool SpawnTarget(const std::filesystem::path& projectFile, LaunchTarget target);

        std::unique_ptr<SparkEditor::ProjectManager> m_projectManager;
        std::vector<SparkEditor::RecentProject> m_recentProjects;
        std::vector<TemplateEntry> m_templates;
        int m_selectedTemplate = 0;
        NewProjectForm m_form;

        std::string m_engineDirectory;
        std::string m_statusMessage;
        bool m_statusIsError = false;
        std::chrono::steady_clock::time_point m_statusTime{};
        bool m_shouldClose = false;
        bool m_appFocusLost = false;
        LauncherView m_activeView = LauncherView::Projects;
        LauncherViewState m_viewState;
    };
} // namespace SparkLauncher
