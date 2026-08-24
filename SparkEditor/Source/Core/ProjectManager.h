/**
 * @file ProjectManager.h
 * @brief Project management system for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 *
 * Handles project creation, loading, saving, and organization.
 * Projects use .sparkproject files and a standardized directory structure.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <memory>
#include <functional>
#include <cstdint>
#include <mutex>

namespace Spark
{
    class LocalFileCache;
}

namespace SparkEditor
{

    /**
 * @brief Engine version info embedded in project files
 */
    struct EngineVersion
    {
        int major = 1;
        int minor = 0;
        int patch = 0;
        std::string ToString() const;
    };

    /**
 * @brief Project template types for new project creation
 */
    enum class ProjectTemplate : uint8_t
    {
        Empty = 0,       ///< Bare project with no assets or scenes
        FirstPerson = 1, ///< FPS template with player controller and basic level
        ThirdPerson = 2, ///< Third person template
        TopDown = 3,     ///< Top-down camera template
        Blank3D = 4,     ///< Single empty scene with default lighting
        MMO = 5,         ///< Bounded local MMO sample
        Platformer = 6,  ///< Side-scrolling platformer with jump mechanics, collectibles, checkpoints
        RPG = 7          ///< RPG template with inventory, dialogue NPCs, quest system
    };

    /** @brief Immutable metadata for one built-in project template. */
    struct ProjectTemplateDescriptor
    {
        ProjectTemplate type;
        std::string_view stableId; ///< Persisted non-localized identity.
        std::string_view displayName;
        std::string_view description;
        std::string_view iconText;
        std::string_view packageDirectory; ///< Child directory of the resolved template root.
        std::string_view defaultScene;     ///< Project-relative reflected scene path.
        std::string_view genre;
        std::span<const std::string_view> features;
        bool sceneOnly = false;
    };

    /**
 * @brief Project information structure stored in .sparkproject files
 */
    struct ProjectInfo
    {
        std::string name;                 ///< Project name
        std::string path;                 ///< Project root directory
        std::string version;              ///< Project version
        std::string description;          ///< Project description
        std::string engineVersion;        ///< Engine version that created this project
        std::vector<std::string> scenes;  ///< Scene file paths (relative to project root)
        std::vector<std::string> modules; ///< Module names in this project
        std::string lastOpenedScene;      ///< Last opened scene path
        std::string defaultScene;         ///< Default scene loaded on play
        uint64_t lastModified = 0;        ///< Last modified timestamp (epoch seconds)
        uint64_t createdTime = 0;         ///< Creation timestamp (epoch seconds)
        ProjectTemplate templateType = ProjectTemplate::Blank3D;
    };

    /**
 * @brief Recent project entry for the project browser
 */
    struct RecentProject
    {
        std::string name;
        std::string path; ///< Full path to .sparkproject file
        std::string engineVersion;
        uint64_t lastOpened = 0;
        bool valid = true; ///< False if project directory no longer exists
    };

    /**
 * @brief Project management system
 *
 * Handles project creation, loading, saving, and organization for the editor.
 * Persists recent projects list and editor preferences across sessions.
 */
    class ProjectManager
    {
      public:
        ProjectManager();
        ~ProjectManager();

        bool Initialize();
        void Shutdown();

        // --- Project lifecycle ---
        bool CreateProject(const std::string& projectName, const std::string& parentDirectory,
                           ProjectTemplate templateType = ProjectTemplate::Blank3D,
                           const std::string& description = "");
        bool CreateProjectFromTemplate(const std::string& projectName, const std::string& projectPath,
                                       const std::string& templateName = "EmptyProject");
        bool OpenProject(const std::string& sparkprojectPath);
        bool SaveProject();
        /// @brief Persist a successfully opened/saved scene as project-relative state.
        /// Rejects missing files and paths outside the current project root.
        bool RecordOpenedScene(const std::string& scenePath);
        void CloseProject();

        bool HasOpenProject() const { return m_hasOpenProject; }
        const ProjectInfo& GetCurrentProject() const { return m_currentProject; }

        /// @brief Process-wide path of the project currently open in the editor.
        ///
        /// Editor panels are factory-created independently of ProjectManager, so
        /// build/cook surfaces use this read-only accessor instead of guessing
        /// the project from the process working directory.
        static std::string GetActiveProjectPath();

        void SetEngineRoot(const std::string& engineRoot) { m_engineRoot = engineRoot; }
        void SetFileCache(Spark::LocalFileCache* cache) { m_fileCache = cache; }

        // --- Recent projects (thread-safe via m_recentProjectsMutex) ---
        std::vector<RecentProject> GetRecentProjects() const
        {
            std::lock_guard<std::mutex> lock(m_recentProjectsMutex);
            return m_recentProjects;
        }
        void RefreshRecentProjects();
        void RemoveRecentProject(const std::string& path);
        void ClearRecentProjects();

        // --- Path helpers ---
        std::string GetProjectAssetsPath() const;
        std::string GetProjectScenesPath() const;
        std::string GetProjectScriptsPath() const;
        std::string GetProjectConfigPath() const;
        std::string GetProjectTempPath() const;
        std::string GetProjectFilePath() const; ///< Full path to .sparkproject file

        // --- Callbacks ---
        using ProjectCallback = std::function<void(const ProjectInfo&)>;
        void SetOnProjectOpened(ProjectCallback cb) { m_onProjectOpened = std::move(cb); }
        void SetOnProjectClosed(ProjectCallback cb) { m_onProjectClosed = std::move(cb); }

        // --- Static helpers ---
        /// [any thread, thread-safe] Static immutable built-in template catalog.
        static std::span<const ProjectTemplateDescriptor> GetProjectTemplateDescriptors() noexcept;
        /// [any thread, thread-safe] Returns nullptr for an invalid enum value.
        static const ProjectTemplateDescriptor* FindProjectTemplateDescriptor(ProjectTemplate t) noexcept;
        /// [any thread, thread-safe] Finds by stable ID or physical package directory.
        static const ProjectTemplateDescriptor* FindProjectTemplateDescriptor(std::string_view identity) noexcept;
        static std::string GetProjectTemplateName(ProjectTemplate t);
        static std::string GetProjectTemplateDescription(ProjectTemplate t);
        static std::string GetEditorDataDirectory(); ///< %APPDATA%/SparkEngine/Editor

      private:
        bool LoadProjectFile(const std::string& sparkprojectPath);
        bool SaveProjectFile();
        bool CreateProjectStructure(const std::string& projectPath, ProjectTemplate templateType);
        bool CreateDefaultScene(const std::string& scenePath, ProjectTemplate templateType);
        bool EnsureBuildScaffold(const std::string& projectPath, const std::string& projectName);
        void CreateDefaultEditorSettings(const std::string& configPath);
        bool CopyTemplate(const std::string& templatePath, const std::string& destPath, const std::string& projectName);

        void LoadRecentProjectsList();
        void SaveRecentProjectsList();
        void AddToRecentProjects(const std::string& projectName, const std::string& sparkprojectPath);

        static std::string GetRecentProjectsFilePath();

        ProjectInfo m_currentProject;
        // The project document is not required to share the display name.
        // Preserve the exact normalized file selected/created so SaveProject()
        // never silently renames a project after loading its metadata.
        std::string m_currentProjectFilePath;
        std::vector<RecentProject> m_recentProjects;
        mutable std::mutex m_recentProjectsMutex; ///< Protects m_recentProjects from concurrent access
        std::string m_engineRoot;
        bool m_hasOpenProject = false;
        bool m_isInitialized = false;

        ProjectCallback m_onProjectOpened;
        ProjectCallback m_onProjectClosed;

        Spark::LocalFileCache* m_fileCache = nullptr;

        static std::mutex s_activeProjectMutex;
        static std::string s_activeProjectPath;
    };

} // namespace SparkEditor
