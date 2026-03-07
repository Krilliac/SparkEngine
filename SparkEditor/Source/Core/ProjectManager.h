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
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>

namespace SparkEditor {

/**
 * @brief Engine version info embedded in project files
 */
struct EngineVersion {
    int major = 1;
    int minor = 0;
    int patch = 0;
    std::string ToString() const;
};

/**
 * @brief Project template types for new project creation
 */
enum class ProjectTemplate {
    Empty,          ///< Bare project with no assets or scenes
    FirstPerson,    ///< FPS template with player controller and basic level
    ThirdPerson,    ///< Third person template
    TopDown,        ///< Top-down camera template
    Blank3D         ///< Single empty scene with default lighting
};

/**
 * @brief Project information structure stored in .sparkproject files
 */
struct ProjectInfo {
    std::string name;                ///< Project name
    std::string path;                ///< Project root directory
    std::string version;             ///< Project version
    std::string description;         ///< Project description
    std::string engineVersion;       ///< Engine version that created this project
    std::vector<std::string> scenes; ///< Scene file paths (relative to project root)
    std::vector<std::string> modules; ///< Module names in this project
    std::string lastOpenedScene;     ///< Last opened scene path
    std::string defaultScene;        ///< Default scene loaded on play
    uint64_t lastModified = 0;       ///< Last modified timestamp (epoch seconds)
    uint64_t createdTime = 0;        ///< Creation timestamp (epoch seconds)
    ProjectTemplate templateType = ProjectTemplate::Blank3D;
};

/**
 * @brief Recent project entry for the project browser
 */
struct RecentProject {
    std::string name;
    std::string path;               ///< Full path to .sparkproject file
    std::string engineVersion;
    uint64_t lastOpened = 0;
    bool valid = true;              ///< False if project directory no longer exists
};

/**
 * @brief Project management system
 *
 * Handles project creation, loading, saving, and organization for the editor.
 * Persists recent projects list and editor preferences across sessions.
 */
class ProjectManager {
public:
    ProjectManager();
    ~ProjectManager();

    bool Initialize();
    void Shutdown();

    // --- Project lifecycle ---
    bool CreateProject(const std::string& projectName, const std::string& parentDirectory,
                       ProjectTemplate templateType = ProjectTemplate::Blank3D,
                       const std::string& description = "");
    bool CreateProjectFromTemplate(const std::string& projectName,
                                   const std::string& projectPath,
                                   const std::string& templateName = "EmptyProject");
    bool OpenProject(const std::string& sparkprojectPath);
    bool SaveProject();
    void CloseProject();

    bool HasOpenProject() const { return m_hasOpenProject; }
    const ProjectInfo& GetCurrentProject() const { return m_currentProject; }

    void SetEngineRoot(const std::string& engineRoot) { m_engineRoot = engineRoot; }

    // --- Recent projects ---
    const std::vector<RecentProject>& GetRecentProjects() const { return m_recentProjects; }
    void RefreshRecentProjects();
    void RemoveRecentProject(const std::string& path);
    void ClearRecentProjects();

    // --- Path helpers ---
    std::string GetProjectAssetsPath() const;
    std::string GetProjectScenesPath() const;
    std::string GetProjectScriptsPath() const;
    std::string GetProjectConfigPath() const;
    std::string GetProjectTempPath() const;
    std::string GetProjectFilePath() const;  ///< Full path to .sparkproject file

    // --- Callbacks ---
    using ProjectCallback = std::function<void(const ProjectInfo&)>;
    void SetOnProjectOpened(ProjectCallback cb) { m_onProjectOpened = std::move(cb); }
    void SetOnProjectClosed(ProjectCallback cb) { m_onProjectClosed = std::move(cb); }

    // --- Static helpers ---
    static std::string GetProjectTemplateName(ProjectTemplate t);
    static std::string GetProjectTemplateDescription(ProjectTemplate t);
    static std::string GetEditorDataDirectory();  ///< %APPDATA%/SparkEngine/Editor

private:
    bool LoadProjectFile(const std::string& sparkprojectPath);
    bool SaveProjectFile();
    bool CreateProjectStructure(const std::string& projectPath, ProjectTemplate templateType);
    void CreateDefaultScene(const std::string& scenePath, ProjectTemplate templateType);
    void CreateDefaultEditorSettings(const std::string& configPath);
    bool CopyTemplate(const std::string& templatePath,
                      const std::string& destPath,
                      const std::string& projectName);

    void LoadRecentProjectsList();
    void SaveRecentProjectsList();
    void AddToRecentProjects(const std::string& projectName, const std::string& sparkprojectPath);

    static std::string GetRecentProjectsFilePath();

    ProjectInfo m_currentProject;
    std::vector<RecentProject> m_recentProjects;
    std::string m_engineRoot;
    bool m_hasOpenProject = false;
    bool m_isInitialized = false;

    ProjectCallback m_onProjectOpened;
    ProjectCallback m_onProjectClosed;
};

} // namespace SparkEditor
