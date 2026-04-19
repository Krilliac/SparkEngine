/**
 * @file LauncherApp.cpp
 * @brief Standalone SparkLauncher UI + editor-spawn logic.
 * @author Spark Engine Team
 * @date 2025
 */

#include "LauncherApp.h"

#include "Utils/FolderDialog.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace SparkLauncher
{
    namespace
    {
        // Location of the Templates/ directory, relative to the launcher binary or CWD.
        fs::path FindTemplatesDirectory()
        {
            const fs::path candidates[] = {
                fs::current_path() / "Templates",
                fs::current_path().parent_path() / "Templates",
                fs::current_path() / ".." / "Templates",
            };
            for (const auto& c : candidates)
            {
                if (fs::exists(c) && fs::is_directory(c))
                {
                    return fs::canonical(c);
                }
            }
            return {};
        }

        fs::path OwnExecutablePath()
        {
#ifdef _WIN32
            char buf[MAX_PATH] = {};
            DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
            return (n > 0) ? fs::path(buf) : fs::path{};
#else
            char buf[4096] = {};
            ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (n > 0)
            {
                buf[n] = '\0';
                return fs::path(buf);
            }
            return {};
#endif
        }

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
        const fs::path templatesDir = FindTemplatesDirectory();
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
            t.directoryName = entry.path().filename().string();
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
        fs::path base = home ? fs::path(home) / "SparkProjects" : fs::current_path();
        std::string s = base.string();
        std::strncpy(m_newProjectLocation, s.c_str(), sizeof(m_newProjectLocation) - 1);
        m_newProjectLocation[sizeof(m_newProjectLocation) - 1] = '\0';
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
                // A project directory contains a single .sparkproject file.
                fs::path chosen;
                for (const auto& entry : fs::directory_iterator(folder))
                {
                    if (entry.is_regular_file() && entry.path().extension() == ".sparkproject")
                    {
                        chosen = entry.path();
                        break;
                    }
                }
                if (chosen.empty())
                {
                    m_statusMessage = "No .sparkproject file found in " + folder;
                    m_statusIsError = true;
                }
                else if (!m_projectManager->OpenProject(chosen.string()))
                {
                    m_statusMessage = "Failed to open " + chosen.string();
                    m_statusIsError = true;
                }
                else
                {
                    SpawnEditor(chosen.string());
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
                if (rp.valid && ImGui::SmallButton("Launch"))
                {
                    if (m_projectManager->OpenProject(rp.path))
                        SpawnEditor(rp.path);
                    else
                    {
                        m_statusMessage = "Failed to open " + rp.path;
                        m_statusIsError = true;
                    }
                }
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
                               "No templates found. Expected Templates/ directory next to the launcher binary.");
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
                std::strncpy(m_newProjectLocation, folder.c_str(), sizeof(m_newProjectLocation) - 1);
                m_newProjectLocation[sizeof(m_newProjectLocation) - 1] = '\0';
            }
        }
        ImGui::InputTextMultiline("Description", m_newProjectDescription, sizeof(m_newProjectDescription),
                                  ImVec2(0, ImGui::GetTextLineHeight() * 3));

        const fs::path projectRoot = fs::path(m_newProjectLocation) / m_newProjectName;
        ImGui::TextDisabled("Will be created at: %s", projectRoot.string().c_str());

        ImGui::Separator();
        if (ImGui::Button("Create Project", ImVec2(160, 0)))
        {
            if (std::strlen(m_newProjectName) == 0 || std::strlen(m_newProjectLocation) == 0)
            {
                m_statusMessage = "Name and location are required";
                m_statusIsError = true;
                return;
            }
            std::error_code ec;
            fs::create_directories(projectRoot, ec);
            if (ec)
            {
                m_statusMessage = "Cannot create directory: " + ec.message();
                m_statusIsError = true;
                return;
            }

            if (!m_projectManager->CreateProjectFromTemplate(m_newProjectName, projectRoot.string(), tpl.directoryName))
            {
                m_statusMessage = "CreateProjectFromTemplate failed for " + tpl.directoryName;
                m_statusIsError = true;
                return;
            }

            const std::string sparkproj = (projectRoot / (std::string(m_newProjectName) + ".sparkproject")).string();
            if (!fs::exists(sparkproj))
            {
                m_statusMessage = "Project file not produced: " + sparkproj;
                m_statusIsError = true;
                return;
            }

            SpawnEditor(sparkproj);
        }
    }

    bool LauncherApp::SpawnEditor(const std::string& projectFilePath)
    {
        const fs::path ownPath = OwnExecutablePath();
        if (ownPath.empty())
        {
            m_statusMessage = "Could not determine launcher path";
            m_statusIsError = true;
            return false;
        }
        const fs::path binDir = ownPath.parent_path();
#ifdef _WIN32
        const fs::path editor = binDir / "SparkEditor.exe";
#else
        const fs::path editor = binDir / "SparkEditor";
#endif
        if (!fs::exists(editor))
        {
            m_statusMessage = "SparkEditor binary not found: " + editor.string();
            m_statusIsError = true;
            return false;
        }

#ifdef _WIN32
        std::string cmdLine = "\"" + editor.string() + "\" --project \"" + projectFilePath + "\"";
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessA(editor.string().c_str(), cmdLine.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS,
                                 nullptr, nullptr, &si, &pi);
        if (!ok)
        {
            m_statusMessage = "CreateProcess failed (err " + std::to_string(GetLastError()) + ")";
            m_statusIsError = true;
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
#else
        pid_t pid = fork();
        if (pid < 0)
        {
            m_statusMessage = "fork() failed";
            m_statusIsError = true;
            return false;
        }
        if (pid == 0)
        {
            // Detach from launcher so it can exit cleanly.
            setsid();
            execl(editor.c_str(), editor.c_str(), "--project", projectFilePath.c_str(), static_cast<char*>(nullptr));
            _exit(127); // execl only returns on failure
        }
#endif
        m_shouldClose = true;
        return true;
    }
} // namespace SparkLauncher
