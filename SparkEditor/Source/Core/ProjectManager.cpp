/**
 * @file ProjectManager.cpp
 * @brief Implementation of the project management system
 * @author Spark Engine Team
 * @date 2025
 */

#include "ProjectManager.h"
#include "Utils/LocalFileCache.h"
#include "Utils/Validate.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <set>

#ifdef _WIN32
#include <ShlObj.h>
#include <Windows.h>
#endif

namespace fs = std::filesystem;

namespace SparkEditor
{

    // ------------------------------------------------------------------
    // EngineVersion
    // ------------------------------------------------------------------
    std::string EngineVersion::ToString() const
    {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    static EngineVersion GetCurrentEngineVersion()
    {
        return {EDITOR_VERSION_MAJOR, EDITOR_VERSION_MINOR, EDITOR_VERSION_PATCH};
    }

    static uint64_t GetCurrentTimestamp()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    // ------------------------------------------------------------------
    // Simple JSON helpers (write-only, no dependency)
    // ------------------------------------------------------------------
    static std::string EscapeJsonString(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s)
        {
            switch (c)
            {
            case '\"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
                break;
            }
        }
        return out;
    }

    static std::string ExtractJsonString(const std::string& json, const std::string& key)
    {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos)
            return "";
        pos = json.find(':', pos);
        if (pos == std::string::npos)
            return "";
        pos = json.find('\"', pos + 1);
        if (pos == std::string::npos)
            return "";
        size_t end = json.find('\"', pos + 1);
        if (end == std::string::npos)
            return "";
        return json.substr(pos + 1, end - pos - 1);
    }

    static uint64_t ExtractJsonUint64(const std::string& json, const std::string& key)
    {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos)
            return 0;
        pos = json.find(':', pos);
        if (pos == std::string::npos)
            return 0;
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
            pos++;
        std::string numStr;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9')
        {
            numStr += json[pos++];
        }
        if (numStr.empty())
            return 0;
        return std::stoull(numStr);
    }

    static std::vector<std::string> ExtractJsonStringArray(const std::string& json, const std::string& key)
    {
        std::vector<std::string> result;
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos)
            return result;
        pos = json.find('[', pos);
        if (pos == std::string::npos)
            return result;
        size_t end = json.find(']', pos);
        if (end == std::string::npos)
            return result;

        std::string arr = json.substr(pos + 1, end - pos - 1);
        size_t i = 0;
        while (i < arr.size())
        {
            size_t qStart = arr.find('\"', i);
            if (qStart == std::string::npos)
                break;
            size_t qEnd = arr.find('\"', qStart + 1);
            if (qEnd == std::string::npos)
                break;
            result.push_back(arr.substr(qStart + 1, qEnd - qStart - 1));
            i = qEnd + 1;
        }
        return result;
    }

    // ------------------------------------------------------------------
    // ProjectManager
    // ------------------------------------------------------------------
    ProjectManager::ProjectManager() = default;
    ProjectManager::~ProjectManager() = default;

    bool ProjectManager::Initialize()
    {
        std::cout << "ProjectManager::Initialize()\n";

        // Ensure editor data directory exists
        std::string editorDataDir = GetEditorDataDirectory();
        try
        {
            fs::create_directories(editorDataDir);
        }
        catch (const std::exception& e)
        {
            std::cerr << "Warning: Could not create editor data directory: " << e.what() << "\n";
        }

        LoadRecentProjectsList();
        m_isInitialized = true;
        return true;
    }

    void ProjectManager::Shutdown()
    {
        std::cout << "ProjectManager::Shutdown()\n";
        if (m_hasOpenProject)
        {
            SaveProject();
            CloseProject();
        }
        SaveRecentProjectsList();
        m_isInitialized = false;
    }

    // ------------------------------------------------------------------
    // Project lifecycle
    // ------------------------------------------------------------------
    bool ProjectManager::CreateProject(const std::string& projectName, const std::string& parentDirectory,
                                       ProjectTemplate templateType, const std::string& description)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !projectName.empty(), false);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !parentDirectory.empty(), false);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Creating project '%s' in '%s'", projectName.c_str(),
                       parentDirectory.c_str());
        // If engine root is set and templates exist, use template-based creation
        if (!m_engineRoot.empty())
        {
            return CreateProjectFromTemplate(projectName, (fs::path(parentDirectory) / projectName).string(),
                                             "EmptyProject");
        }

        std::cout << "Creating project '" << projectName << "' in " << parentDirectory << "\n";

        // Build the project root: parentDirectory/projectName
        std::string projectRoot = (fs::path(parentDirectory) / projectName).string();

        try
        {
            if (fs::exists(projectRoot) && !fs::is_empty(projectRoot))
            {
                std::cerr << "Project directory already exists and is not empty: " << projectRoot << "\n";
                return false;
            }

            if (!CreateProjectStructure(projectRoot, templateType))
            {
                return false;
            }

            // Fill project info
            m_currentProject = ProjectInfo{};
            m_currentProject.name = projectName;
            m_currentProject.path = projectRoot;
            m_currentProject.version = "1.0.0";
            m_currentProject.description = description.empty() ? "Spark Engine Project" : description;
            m_currentProject.engineVersion = GetCurrentEngineVersion().ToString();
            m_currentProject.createdTime = GetCurrentTimestamp();
            m_currentProject.lastModified = m_currentProject.createdTime;
            m_currentProject.templateType = templateType;
            m_currentProject.modules.push_back(projectName);

            // Add the default scene
            std::string defaultSceneRelative = "Scenes/Default.sparkscene";
            m_currentProject.scenes.push_back(defaultSceneRelative);
            m_currentProject.defaultScene = defaultSceneRelative;
            m_currentProject.lastOpenedScene = defaultSceneRelative;

            if (!SaveProjectFile())
            {
                return false;
            }

            m_hasOpenProject = true;
            AddToRecentProjects(projectName, GetProjectFilePath());

            if (m_onProjectOpened)
            {
                m_onProjectOpened(m_currentProject);
            }

            std::cout << "Project created successfully: " << projectRoot << "\n";
            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error creating project: " << e.what() << "\n";
            return false;
        }
    }

    bool ProjectManager::CreateProjectFromTemplate(const std::string& projectName, const std::string& projectPath,
                                                   const std::string& templateName)
    {
        std::cout << "Creating project '" << projectName << "' from template '" << templateName << "' at "
                  << projectPath << "\n";

        std::string templateDir = m_engineRoot + "/Templates/" + templateName;
        if (!fs::exists(templateDir))
        {
            std::cerr << "Template not found: " << templateDir << "\n";
            return false;
        }

        try
        {
            // Copy template and replace placeholders
            if (!CopyTemplate(templateDir, projectPath, projectName))
            {
                return false;
            }

            // Load the generated project settings
            if (!LoadProjectFile(projectPath + "/" + projectName + ".sparkproject"))
            {
                // Fall back: set defaults if no .sparkproject was in template
                m_currentProject = ProjectInfo{};
                m_currentProject.name = projectName;
                m_currentProject.path = projectPath;
                m_currentProject.version = "1.0.0";
                m_currentProject.engineVersion = GetCurrentEngineVersion().ToString();
                m_currentProject.modules.push_back(projectName);
            }

            m_hasOpenProject = true;
            AddToRecentProjects(projectName, projectPath);

            if (m_onProjectOpened)
            {
                m_onProjectOpened(m_currentProject);
            }

            std::cout << "Project '" << projectName << "' created successfully from template '" << templateName
                      << "'\n";
            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error creating project from template: " << e.what() << "\n";
            return false;
        }
    }

    bool ProjectManager::CopyTemplate(const std::string& templatePath, const std::string& destPath,
                                      const std::string& projectName)
    {
        try
        {
            // Copy the entire template directory
            fs::copy(templatePath, destPath, fs::copy_options::recursive | fs::copy_options::overwrite_existing);

            // Text file extensions that should have placeholders replaced
            static const std::set<std::string> textExtensions = {".h",    ".hpp",   ".cpp", ".c", ".txt",
                                                                 ".json", ".cmake", ".md",  ".py"};

            const std::string placeholder = "{{PROJECT_NAME}}";

            // Walk through all files and replace {{PROJECT_NAME}}
            for (auto& entry : fs::recursive_directory_iterator(destPath))
            {
                if (!entry.is_regular_file())
                    continue;

                auto ext = entry.path().extension().string();
                if (textExtensions.find(ext) == textExtensions.end())
                    continue;

                // Read file
                std::ifstream inFile(entry.path());
                if (!inFile.is_open())
                    continue;

                std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
                inFile.close();

                // Replace placeholders
                if (!content.contains(placeholder))
                    continue;

                size_t pos = 0;
                while ((pos = content.find(placeholder, pos)) != std::string::npos)
                {
                    content.replace(pos, placeholder.length(), projectName);
                    pos += projectName.length();
                }

                // Write back
                std::ofstream outFile(entry.path());
                if (outFile.is_open())
                {
                    outFile << content;
                    outFile.close();
                }
            }

            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error copying template: " << e.what() << "\n";
            return false;
        }
    }

    bool ProjectManager::OpenProject(const std::string& sparkprojectPath)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !sparkprojectPath.empty(), false);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Loading project from '%s'", sparkprojectPath.c_str());
        std::cout << "Opening project: " << sparkprojectPath << "\n";

        // Accept either a .sparkproject file or a directory containing one
        std::string resolvedPath = sparkprojectPath;
        if (fs::is_directory(sparkprojectPath))
        {
            // Search for .sparkproject file inside
            for (auto& entry : fs::directory_iterator(sparkprojectPath))
            {
                if (entry.path().extension() == ".sparkproject")
                {
                    resolvedPath = entry.path().string();
                    break;
                }
            }
            // Also try spark.project.json (new module format)
            if (resolvedPath == sparkprojectPath)
            {
                std::string sparkJson = sparkprojectPath + "/spark.project.json";
                if (fs::exists(sparkJson))
                {
                    resolvedPath = sparkJson;
                }
            }
        }

        if (!fs::exists(resolvedPath))
        {
            std::cerr << "Project file not found: " << resolvedPath << "\n";
            return false;
        }

        if (m_hasOpenProject)
        {
            SaveProject();
            CloseProject();
        }

        if (!LoadProjectFile(resolvedPath))
        {
            return false;
        }

        m_hasOpenProject = true;
        m_currentProject.lastModified = GetCurrentTimestamp();
        AddToRecentProjects(m_currentProject.name, resolvedPath);

        if (m_onProjectOpened)
        {
            m_onProjectOpened(m_currentProject);
        }

        return true;
    }

    bool ProjectManager::SaveProject()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (!m_hasOpenProject)
        {
            std::cerr << "No project is currently open\n";
            return false;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Saving project '%s'", m_currentProject.name.c_str());
        std::cout << "Saving project: " << m_currentProject.name << "\n";
        m_currentProject.lastModified = GetCurrentTimestamp();
        return SaveProjectFile();
    }

    void ProjectManager::CloseProject()
    {
        if (m_hasOpenProject)
        {
            std::cout << "Closing project: " << m_currentProject.name << "\n";
            ProjectInfo closed = m_currentProject;
            m_hasOpenProject = false;
            m_currentProject = ProjectInfo{};
            if (m_onProjectClosed)
            {
                m_onProjectClosed(closed);
            }
        }
    }

    // ------------------------------------------------------------------
    // Path helpers
    // ------------------------------------------------------------------
    std::string ProjectManager::GetProjectAssetsPath() const
    {
        return (fs::path(m_currentProject.path) / "Assets").string();
    }
    std::string ProjectManager::GetProjectScenesPath() const
    {
        return (fs::path(m_currentProject.path) / "Scenes").string();
    }
    std::string ProjectManager::GetProjectScriptsPath() const
    {
        return (fs::path(m_currentProject.path) / "Scripts").string();
    }
    std::string ProjectManager::GetProjectConfigPath() const
    {
        return (fs::path(m_currentProject.path) / "Config").string();
    }
    std::string ProjectManager::GetProjectTempPath() const
    {
        return (fs::path(m_currentProject.path) / "Temp").string();
    }
    std::string ProjectManager::GetProjectFilePath() const
    {
        return (fs::path(m_currentProject.path) / (m_currentProject.name + ".sparkproject")).string();
    }

    // ------------------------------------------------------------------
    // Recent projects
    // ------------------------------------------------------------------
    void ProjectManager::RefreshRecentProjects()
    {
        std::lock_guard<std::mutex> lock(m_recentProjectsMutex);
        for (auto& rp : m_recentProjects)
        {
            rp.valid = fs::exists(rp.path);
        }
    }

    void ProjectManager::RemoveRecentProject(const std::string& path)
    {
        {
            std::lock_guard<std::mutex> lock(m_recentProjectsMutex);
            m_recentProjects.erase(std::remove_if(m_recentProjects.begin(), m_recentProjects.end(),
                                                  [&](const RecentProject& rp) { return rp.path == path; }),
                                   m_recentProjects.end());
        }
        SaveRecentProjectsList();
    }

    void ProjectManager::ClearRecentProjects()
    {
        {
            std::lock_guard<std::mutex> lock(m_recentProjectsMutex);
            m_recentProjects.clear();
        }
        SaveRecentProjectsList();
    }

    void ProjectManager::AddToRecentProjects(const std::string& projectName, const std::string& sparkprojectPath)
    {
        {
            std::lock_guard<std::mutex> lock(m_recentProjectsMutex);
            // Remove existing entry with same path
            m_recentProjects.erase(std::remove_if(m_recentProjects.begin(), m_recentProjects.end(),
                                                  [&](const RecentProject& rp) { return rp.path == sparkprojectPath; }),
                                   m_recentProjects.end());

            RecentProject rp;
            rp.name = projectName;
            rp.path = sparkprojectPath;
            rp.engineVersion = GetCurrentEngineVersion().ToString();
            rp.lastOpened = GetCurrentTimestamp();
            rp.valid = true;

            m_recentProjects.insert(m_recentProjects.begin(), rp);

            // Keep max 10
            if (m_recentProjects.size() > 10)
            {
                m_recentProjects.resize(10);
            }
        }
        SaveRecentProjectsList();
    }

    // ------------------------------------------------------------------
    // Project file I/O (.sparkproject)
    // ------------------------------------------------------------------
    bool ProjectManager::LoadProjectFile(const std::string& sparkprojectPath)
    {
        std::string content;

        if (m_fileCache)
        {
            auto result = m_fileCache->ReadText(sparkprojectPath);
            if (result.IsOk())
            {
                content = result.Value();
            }
        }

        if (content.empty())
        {
            std::ifstream file(sparkprojectPath);
            if (!file.is_open())
            {
                std::cerr << "Could not open project file: " << sparkprojectPath << "\n";
                return false;
            }
            content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            file.close();
        }

        std::string name = ExtractJsonString(content, "name");
        std::string version = ExtractJsonString(content, "version");
        std::string description = ExtractJsonString(content, "description");
        std::string engineVer = ExtractJsonString(content, "engineVersion");
        std::string defaultScene = ExtractJsonString(content, "defaultScene");
        std::string lastScene = ExtractJsonString(content, "lastOpenedScene");
        uint64_t lastModified = ExtractJsonUint64(content, "lastModified");
        uint64_t createdTime = ExtractJsonUint64(content, "createdTime");
        auto scenes = ExtractJsonStringArray(content, "scenes");
        auto modules = ExtractJsonStringArray(content, "modules");

        // Derive project root from the .sparkproject file location
        std::string projectRoot = fs::path(sparkprojectPath).parent_path().string();

        m_currentProject = ProjectInfo{};
        m_currentProject.name = name.empty() ? fs::path(projectRoot).filename().string() : name;
        m_currentProject.path = projectRoot;
        m_currentProject.version = version.empty() ? "1.0.0" : version;
        m_currentProject.description = description.empty() ? "Spark Engine Project" : description;
        m_currentProject.engineVersion = engineVer.empty() ? GetCurrentEngineVersion().ToString() : engineVer;
        m_currentProject.defaultScene = defaultScene;
        m_currentProject.lastOpenedScene = lastScene;
        m_currentProject.lastModified = lastModified;
        m_currentProject.createdTime = createdTime;
        m_currentProject.scenes = scenes;
        m_currentProject.modules = modules;

        std::cout << "Loaded project: " << m_currentProject.name << " v" << m_currentProject.version << " (engine "
                  << m_currentProject.engineVersion << ")\n";
        return true;
    }

    bool ProjectManager::SaveProjectFile()
    {
        std::string filePath = GetProjectFilePath();

        try
        {
            // Ensure directory exists
            fs::create_directories(fs::path(filePath).parent_path());

            std::ofstream file(filePath);
            if (!file.is_open())
            {
                std::cerr << "Failed to open project file for writing: " << filePath << "\n";
                return false;
            }

            file << "{\n";
            file << "  \"projectFileVersion\": 1,\n";
            file << "  \"name\": \"" << EscapeJsonString(m_currentProject.name) << "\",\n";
            file << "  \"version\": \"" << EscapeJsonString(m_currentProject.version) << "\",\n";
            file << "  \"description\": \"" << EscapeJsonString(m_currentProject.description) << "\",\n";
            file << "  \"engineVersion\": \"" << EscapeJsonString(m_currentProject.engineVersion) << "\",\n";
            file << "  \"defaultScene\": \"" << EscapeJsonString(m_currentProject.defaultScene) << "\",\n";
            file << "  \"lastOpenedScene\": \"" << EscapeJsonString(m_currentProject.lastOpenedScene) << "\",\n";
            file << "  \"createdTime\": " << m_currentProject.createdTime << ",\n";
            file << "  \"lastModified\": " << m_currentProject.lastModified << ",\n";

            // modules array
            file << "  \"modules\": [\n";
            for (size_t i = 0; i < m_currentProject.modules.size(); ++i)
            {
                file << "    \"" << EscapeJsonString(m_currentProject.modules[i]) << "\"";
                if (i + 1 < m_currentProject.modules.size())
                    file << ",";
                file << "\n";
            }
            file << "  ],\n";

            // scenes array
            file << "  \"scenes\": [\n";
            for (size_t i = 0; i < m_currentProject.scenes.size(); ++i)
            {
                file << "    \"" << EscapeJsonString(m_currentProject.scenes[i]) << "\"";
                if (i + 1 < m_currentProject.scenes.size())
                    file << ",";
                file << "\n";
            }
            file << "  ]\n";
            file << "}\n";

            file.close();

            if (m_fileCache)
            {
                m_fileCache->Invalidate(filePath);
            }

            std::cout << "Project file saved: " << filePath << "\n";
            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error saving project file: " << e.what() << "\n";
            return false;
        }
    }

    // ------------------------------------------------------------------
    // Project structure creation
    // ------------------------------------------------------------------
    bool ProjectManager::CreateProjectStructure(const std::string& projectPath, ProjectTemplate templateType)
    {
        try
        {
            fs::create_directories(projectPath);

            // Core directories every project needs
            fs::create_directories(projectPath + "/Source");
            fs::create_directories(projectPath + "/Assets");
            fs::create_directories(projectPath + "/Assets/Textures");
            fs::create_directories(projectPath + "/Assets/Models");
            fs::create_directories(projectPath + "/Assets/Materials");
            fs::create_directories(projectPath + "/Assets/Audio");
            fs::create_directories(projectPath + "/Assets/Fonts");
            fs::create_directories(projectPath + "/Assets/Prefabs");
            fs::create_directories(projectPath + "/Scenes");
            fs::create_directories(projectPath + "/Scripts");
            fs::create_directories(projectPath + "/Config");
            fs::create_directories(projectPath + "/Temp");
            fs::create_directories(projectPath + "/Saved");
            fs::create_directories(projectPath + "/Saved/Backups");

            // Template-specific directories
            if (templateType == ProjectTemplate::FirstPerson || templateType == ProjectTemplate::ThirdPerson)
            {
                fs::create_directories(projectPath + "/Assets/Characters");
                fs::create_directories(projectPath + "/Assets/Weapons");
                fs::create_directories(projectPath + "/Assets/UI");
            }

            // Create default scene
            CreateDefaultScene(projectPath + "/Scenes/Default.sparkscene", templateType);

            // Create editor settings
            CreateDefaultEditorSettings(projectPath + "/Config");

            // Create .gitignore for the project
            {
                std::ofstream gitignore(projectPath + "/.gitignore");
                if (gitignore.is_open())
                {
                    gitignore << "# Spark Engine Project\n";
                    gitignore << "Temp/\n";
                    gitignore << "Saved/Backups/\n";
                    gitignore << "*.log\n";
                    gitignore << ".vs/\n";
                    gitignore << "build/\n";
                    gitignore.close();
                }
            }

            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error creating project structure: " << e.what() << "\n";
            return false;
        }
    }

    void ProjectManager::CreateDefaultScene(const std::string& scenePath, ProjectTemplate templateType)
    {
        std::ofstream file(scenePath);
        if (!file.is_open())
            return;

        file << "{\n";
        file << "  \"sceneVersion\": 1,\n";
        file << "  \"name\": \"Default\",\n";
        file << "  \"entities\": [\n";

        // Every template gets a directional light
        file << "    {\n";
        file << "      \"name\": \"Directional Light\",\n";
        file << "      \"components\": [\n";
        file << "        {\n";
        file << "          \"type\": \"Transform\",\n";
        file << "          \"position\": [0, 10, 0],\n";
        file << "          \"rotation\": [50, -30, 0],\n";
        file << "          \"scale\": [1, 1, 1]\n";
        file << "        },\n";
        file << "        {\n";
        file << "          \"type\": \"DirectionalLight\",\n";
        file << "          \"color\": [1.0, 0.96, 0.84],\n";
        file << "          \"intensity\": 1.0\n";
        file << "        }\n";
        file << "      ]\n";
        file << "    }";

        // Templates with a ground plane
        if (templateType != ProjectTemplate::Empty)
        {
            file << ",\n";
            file << "    {\n";
            file << "      \"name\": \"Ground\",\n";
            file << "      \"components\": [\n";
            file << "        {\n";
            file << "          \"type\": \"Transform\",\n";
            file << "          \"position\": [0, 0, 0],\n";
            file << "          \"rotation\": [0, 0, 0],\n";
            file << "          \"scale\": [50, 1, 50]\n";
            file << "        },\n";
            file << "        { \"type\": \"MeshRenderer\", \"mesh\": \"Plane\" }\n";
            file << "      ]\n";
            file << "    }";
        }

        // Camera
        if (templateType != ProjectTemplate::Empty)
        {
            file << ",\n";
            file << "    {\n";
            file << "      \"name\": \"Main Camera\",\n";
            file << "      \"components\": [\n";
            file << "        {\n";
            file << "          \"type\": \"Transform\",\n";

            switch (templateType)
            {
            case ProjectTemplate::FirstPerson:
                file << "          \"position\": [0, 1.7, 0],\n";
                break;
            case ProjectTemplate::ThirdPerson:
                file << "          \"position\": [0, 5, -8],\n";
                file << "          \"rotation\": [25, 0, 0],\n";
                break;
            case ProjectTemplate::TopDown:
                file << "          \"position\": [0, 20, 0],\n";
                file << "          \"rotation\": [90, 0, 0],\n";
                break;
            default:
                file << "          \"position\": [0, 3, -8],\n";
                file << "          \"rotation\": [15, 0, 0],\n";
                break;
            }
            if (templateType == ProjectTemplate::FirstPerson || templateType == ProjectTemplate::Blank3D)
            {
                // Only write scale if rotation wasn't already written above for some templates
            }
            file << "          \"scale\": [1, 1, 1]\n";
            file << "        },\n";
            file << "        { \"type\": \"Camera\", \"fov\": 60.0, \"nearClip\": 0.1, \"farClip\": 1000.0 }\n";
            file << "      ]\n";
            file << "    }";
        }

        // Player for FPS/TPS templates
        if (templateType == ProjectTemplate::FirstPerson || templateType == ProjectTemplate::ThirdPerson)
        {
            file << ",\n";
            file << "    {\n";
            file << "      \"name\": \"Player\",\n";
            file << "      \"components\": [\n";
            file << "        {\n";
            file << "          \"type\": \"Transform\",\n";
            file << "          \"position\": [0, 1, 0],\n";
            file << "          \"rotation\": [0, 0, 0],\n";
            file << "          \"scale\": [1, 1, 1]\n";
            file << "        },\n";
            file << "        { \"type\": \"CharacterController\", \"height\": 1.8, \"radius\": 0.3 }\n";
            file << "      ]\n";
            file << "    }";
        }

        file << "\n  ]\n";
        file << "}\n";
        file.close();
    }

    void ProjectManager::CreateDefaultEditorSettings(const std::string& configPath)
    {
        std::ofstream file(configPath + "/EditorSettings.json");
        if (!file.is_open())
            return;

        file << "{\n";
        file << "  \"editor\": {\n";
        file << "    \"theme\": \"Spark Professional\",\n";
        file << "    \"fontSize\": 15,\n";
        file << "    \"autoSaveInterval\": 300,\n";
        file << "    \"showGrid\": true,\n";
        file << "    \"gridSize\": 1.0,\n";
        file << "    \"snapToGrid\": false\n";
        file << "  },\n";
        file << "  \"viewport\": {\n";
        file << "    \"fieldOfView\": 60.0,\n";
        file << "    \"nearClip\": 0.1,\n";
        file << "    \"farClip\": 5000.0,\n";
        file << "    \"moveSpeed\": 10.0,\n";
        file << "    \"showGizmos\": true\n";
        file << "  },\n";
        file << "  \"build\": {\n";
        file << "    \"targetPlatform\": \"Windows\",\n";
        file << "    \"configuration\": \"Debug\"\n";
        file << "  }\n";
        file << "}\n";
        file.close();
    }

    // ------------------------------------------------------------------
    // Recent projects persistence
    // ------------------------------------------------------------------
    std::string ProjectManager::GetEditorDataDirectory()
    {
#ifdef _WIN32
        char appDataPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath)))
        {
            return std::string(appDataPath) + "\\SparkEngine\\Editor";
        }
        return "./SparkEditor";
#else
        const char* home = getenv("HOME");
        if (home)
        {
            return std::string(home) + "/.sparkengine/editor";
        }
        return "./.sparkengine/editor";
#endif
    }

    std::string ProjectManager::GetRecentProjectsFilePath()
    {
        return (fs::path(GetEditorDataDirectory()) / "RecentProjects.json").string();
    }

    void ProjectManager::LoadRecentProjectsList()
    {
        std::lock_guard<std::mutex> lock(m_recentProjectsMutex);
        m_recentProjects.clear();
        std::string filePath = GetRecentProjectsFilePath();

        std::string content;

        if (m_fileCache)
        {
            auto result = m_fileCache->ReadText(filePath);
            if (result.IsOk())
            {
                content = result.Value();
            }
        }

        if (content.empty())
        {
            std::ifstream file(filePath);
            if (!file.is_open())
                return;
            content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            file.close();
        }

        // Parse array of recent projects (simple parser)
        // Format: { "recentProjects": [ { "name": ..., "path": ..., ... }, ... ] }
        size_t pos = 0;
        while (true)
        {
            pos = content.find('{', pos);
            if (pos == std::string::npos)
                break;

            // Skip the outer object
            if (content.contains("\"recentProjects\"") && pos == 0)
            {
                pos++;
                continue;
            }

            size_t end = content.find('}', pos);
            if (end == std::string::npos)
                break;

            std::string entry = content.substr(pos, end - pos + 1);
            std::string name = ExtractJsonString(entry, "name");
            std::string path = ExtractJsonString(entry, "path");
            std::string engineVer = ExtractJsonString(entry, "engineVersion");
            uint64_t lastOpened = ExtractJsonUint64(entry, "lastOpened");

            if (!path.empty())
            {
                RecentProject rp;
                rp.name = name;
                rp.path = path;
                rp.engineVersion = engineVer;
                rp.lastOpened = lastOpened;
                rp.valid = fs::exists(path);
                m_recentProjects.push_back(rp);
            }

            pos = end + 1;
        }

        std::cout << "Loaded " << m_recentProjects.size() << " recent projects\n";
    }

    void ProjectManager::SaveRecentProjectsList()
    {
        std::string filePath = GetRecentProjectsFilePath();

        try
        {
            fs::create_directories(fs::path(filePath).parent_path());

            std::ofstream file(filePath);
            if (!file.is_open())
                return;

            file << "{\n";
            file << "  \"recentProjects\": [\n";
            for (size_t i = 0; i < m_recentProjects.size(); ++i)
            {
                const auto& rp = m_recentProjects[i];
                file << "    {\n";
                file << "      \"name\": \"" << EscapeJsonString(rp.name) << "\",\n";
                file << "      \"path\": \"" << EscapeJsonString(rp.path) << "\",\n";
                file << "      \"engineVersion\": \"" << EscapeJsonString(rp.engineVersion) << "\",\n";
                file << "      \"lastOpened\": " << rp.lastOpened << "\n";
                file << "    }";
                if (i + 1 < m_recentProjects.size())
                    file << ",";
                file << "\n";
            }
            file << "  ]\n";
            file << "}\n";
            file.close();

            if (m_fileCache)
            {
                m_fileCache->Invalidate(filePath);
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error saving recent projects: " << e.what() << "\n";
        }
    }

    // ------------------------------------------------------------------
    // Static template helpers
    // ------------------------------------------------------------------
    std::string ProjectManager::GetProjectTemplateName(ProjectTemplate t)
    {
        switch (t)
        {
        case ProjectTemplate::Empty:
            return "Empty";
        case ProjectTemplate::FirstPerson:
            return "First Person";
        case ProjectTemplate::ThirdPerson:
            return "Third Person";
        case ProjectTemplate::TopDown:
            return "Top Down";
        case ProjectTemplate::Blank3D:
            return "Blank 3D";
        default:
            return "Unknown";
        }
    }

    std::string ProjectManager::GetProjectTemplateDescription(ProjectTemplate t)
    {
        switch (t)
        {
        case ProjectTemplate::Empty:
            return "A completely empty project with no default scenes or assets.";
        case ProjectTemplate::FirstPerson:
            return "First-person template with player controller, camera, and a basic level.";
        case ProjectTemplate::ThirdPerson:
            return "Third-person template with follow camera and character controller.";
        case ProjectTemplate::TopDown:
            return "Top-down camera template suitable for strategy or adventure games.";
        case ProjectTemplate::Blank3D:
            return "A single empty 3D scene with default lighting and a ground plane.";
        default:
            return "";
        }
    }

} // namespace SparkEditor
