/**
 * @file LauncherApp.cpp
 * @brief Standalone SparkLauncher UI + editor-spawn logic.
 * @author Spark Engine Team
 * @date 2025
 */

#include "LauncherApp.h"
#include "LauncherPaths.h"

#include "Utils/FolderDialog.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace SparkLauncher
{
    namespace
    {
        std::string ReadSmallFile(const fs::path& p)
        {
            std::ifstream in(p);
            if (!in)
                return {};
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        // Very small JSON string-field extractor — matches ProjectManager.cpp's approach.
        std::string ExtractJsonString(const std::string& json, const std::string& key)
        {
            const std::string search = "\"" + key + "\"";
            size_t pos = json.find(search);
            if (pos == std::string::npos)
                return {};
            pos = json.find(':', pos);
            if (pos == std::string::npos)
                return {};
            pos = json.find('"', pos + 1);
            if (pos == std::string::npos)
                return {};
            size_t end = json.find('"', pos + 1);
            if (end == std::string::npos)
                return {};
            return json.substr(pos + 1, end - pos - 1);
        }

        std::string FormatTimestamp(uint64_t epochSeconds)
        {
            if (epochSeconds == 0)
                return "never";
            std::time_t t = static_cast<std::time_t>(epochSeconds);
            char buf[64] = {};
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
            return buf;
        }

        // Copies all of @p text or nothing: truncating UTF-8 could split a character
        // and leave the buffer undecodable.
        template <std::size_t N> bool CopyWhole(const std::string& text, char (&buffer)[N])
        {
            if (text.size() >= N)
            {
                return false;
            }
            std::memcpy(buffer, text.c_str(), text.size() + 1);
            return true;
        }
    } // namespace

    LauncherApp::LauncherApp() = default;
    LauncherApp::~LauncherApp() = default;

    bool LauncherApp::Initialize()
    {
        m_projectManager = std::make_unique<SparkEditor::ProjectManager>();
        if (!m_projectManager->Initialize())
        {
            m_statusMessage = "Failed to initialize ProjectManager";
            m_statusIsError = true;
            return false;
        }
        LoadTemplates();
        DefaultNewProjectLocation();
        return true;
    }

    void LauncherApp::LoadTemplates()
    {
        m_templates.clear();
        const fs::path templatesDir = FindLauncherTemplatesDirectory(GetLauncherExecutablePath(), fs::current_path());
        if (templatesDir.empty())
        {
            return;
        }
        for (const auto& entry : fs::directory_iterator(templatesDir))
        {
            if (!entry.is_directory())
                continue;
            const fs::path manifest = entry.path() / "template.json";
            if (!fs::exists(manifest))
                continue;

            const std::string json = ReadSmallFile(manifest);
            TemplateEntry t;
            t.directoryName = PathToUtf8(entry.path().filename());
            t.displayName = ExtractJsonString(json, "name");
            if (t.displayName.empty())
                t.displayName = t.directoryName;
            t.description = ExtractJsonString(json, "description");
            t.genre = ExtractJsonString(json, "genre");
            t.gameModule = ExtractJsonString(json, "gameModule");
            m_templates.push_back(std::move(t));
        }
        std::sort(m_templates.begin(), m_templates.end(),
                  [](const TemplateEntry& a, const TemplateEntry& b) { return a.displayName < b.displayName; });
    }

    void LauncherApp::DefaultNewProjectLocation()
    {
#ifdef _WIN32
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        // getenv returns active-code-page text on Windows, which fs::path(const char*)
        // decodes; the ImGui buffer holds UTF-8.
        fs::path base = home ? fs::path(home) / "SparkProjects" : fs::current_path();
        if (!CopyWhole(PathToUtf8(base), m_newProjectLocation))
        {
            m_newProjectLocation[0] = '\0';
        }
    }

    void LauncherApp::DrawUI()
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

        ImGui::Begin("SparkLauncher", nullptr, flags);

        ImGui::TextUnformatted("Spark Engine Launcher");
        ImGui::SameLine();
        ImGui::TextDisabled("(pick a project to launch the editor)");
        ImGui::Separator();

        if (ImGui::BeginTabBar("##LauncherTabs"))
        {
            if (ImGui::BeginTabItem("Projects"))
            {
                m_activeTab = Tab::Projects;
                DrawProjectsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("New Project"))
            {
                m_activeTab = Tab::NewProject;
                DrawNewProjectTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        if (!m_statusMessage.empty())
        {
            ImGui::Separator();
            ImVec4 color = m_statusIsError ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImVec4(0.5f, 1.0f, 0.5f, 1.0f);
            ImGui::TextColored(color, "%s", m_statusMessage.c_str());
        }

        ImGui::End();
    }

    void LauncherApp::DrawProjectsTab()
    {
        if (ImGui::Button("Open Existing Project..."))
        {
            std::string folder;
            if (SparkEditor::Utils::BrowseForFolder(folder, "Select Project Folder"))
            {
                // BrowseForFolder returns active-code-page text on Windows, which
                // fs::path(std::string) decodes. From here on the project is a path, and
                // it only becomes text again as UTF-8 (ProjectManager, status line).
                const fs::path folderPath(folder);
                // A project directory contains a single .sparkproject file.
                fs::path chosen;
                std::error_code iterateError;
                for (fs::directory_iterator it(folderPath, iterateError), end; !iterateError && it != end;
                     it.increment(iterateError))
                {
                    std::error_code entryError;
                    if (it->is_regular_file(entryError) && it->path().extension() == ".sparkproject")
                    {
                        chosen = it->path();
                        break;
                    }
                }
                if (chosen.empty())
                {
                    m_statusMessage = "No .sparkproject file found in " + PathToUtf8(folderPath);
                    m_statusIsError = true;
                }
                else if (!m_projectManager->OpenProject(PathToUtf8(chosen)))
                {
                    m_statusMessage = "Failed to open " + PathToUtf8(chosen);
                    m_statusIsError = true;
                }
                else
                {
                    SpawnTarget(chosen, LaunchTarget::Editor);
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh"))
        {
            m_projectManager->RefreshRecentProjects();
        }

        ImGui::Separator();

        const auto recent = m_projectManager->GetRecentProjects();
        if (recent.empty())
        {
            ImGui::TextDisabled("No recent projects — create a new one or open an existing project.");
            return;
        }

        ImGui::TextDisabled("Recent Projects");
        if (ImGui::BeginTable("##RecentProjects", 4,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Last Opened");
            ImGui::TableSetupColumn("Engine");
            ImGui::TableSetupColumn("Actions");
            ImGui::TableHeadersRow();

            for (const auto& rp : recent)
            {
                // ProjectManager stores recent-project paths as UTF-8 text.
                const auto spawnRecent = [this, &rp](LaunchTarget target)
                {
                    auto projectFile = PathFromUtf8(rp.path);
                    if (!projectFile)
                    {
                        m_statusMessage = projectFile.error() + ": " + rp.path;
                        m_statusIsError = true;
                        return;
                    }
                    SpawnTarget(*projectFile, target);
                };

                ImGui::TableNextRow();
                ImGui::PushID(rp.path.c_str());

                ImGui::TableNextColumn();
                const ImVec4 color = rp.valid ? ImVec4(1, 1, 1, 1) : ImVec4(1.0f, 0.5f, 0.5f, 1.0f);
                ImGui::TextColored(color, "%s", rp.name.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", rp.path.c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(FormatTimestamp(rp.lastOpened).c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(rp.engineVersion.c_str());

                ImGui::TableNextColumn();
                if (rp.valid && ImGui::SmallButton("Editor"))
                {
                    if (m_projectManager->OpenProject(rp.path))
                        spawnRecent(LaunchTarget::Editor);
                    else
                    {
                        m_statusMessage = "Failed to open " + rp.path;
                        m_statusIsError = true;
                    }
                }
                ImGui::SameLine();
                if (rp.valid && ImGui::SmallButton("Play"))
                    spawnRecent(LaunchTarget::Game);
                ImGui::SameLine();
                if (rp.valid && ImGui::SmallButton("Server"))
                    spawnRecent(LaunchTarget::DedicatedServer);
                ImGui::SameLine();
                if (rp.valid && ImGui::SmallButton("Services"))
                    spawnRecent(LaunchTarget::ServiceTopology);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove"))
                {
                    m_projectManager->RemoveRecentProject(rp.path);
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void LauncherApp::DrawNewProjectTab()
    {
        ImGui::TextDisabled("Template");
        if (m_templates.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "No templates found in the development tree or installed SparkEngine share directory.");
            return;
        }

        if (ImGui::BeginCombo("##Template", m_templates[m_selectedTemplate].displayName.c_str()))
        {
            for (int i = 0; i < static_cast<int>(m_templates.size()); ++i)
            {
                const bool selected = (i == m_selectedTemplate);
                if (ImGui::Selectable(m_templates[i].displayName.c_str(), selected))
                    m_selectedTemplate = i;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const TemplateEntry& tpl = m_templates[m_selectedTemplate];
        ImGui::TextWrapped("%s", tpl.description.c_str());
        if (!tpl.genre.empty())
            ImGui::Text("Genre: %s   Module: %s", tpl.genre.c_str(), tpl.gameModule.c_str());

        ImGui::Separator();
        ImGui::InputText("Project Name", m_newProjectName, sizeof(m_newProjectName));
        ImGui::InputText("Location", m_newProjectLocation, sizeof(m_newProjectLocation));
        ImGui::SameLine();
        if (ImGui::Button("Browse..."))
        {
            std::string folder;
            if (SparkEditor::Utils::BrowseForFolder(folder, "Select Parent Folder"))
            {
                // Active-code-page text from the dialog; the ImGui buffer holds UTF-8.
                if (!CopyWhole(PathToUtf8(fs::path(folder)), m_newProjectLocation))
                {
                    m_statusMessage = "Selected folder path is too long";
                    m_statusIsError = true;
                }
            }
        }
        ImGui::InputTextMultiline("Description", m_newProjectDescription, sizeof(m_newProjectDescription),
                                  ImVec2(0, ImGui::GetTextLineHeight() * 3));

        // Both buffers are ImGui (UTF-8) text. fs::path(const char*) would read them as
        // the active code page on Windows, and path::string() throws for characters
        // outside it -- every frame, from whatever the user typed.
        const auto location = PathFromUtf8(m_newProjectLocation);
        const auto name = PathFromUtf8(m_newProjectName);
        fs::path projectRoot;
        if (location && name)
        {
            projectRoot = *location / *name;
            ImGui::TextDisabled("Will be created at: %s", PathToUtf8(projectRoot).c_str());
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Project name and location must be valid UTF-8 text");
        }

        ImGui::Separator();
        if (ImGui::Button("Create Project", ImVec2(160, 0)))
        {
            if (std::strlen(m_newProjectName) == 0 || std::strlen(m_newProjectLocation) == 0)
            {
                m_statusMessage = "Name and location are required";
                m_statusIsError = true;
                return;
            }
            if (projectRoot.empty())
            {
                m_statusMessage = "Project name and location must be valid UTF-8 text";
                m_statusIsError = true;
                return;
            }
            std::error_code ec;
            fs::create_directories(*location, ec);
            if (ec)
            {
                m_statusMessage = "Cannot create directory: " + ec.message();
                m_statusIsError = true;
                return;
            }

            if (!m_projectManager->CreateProjectFromTemplate(m_newProjectName, PathToUtf8(projectRoot),
                                                             tpl.directoryName))
            {
                m_statusMessage = "CreateProjectFromTemplate failed for " + tpl.directoryName;
                m_statusIsError = true;
                return;
            }

            fs::path projectFileName = *name;
            projectFileName += ".sparkproject";
            const fs::path sparkproj = projectRoot / projectFileName;
            std::error_code existsError;
            if (!fs::exists(sparkproj, existsError))
            {
                m_statusMessage = "Project file not produced: " + PathToUtf8(sparkproj);
                m_statusIsError = true;
                return;
            }

            SpawnTarget(sparkproj, LaunchTarget::Editor);
        }
    }

    bool LauncherApp::SpawnTarget(const fs::path& projectFile, LaunchTarget target)
    {
        // This runs inside the ImGui frame with nothing above it to catch: a
        // filesystem or encoding exception from an unusual project path must end as a
        // status message, not std::terminate.
        try
        {
            const fs::path ownPath = GetLauncherExecutablePath();
            if (ownPath.empty())
            {
                m_statusMessage = "Could not determine launcher path";
                m_statusIsError = true;
                return false;
            }
            auto request = BuildLaunchRequest(ownPath.parent_path(), projectFile, target);
            if (!request)
            {
                m_statusMessage = request.error();
                m_statusIsError = true;
                return false;
            }
            auto launched = LaunchDetached(*request);
            if (!launched)
            {
                m_statusMessage = launched.error();
                m_statusIsError = true;
                return false;
            }
        }
        catch (const std::exception& exception)
        {
            m_statusMessage = std::string("Could not launch ") + LaunchTargetName(target) + ": " + exception.what();
            m_statusIsError = true;
            return false;
        }
        m_statusMessage = std::string(LaunchTargetName(target)) + " started";
        m_statusIsError = false;
        m_shouldClose = target != LaunchTarget::DedicatedServer;
        return true;
    }
} // namespace SparkLauncher
