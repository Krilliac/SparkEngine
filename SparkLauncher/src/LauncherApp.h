/**
 * @file LauncherApp.h
 * @brief Standalone SparkLauncher — project browser that spawns SparkEditor.
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "Core/ProjectManager.h"
#include "LauncherProcess.h"

#include <memory>
#include <string>
#include <vector>

namespace SparkLauncher
{
    struct TemplateEntry
    {
        std::string directoryName;
        std::string displayName;
        std::string description;
        std::string genre;
        std::string gameModule;
    };

    class LauncherApp
    {
      public:
        LauncherApp();
        ~LauncherApp();

        bool Initialize();
        void DrawUI();
        bool ShouldClose() const { return m_shouldClose; }

      private:
        enum class Tab
        {
            Projects,
            NewProject
        };

        void DrawProjectsTab();
        void DrawNewProjectTab();
        void LoadTemplates();
        void DefaultNewProjectLocation();
        bool SpawnTarget(const std::string& projectFilePath, LaunchTarget target);

        std::unique_ptr<SparkEditor::ProjectManager> m_projectManager;
        std::vector<TemplateEntry> m_templates;
        int m_selectedTemplate = 0;

        char m_newProjectName[256] = "MyProject";
        char m_newProjectLocation[1024] = "";
        char m_newProjectDescription[512] = "";

        std::string m_statusMessage;
        bool m_statusIsError = false;
        bool m_shouldClose = false;
        Tab m_activeTab = Tab::Projects;
    };
} // namespace SparkLauncher
