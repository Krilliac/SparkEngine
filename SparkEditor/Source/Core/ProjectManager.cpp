/**
 * @file ProjectManager.cpp
 * @brief Implementation of the project management system
 * @author Spark Engine Team
 * @date 2025
 */

#include "ProjectManager.h"
#include "Utils/ContainerUtils.h"
#include "Utils/LocalFileCache.h"
#include "Utils/Validate.h"
#include "Engine/ECS/Components.h"
#include "SceneManager/ReflectedSceneSerializer.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <set>
#include <array>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
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
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    constexpr char hex[] = "0123456789ABCDEF";
                    const unsigned char value = static_cast<unsigned char>(c);
                    out += "\\u00";
                    out += hex[value >> 4];
                    out += hex[value & 0x0F];
                }
                else
                {
                    out += c;
                }
                break;
            }
        }
        return out;
    }

    static int JsonHexValue(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    static bool ParseJsonHex4(const std::string& json, size_t offset, uint32_t& value)
    {
        if (offset + 4 > json.size())
            return false;
        value = 0;
        for (size_t index = 0; index < 4; ++index)
        {
            const int digit = JsonHexValue(json[offset + index]);
            if (digit < 0)
                return false;
            value = (value << 4) | static_cast<uint32_t>(digit);
        }
        return true;
    }

    static bool AppendUtf8(uint32_t codePoint, std::string& output)
    {
        if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
            return false;
        if (codePoint <= 0x7F)
            output.push_back(static_cast<char>(codePoint));
        else if (codePoint <= 0x7FF)
        {
            output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else if (codePoint <= 0xFFFF)
        {
            output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else
        {
            output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        return true;
    }

    // `index` points at the character immediately after a JSON backslash and
    // advances across any consumed unicode digits/surrogate pair.
    static bool DecodeJsonEscape(const std::string& json, size_t& index, std::string& output)
    {
        switch (json[index])
        {
        case '"':
            output += '"';
            return true;
        case '\\':
            output += '\\';
            return true;
        case '/':
            output += '/';
            return true;
        case 'b':
            output += '\b';
            return true;
        case 'f':
            output += '\f';
            return true;
        case 'n':
            output += '\n';
            return true;
        case 'r':
            output += '\r';
            return true;
        case 't':
            output += '\t';
            return true;
        case 'u':
        {
            uint32_t first = 0;
            if (!ParseJsonHex4(json, index + 1, first))
                return false;
            index += 4;
            if (first >= 0xD800 && first <= 0xDBFF)
            {
                if (index + 6 >= json.size() || json[index + 1] != '\\' || json[index + 2] != 'u')
                    return false;
                uint32_t second = 0;
                if (!ParseJsonHex4(json, index + 3, second) || second < 0xDC00 || second > 0xDFFF)
                    return false;
                first = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
                index += 6;
            }
            else if (first >= 0xDC00 && first <= 0xDFFF)
            {
                return false;
            }
            return AppendUtf8(first, output);
        }
        default:
            return false;
        }
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
        std::string result;
        for (size_t i = pos + 1; i < json.size(); ++i)
        {
            const char c = json[i];
            if (c == '\"')
                return result;
            if (c != '\\')
            {
                result += c;
                continue;
            }
            if (++i >= json.size())
                return "";
            if (!DecodeJsonEscape(json, i, result))
                return "";
        }
        return "";
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

        size_t i = pos + 1;
        while (i < end)
        {
            size_t qStart = json.find('\"', i);
            if (qStart == std::string::npos || qStart >= end)
                break;
            std::string value;
            bool closed = false;
            for (i = qStart + 1; i < end; ++i)
            {
                char c = json[i];
                if (c == '\"')
                {
                    ++i;
                    closed = true;
                    break;
                }
                if (c == '\\' && i + 1 < end)
                {
                    ++i;
                    if (!DecodeJsonEscape(json, i, value) || i >= end)
                        return {};
                    continue;
                }
                else if (c == '\\')
                {
                    return {};
                }
                value += c;
            }
            if (!closed)
                break;
            result.push_back(std::move(value));
        }
        return result;
    }

    // ------------------------------------------------------------------
    // ProjectManager
    // ------------------------------------------------------------------
    std::mutex ProjectManager::s_activeProjectMutex;
    std::string ProjectManager::s_activeProjectPath;

    namespace
    {
        constexpr std::array<std::string_view, 2> kEmptyFeatures = {"minimal", "scene-editing"};
        constexpr std::array<std::string_view, 6> kFirstPersonFeatures = {"movement", "mouselook", "weapons",
                                                                          "damage",   "hud",       "restart"};
        constexpr std::array<std::string_view, 5> kThirdPersonFeatures = {"movement", "orbit-camera", "jump", "pickup",
                                                                          "goal"};
        constexpr std::array<std::string_view, 6> kTopDownFeatures = {"movement", "pan-zoom-camera", "collision",
                                                                      "enemy",    "pickup",          "restart"};
        constexpr std::array<std::string_view, 4> kBlank3DFeatures = {"primitive", "ground", "lighting", "fly-camera"};
        constexpr std::array<std::string_view, 7> kMmoFeatures = {
            "local-client-server", "character", "faction", "capture-point", "bot", "chat", "respawn"};
        constexpr std::array<std::string_view, 8> kPlatformerFeatures = {
            "movement", "jump", "double-jump", "collectibles", "hazard", "checkpoint", "finish", "restart"};
        constexpr std::array<std::string_view, 7> kRpgFeatures = {"movement", "dialogue", "quest",    "inventory",
                                                                  "combat",   "reward",   "save-load"};

        constexpr std::array<ProjectTemplateDescriptor, 8> kProjectTemplates = {{
            {ProjectTemplate::Blank3D, "blank-3d", "Blank 3D",
             "A ready-to-edit 3D scene with a visible primitive, ground, lighting, and fly camera.", "[3D]", "Blank3D",
             "Scenes/Default.sparkscene", "General", kBlank3DFeatures, true},
            {ProjectTemplate::FirstPerson, "first-person", "First Person",
             "A compact first-person game with movement, weapons, a damageable target, HUD, and restart loop.", "[FP]",
             "FPSStarter", "Scenes/Arena.sparkscene", "FPS", kFirstPersonFeatures, false},
            {ProjectTemplate::ThirdPerson, "third-person", "Third Person",
             "A third-person adventure slice with orbit camera, jumping, a pickup, and a goal.", "[TP]",
             "ThirdPersonStarter", "Scenes/Adventure.sparkscene", "Adventure", kThirdPersonFeatures, false},
            {ProjectTemplate::TopDown, "top-down", "Top Down",
             "A top-down action slice with pan/zoom camera, collision, an enemy, a pickup, and restart.", "[TD]",
             "TopDownStarter", "Scenes/Skirmish.sparkscene", "Action", kTopDownFeatures, false},
            {ProjectTemplate::Platformer, "platformer", "Platformer",
             "A complete platformer level with double jump, collectibles, hazards, checkpoint, finish, and restart.",
             "[PL]", "PlatformerKit", "Scenes/Level01.sparkscene", "Platformer", kPlatformerFeatures, false},
            {ProjectTemplate::RPG, "rpg", "RPG",
             "A village RPG slice with dialogue, quest, inventory, combat, reward, and save/load.", "[RPG]",
             "RPGStarter", "Scenes/Village.sparkscene", "RPG", kRpgFeatures, false},
            {ProjectTemplate::MMO, "mmo", "MMO",
             "A bounded local client/server sample with character setup, faction objective, bot, chat, and respawn.",
             "[MMO]", "MMOStarter", "Scenes/Frontier.sparkscene", "MMO", kMmoFeatures, false},
            {ProjectTemplate::Empty, "empty", "Empty",
             "A truthful empty project with an editable world and no bundled art or gameplay assumptions.", "[ ]",
             "EmptyProject", "Scenes/Default.sparkscene", "General", kEmptyFeatures, true},
        }};

        static_assert(kProjectTemplates.size() == 8);

        std::string NormalizeProjectPath(const std::string& value)
        {
            if (value.empty())
                return {};
            std::error_code ec;
            fs::path normalized = fs::absolute(fs::path(value), ec);
            if (ec)
            {
                ec.clear();
                normalized = fs::path(value);
            }
            const fs::path canonical = fs::weakly_canonical(normalized, ec);
            if (!ec)
                normalized = canonical;
            return normalized.lexically_normal().string();
        }

        bool ProjectPathsEqual(const std::string& left, const std::string& right)
        {
            std::string normalizedLeft = NormalizeProjectPath(left);
            std::string normalizedRight = NormalizeProjectPath(right);
#ifdef _WIN32
            std::transform(normalizedLeft.begin(), normalizedLeft.end(), normalizedLeft.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::transform(normalizedRight.begin(), normalizedRight.end(), normalizedRight.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
            return normalizedLeft == normalizedRight;
        }

        std::string MakeCodeIdentifier(const std::string& value)
        {
            std::string result;
            result.reserve(value.size() + 8);
            for (unsigned char c : value)
                result.push_back(std::isalnum(c) || c == '_' ? static_cast<char>(c) : '_');
            if (result.empty())
                result = "SparkGame";
            if (std::isdigit(static_cast<unsigned char>(result.front())))
                result.insert(0, "SparkGame_");
            return result;
        }

        std::string EscapeCppString(const std::string& value)
        {
            std::string result;
            result.reserve(value.size() + 8);
            for (char c : value)
            {
                switch (c)
                {
                case '\\':
                    result += "\\\\";
                    break;
                case '\"':
                    result += "\\\"";
                    break;
                case '\n':
                    result += "\\n";
                    break;
                case '\r':
                    result += "\\r";
                    break;
                case '\t':
                    result += "\\t";
                    break;
                default:
                    result += c;
                    break;
                }
            }
            return result;
        }

        bool WriteNewFile(const fs::path& path, const std::string& contents)
        {
            if (fs::exists(path))
                return true;
            std::ofstream file(path, std::ios::binary);
            if (!file.is_open())
                return false;
            file << contents;
            return file.good();
        }
    } // namespace

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
            m_currentProject.path = NormalizeProjectPath(projectRoot);
            m_currentProjectFilePath =
                NormalizeProjectPath((fs::path(m_currentProject.path) / (projectName + ".sparkproject")).string());
            m_currentProject.version = "1.0.0";
            m_currentProject.description = description.empty() ? "Spark Engine Project" : description;
            m_currentProject.engineVersion = GetCurrentEngineVersion().ToString();
            m_currentProject.createdTime = GetCurrentTimestamp();
            m_currentProject.lastModified = m_currentProject.createdTime;
            m_currentProject.templateType = templateType;
            m_currentProject.modules.push_back(MakeCodeIdentifier(projectName));

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
            {
                std::lock_guard lock(s_activeProjectMutex);
                s_activeProjectPath = m_currentProject.path;
            }
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
                m_currentProject.path = NormalizeProjectPath(projectPath);
                m_currentProjectFilePath =
                    NormalizeProjectPath((fs::path(m_currentProject.path) / (projectName + ".sparkproject")).string());
                m_currentProject.version = "1.0.0";
                m_currentProject.engineVersion = GetCurrentEngineVersion().ToString();
                m_currentProject.modules.push_back(MakeCodeIdentifier(projectName));
                m_currentProject.createdTime = GetCurrentTimestamp();
                m_currentProject.lastModified = m_currentProject.createdTime;
                if (!SaveProjectFile())
                    return false;
            }

            if (!EnsureBuildScaffold(projectPath, m_currentProject.name))
                return false;

            m_hasOpenProject = true;
            {
                std::lock_guard lock(s_activeProjectMutex);
                s_activeProjectPath = m_currentProject.path;
            }
            AddToRecentProjects(projectName, GetProjectFilePath());

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

            // Text file extensions that should have their template name rewritten
            static const std::set<std::string> textExtensions = {".h",    ".hpp",   ".cpp", ".c", ".txt",
                                                                 ".json", ".cmake", ".md",  ".py"};

            // Templates are shipped as real, compilable game modules named after
            // their directory (e.g. `Templates/FPSStarter` uses `FPSStarterModule`,
            // project name `FPSStarter`, CMake target `FPSStarter`, etc.). When we
            // materialize a user project from a template, rewrite every textual
            // occurrence of the template's name to the user's chosen project name
            // so they do not have to do it by hand.
            const std::string templateName = fs::path(templatePath).filename().string();
            if (templateName.empty() || templateName == projectName)
            {
                // Nothing to rewrite — the copy is the final artifact.
                return true;
            }

            for (auto& entry : fs::recursive_directory_iterator(destPath))
            {
                if (!entry.is_regular_file())
                    continue;

                auto ext = entry.path().extension().string();
                if (!Spark::ContainerUtils::Contains(textExtensions, ext))
                    continue;

                std::ifstream inFile(entry.path());
                if (!inFile.is_open())
                    continue;

                std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
                inFile.close();

                if (!content.contains(templateName))
                    continue;

                size_t pos = 0;
                while ((pos = content.find(templateName, pos)) != std::string::npos)
                {
                    content.replace(pos, templateName.length(), projectName);
                    pos += projectName.length();
                }

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

        // Older editor-created projects predate the standalone build skeleton.
        // Add only missing generated files; never overwrite user-authored build
        // scripts or module sources.
        if (!EnsureBuildScaffold(m_currentProject.path, m_currentProject.name))
            return false;

        m_hasOpenProject = true;
        {
            std::lock_guard lock(s_activeProjectMutex);
            s_activeProjectPath = NormalizeProjectPath(m_currentProject.path);
        }
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

    bool ProjectManager::RecordOpenedScene(const std::string& scenePath)
    {
        if (!m_hasOpenProject || scenePath.empty())
            return false;

        const fs::path projectRoot = NormalizeProjectPath(m_currentProject.path);
        fs::path candidate(scenePath);
        if (candidate.is_relative())
            candidate = projectRoot / candidate;
        candidate = NormalizeProjectPath(candidate.string());
        if (!fs::is_regular_file(candidate))
            return false;

        std::error_code ec;
        const fs::path relative = fs::relative(candidate, projectRoot, ec).lexically_normal();
        if (ec || relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            return false;

        const std::string storedPath = relative.generic_string();
        const ProjectInfo previous = m_currentProject;
        m_currentProject.lastOpenedScene = storedPath;
        if (std::none_of(m_currentProject.scenes.begin(), m_currentProject.scenes.end(), [&](const std::string& scene)
                         { return ProjectPathsEqual((projectRoot / fs::path(scene)).string(), candidate.string()); }))
        {
            m_currentProject.scenes.push_back(storedPath);
        }
        m_currentProject.lastModified = GetCurrentTimestamp();
        if (SaveProjectFile())
            return true;

        m_currentProject = previous;
        return false;
    }

    void ProjectManager::CloseProject()
    {
        if (m_hasOpenProject)
        {
            std::cout << "Closing project: " << m_currentProject.name << "\n";
            ProjectInfo closed = m_currentProject;
            m_hasOpenProject = false;
            m_currentProject = ProjectInfo{};
            m_currentProjectFilePath.clear();
            {
                std::lock_guard lock(s_activeProjectMutex);
                if (ProjectPathsEqual(s_activeProjectPath, closed.path))
                    s_activeProjectPath.clear();
            }
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
        if (!m_currentProjectFilePath.empty())
            return m_currentProjectFilePath;
        if (m_currentProject.path.empty() || m_currentProject.name.empty())
            return {};
        return NormalizeProjectPath(
            (fs::path(m_currentProject.path) / (m_currentProject.name + ".sparkproject")).string());
    }

    std::string ProjectManager::GetActiveProjectPath()
    {
        std::lock_guard lock(s_activeProjectMutex);
        return s_activeProjectPath;
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
        const std::string normalizedPath = NormalizeProjectPath(path);
        {
            std::lock_guard<std::mutex> lock(m_recentProjectsMutex);
            m_recentProjects.erase(std::remove_if(m_recentProjects.begin(), m_recentProjects.end(),
                                                  [&](const RecentProject& rp)
                                                  { return ProjectPathsEqual(rp.path, normalizedPath); }),
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
        const std::string normalizedPath = NormalizeProjectPath(sparkprojectPath);
        {
            std::lock_guard<std::mutex> lock(m_recentProjectsMutex);
            // Remove existing entry with same path
            m_recentProjects.erase(std::remove_if(m_recentProjects.begin(), m_recentProjects.end(),
                                                  [&](const RecentProject& rp)
                                                  { return ProjectPathsEqual(rp.path, normalizedPath); }),
                                   m_recentProjects.end());

            RecentProject rp;
            rp.name = projectName;
            rp.path = normalizedPath;
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

        // Preserve the selected document path independently from its display
        // name. On macOS, normalization also resolves /var -> /private/var;
        // every public project path then uses the same canonical spelling.
        const std::string normalizedProjectFile = NormalizeProjectPath(sparkprojectPath);
        std::string projectRoot = fs::path(normalizedProjectFile).parent_path().string();

        m_currentProject = ProjectInfo{};
        m_currentProjectFilePath = normalizedProjectFile;
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

            // Create a scene through the same serializer the editor uses to
            // open/save it. Hand-written legacy JSON here used a different
            // schema and appeared empty as soon as the project opened.
            if (!CreateDefaultScene(projectPath + "/Scenes/Default.sparkscene", templateType))
                return false;

            // Create editor settings
            CreateDefaultEditorSettings(projectPath + "/Config");

            if (!EnsureBuildScaffold(projectPath, fs::path(projectPath).filename().string()))
                return false;

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

    bool ProjectManager::CreateDefaultScene(const std::string& scenePath, ProjectTemplate templateType)
    {
        ::World world;

        const ::EntityID lightEntity = world.CreateEntity("Directional Light");
        auto& lightTransform = world.AddComponent<::Transform>(lightEntity);
        lightTransform.position = {0.0f, 10.0f, 0.0f};
        lightTransform.rotation = {50.0f, -30.0f, 0.0f};
        auto& light = world.AddComponent<::LightComponent>(lightEntity);
        light.type = ::LightComponent::Type::Directional;
        light.color = {1.0f, 0.96f, 0.84f};
        light.intensity = 1.0f;

        if (templateType != ProjectTemplate::Empty)
        {
            const ::EntityID groundEntity = world.CreateEntity("Ground");
            auto& groundTransform = world.AddComponent<::Transform>(groundEntity);
            groundTransform.scale = {50.0f, 0.1f, 50.0f};
            auto& groundMesh = world.AddComponent<::MeshRenderer>(groundEntity);
            // A non-empty missing path deliberately uses the renderer's
            // procedural placeholder mesh, giving every fresh project visible
            // geometry without depending on engine-repository assets.
            groundMesh.meshPath = "__spark_primitive_ground__.obj";
        }

        if (templateType != ProjectTemplate::Empty)
        {
            const ::EntityID cameraEntity = world.CreateEntity("Main Camera");
            auto& cameraTransform = world.AddComponent<::Transform>(cameraEntity);
            switch (templateType)
            {
            case ProjectTemplate::FirstPerson:
                cameraTransform.position = {0.0f, 1.7f, -8.0f};
                break;
            case ProjectTemplate::ThirdPerson:
                cameraTransform.position = {0.0f, 5.0f, -8.0f};
                cameraTransform.rotation = {25.0f, 0.0f, 0.0f};
                break;
            case ProjectTemplate::TopDown:
                cameraTransform.position = {0.0f, 20.0f, 0.0f};
                cameraTransform.rotation = {90.0f, 0.0f, 0.0f};
                break;
            case ProjectTemplate::Platformer:
                cameraTransform.position = {0.0f, 5.0f, -12.0f};
                cameraTransform.rotation = {10.0f, 0.0f, 0.0f};
                break;
            case ProjectTemplate::RPG:
                cameraTransform.position = {0.0f, 8.0f, -10.0f};
                cameraTransform.rotation = {30.0f, 0.0f, 0.0f};
                break;
            default:
                cameraTransform.position = {0.0f, 3.0f, -8.0f};
                cameraTransform.rotation = {15.0f, 0.0f, 0.0f};
                break;
            }
            auto& camera = world.AddComponent<::Camera>(cameraEntity);
            camera.isMainCamera = true;
        }

        if (templateType == ProjectTemplate::FirstPerson || templateType == ProjectTemplate::ThirdPerson)
        {
            const ::EntityID playerEntity = world.CreateEntity("Player");
            auto& playerTransform = world.AddComponent<::Transform>(playerEntity);
            playerTransform.position = {0.0f, 1.0f, 0.0f};
            world.AddComponent<::CharacterControllerComponent>(playerEntity);
        }

        return Spark::SaveWorld(world, scenePath);
    }

    bool ProjectManager::EnsureBuildScaffold(const std::string& projectPath, const std::string& projectName)
    {
        try
        {
            const fs::path root = fs::absolute(projectPath).lexically_normal();
            const fs::path source = root / "Source";
            fs::create_directories(source);

            const std::string target = MakeCodeIdentifier(projectName);
            const std::string displayName = EscapeCppString(projectName);

            std::ostringstream cmake;
            cmake << "cmake_minimum_required(VERSION 3.25)\n"
                  << "project(" << target << " LANGUAGES CXX)\n\n"
                  << "set(CMAKE_CXX_STANDARD 23)\n"
                  << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n"
                  << "find_package(SparkEngine CONFIG REQUIRED)\n\n"
                  << "file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS\n"
                  << "    \"Source/*.cpp\"\n"
                  << "    \"Source/*.h\"\n"
                  << "    \"Source/*.hpp\")\n\n"
                  << "spark_add_game_module(" << target << " ${GAME_SOURCES})\n"
                  << "target_include_directories(" << target << " PRIVATE \"Source\")\n";

            std::ostringstream header;
            header << "#pragma once\n\n"
                   << "#include <Spark/SparkSDK.h>\n"
                   << "#include <Graphics/GraphicsEngine.h>\n\n"
                   << "class " << target << "Module final : public Spark::IModule\n"
                   << "{\npublic:\n"
                   << "    Spark::ModuleInfo GetModuleInfo() const override\n"
                   << "    {\n"
                   << "        Spark::ModuleInfo info{};\n"
                   << "        info.name = \"" << displayName << "\";\n"
                   << "        info.version = \"1.0.0\";\n"
                   << "        info.sdkVersion = SPARK_SDK_VERSION;\n"
                   << "        info.loadOrder = 1000;\n"
                   << "        return info;\n"
                   << "    }\n\n"
                   << "    bool OnLoad(Spark::IEngineContext* context) override\n"
                   << "    {\n        m_context = context;\n        return true;\n    }\n\n"
                   << "    void OnUnload() override { m_context = nullptr; }\n"
                   << "    void OnUpdate(float deltaTime) override { (void)deltaTime; }\n"
                   << "    void OnRender() override\n"
                   << "    {\n"
                   << "        // Modules own their graphics frame.  Present a valid clear frame\n"
                   << "        // even before the project adds its first renderer.\n"
                   << "        if (m_context)\n"
                   << "            if (auto* graphics = m_context->GetGraphics())\n"
                   << "            {\n"
                   << "                graphics->BeginFrame();\n"
                   << "                graphics->EndFrame();\n"
                   << "            }\n"
                   << "    }\n\n"
                   << "private:\n"
                   << "    Spark::IEngineContext* m_context = nullptr;\n"
                   << "};\n";

            std::ostringstream implementation;
            implementation << "#include \"GameModule.h\"\n"
                           << "#include <Spark/ModuleDllMain.h>\n\n"
                           << "SPARK_IMPLEMENT_MODULE(" << target << "Module)\n";

            std::ostringstream modules;
            modules << "{\n  \"modules\": [\n    {\n"
                    << "      \"name\": \"" << EscapeJsonString(target) << "\",\n"
                    << "      \"path\": \"" << EscapeJsonString(target) << ".dll\",\n"
                    << "      \"loadOrder\": 1000\n"
                    << "    }\n  ]\n}\n";

            return WriteNewFile(root / "CMakeLists.txt", cmake.str()) &&
                   WriteNewFile(source / "GameModule.h", header.str()) &&
                   WriteNewFile(source / "GameModule.cpp", implementation.str()) &&
                   WriteNewFile(root / "spark.modules.json", modules.str());
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error creating project build scaffold: " << e.what() << "\n";
            return false;
        }
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

        // A recent-projects list is a handful of entries; anything huge is a
        // corrupt/runaway file (a 3 GB one was found in the wild after
        // repeated load/save cycles). Start fresh rather than parse it.
        {
            std::error_code sizeEc;
            const auto sz = fs::file_size(filePath, sizeEc);
            if (!sizeEc && sz > 1024 * 1024)
            {
                std::cerr << "RecentProjects.json is " << sz << " bytes - corrupt/runaway, resetting.\n";
                std::error_code rmEc;
                fs::remove(filePath, rmEc);
                return;
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

            if (m_recentProjects.size() >= 15)
                break; // hard cap - the UI never shows more

            if (!path.empty())
            {
                RecentProject rp;
                rp.name = name;
                rp.path = NormalizeProjectPath(path);
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
            const size_t count = std::min<size_t>(m_recentProjects.size(), 15);
            for (size_t i = 0; i < count; ++i)
            {
                const auto& rp = m_recentProjects[i];
                file << "    {\n";
                file << "      \"name\": \"" << EscapeJsonString(rp.name) << "\",\n";
                file << "      \"path\": \"" << EscapeJsonString(rp.path) << "\",\n";
                file << "      \"engineVersion\": \"" << EscapeJsonString(rp.engineVersion) << "\",\n";
                file << "      \"lastOpened\": " << rp.lastOpened << "\n";
                file << "    }";
                if (i + 1 < count)
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
    std::span<const ProjectTemplateDescriptor> ProjectManager::GetProjectTemplateDescriptors() noexcept
    {
        return kProjectTemplates;
    }

    const ProjectTemplateDescriptor* ProjectManager::FindProjectTemplateDescriptor(ProjectTemplate t) noexcept
    {
        const auto found =
            std::find_if(kProjectTemplates.begin(), kProjectTemplates.end(),
                         [t](const ProjectTemplateDescriptor& descriptor) { return descriptor.type == t; });
        return found == kProjectTemplates.end() ? nullptr : &*found;
    }

    const ProjectTemplateDescriptor* ProjectManager::FindProjectTemplateDescriptor(std::string_view identity) noexcept
    {
        const auto found = std::find_if(
            kProjectTemplates.begin(), kProjectTemplates.end(), [identity](const ProjectTemplateDescriptor& descriptor)
            { return descriptor.stableId == identity || descriptor.packageDirectory == identity; });
        return found == kProjectTemplates.end() ? nullptr : &*found;
    }

    std::string ProjectManager::GetProjectTemplateName(ProjectTemplate t)
    {
        const auto* descriptor = FindProjectTemplateDescriptor(t);
        return descriptor ? std::string(descriptor->displayName) : "Unknown";
    }

    std::string ProjectManager::GetProjectTemplateDescription(ProjectTemplate t)
    {
        const auto* descriptor = FindProjectTemplateDescriptor(t);
        return descriptor ? std::string(descriptor->description) : "";
    }

} // namespace SparkEditor
