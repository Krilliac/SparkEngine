/**
 * @file ProjectBrowserPanel.h
 * @brief Project browser panel for creating and opening projects
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a startup hub similar to Unity Hub / Unreal Project Browser.
 * Users can create new projects from templates, open existing projects,
 * or pick from their recent projects list.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../Core/ProjectManager.h"
#include <string>
#include <functional>
#include <utility>

namespace SparkEditor
{

    /**
 * @brief Project browser panel for creating/opening projects
 *
 * Displayed as a modal window on startup (when no project is loaded),
 * or via File > New Project / Open Project. Provides:
 * - Recent projects list
 * - New project wizard with template selection
 * - Browse for existing project
 */
    class ProjectBrowserPanel : public EditorPanel
    {
      public:
        ProjectBrowserPanel(ProjectManager* projectManager);
        ~ProjectBrowserPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        /// Show the panel in "New Project" mode
        void ShowNewProject();
        /// Show the panel in "Open Project" mode
        void ShowOpenProject();
        /// Show the full browser (recent projects + new/open buttons)
        void ShowBrowser();

        /// Returns true while the browser wants to be shown as a modal overlay
        bool IsModalActive() const { return m_showModal; }
        void SetModalActive(bool active) { m_showModal = active; }

        using OpenProjectRequestHandler = std::function<bool(const std::string&)>;
        using CreateProjectRequestHandler =
            std::function<bool(const std::string&, const std::string&, ProjectTemplate, const std::string&)>;
        void SetOpenProjectRequestHandler(OpenProjectRequestHandler handler)
        {
            m_openProjectRequestHandler = std::move(handler);
        }
        void SetCreateProjectRequestHandler(CreateProjectRequestHandler handler)
        {
            m_createProjectRequestHandler = std::move(handler);
        }

      private:
        enum class Tab
        {
            RecentProjects,
            NewProject
        };

        void RenderModal();
        void RenderRecentProjects();
        void RenderNewProjectForm();
        void RenderTemplateCard(ProjectTemplate tmpl, const char* icon);
        bool BrowseForFolder(std::string& outPath);
        bool RequestOpenProject(const std::string& projectPath);
        bool RequestCreateProject(const std::string& projectName, const std::string& parentDirectory,
                                  ProjectTemplate templateType, const std::string& description);

        ProjectManager* m_projectManager = nullptr;
        OpenProjectRequestHandler m_openProjectRequestHandler;
        CreateProjectRequestHandler m_createProjectRequestHandler;
        bool m_showModal = false;
        Tab m_activeTab = Tab::RecentProjects;

        // New project form state
        char m_newProjectName[256] = "MyProject";
        char m_newProjectPath[512] = "";
        char m_newProjectDescription[512] = "";
        ProjectTemplate m_selectedTemplate = ProjectTemplate::Blank3D;
        std::string m_createError;

        // Open project state
        std::string m_openError;
    };

} // namespace SparkEditor
