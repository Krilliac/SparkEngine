/**
 * @file ProjectBrowserPanel.cpp
 * @brief Implementation of the project browser panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "ProjectBrowserPanel.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <iostream>
#include <chrono>
#include <ctime>
#include <cstring>
#include <cstdio>

#ifdef _WIN32
#include <Windows.h>
#include <ShlObj.h>
#endif

namespace fs = std::filesystem;

namespace SparkEditor
{

    ProjectBrowserPanel::ProjectBrowserPanel(ProjectManager* projectManager)
        : EditorPanel("Project Browser", "project_browser_panel"), m_projectManager(projectManager)
    {
        // Default new project path to user's Documents/SparkProjects
#ifdef _WIN32
        char docPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docPath)))
        {
            std::string defaultPath = std::string(docPath) + "\\SparkProjects";
            strncpy(m_newProjectPath, defaultPath.c_str(), sizeof(m_newProjectPath) - 1);
        }
        else
        {
            strncpy(m_newProjectPath, "C:\\SparkProjects", sizeof(m_newProjectPath) - 1);
        }
#else
        const char* home = getenv("HOME");
        if (home)
        {
            std::string defaultPath = std::string(home) + "/SparkProjects";
            strncpy(m_newProjectPath, defaultPath.c_str(), sizeof(m_newProjectPath) - 1);
        }
#endif
    }

    bool ProjectBrowserPanel::Initialize()
    {
        m_isInitialized = true;
        return true;
    }

    void ProjectBrowserPanel::Update(float /*deltaTime*/)
    {
        // Nothing to update
    }

    void ProjectBrowserPanel::Shutdown()
    {
        m_isInitialized = false;
    }

    void ProjectBrowserPanel::ShowNewProject()
    {
        m_showModal = true;
        m_activeTab = Tab::NewProject;
        m_createError.clear();
    }

    void ProjectBrowserPanel::ShowOpenProject()
    {
        m_showModal = true;
        m_activeTab = Tab::RecentProjects;
        m_openError.clear();
    }

    void ProjectBrowserPanel::ShowBrowser()
    {
        m_showModal = true;
        m_activeTab = Tab::RecentProjects;
        m_createError.clear();
        m_openError.clear();
    }

    void ProjectBrowserPanel::Render()
    {
        if (m_showModal)
        {
            RenderModal();
        }
    }

    void ProjectBrowserPanel::RenderModal()
    {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 center = ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                               viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
        ImVec2 modalSize = ImVec2(820, 560);

        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(modalSize, ImGuiCond_Always);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                                 ImGuiWindowFlags_NoSavedSettings;

        // Dim background
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            viewport->WorkPos,
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y),
            IM_COL32(0, 0, 0, 140));

        if (ImGui::Begin("Spark Engine - Project Browser###project_browser_modal", &m_showModal, flags))
        {

            // Tab bar
            if (ImGui::BeginTabBar("ProjectBrowserTabs"))
            {
                if (ImGui::BeginTabItem("Projects"))
                {
                    m_activeTab = Tab::RecentProjects;
                    RenderRecentProjects();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("New Project"))
                {
                    m_activeTab = Tab::NewProject;
                    RenderNewProjectForm();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
    }

    // ------------------------------------------------------------------
    // Recent Projects tab
    // ------------------------------------------------------------------
    void ProjectBrowserPanel::RenderRecentProjects()
    {
        if (!m_projectManager)
            return;

        const auto& recentProjects = m_projectManager->GetRecentProjects();

        ImGui::Spacing();

        // Open project button
        if (ImGui::Button("Browse...", ImVec2(100, 30)))
        {
            std::string path;
            if (BrowseForFolder(path))
            {
                if (m_projectManager->OpenProject(path))
                {
                    m_showModal = false;
                }
                else
                {
                    m_openError = "Failed to open project at: " + path;
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Open a project by browsing to its folder or .sparkproject file");

        if (!m_openError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", m_openError.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Separator();
        ImGui::Spacing();

        if (recentProjects.empty())
        {
            ImGui::Spacing();
            ImGui::Spacing();
            float windowWidth = ImGui::GetContentRegionAvail().x;
            std::string text = "No recent projects";
            float textWidth = ImGui::CalcTextSize(text.c_str()).x;
            ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
            ImGui::TextDisabled("%s", text.c_str());

            ImGui::Spacing();
            std::string hint = "Create a new project or open an existing one to get started.";
            float hintWidth = ImGui::CalcTextSize(hint.c_str()).x;
            ImGui::SetCursorPosX((windowWidth - hintWidth) * 0.5f);
            ImGui::TextDisabled("%s", hint.c_str());
            return;
        }

        // Recent projects header
        ImGui::Text("Recent Projects");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
        if (ImGui::SmallButton("Clear All"))
        {
            m_projectManager->ClearRecentProjects();
        }

        ImGui::Spacing();

        // Project list
        if (ImGui::BeginChild("RecentProjectsList", ImVec2(0, 0), ImGuiChildFlags_Borders))
        {
            int removeIdx = -1;

            for (size_t i = 0; i < recentProjects.size(); ++i)
            {
                const auto& rp = recentProjects[i];

                ImGui::PushID(static_cast<int>(i));

                // Highlight invalid projects
                if (!rp.valid)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                }

                // Clickable selectable for the whole row
                bool selected = false;
                if (ImGui::Selectable("##project_entry", &selected, ImGuiSelectableFlags_AllowDoubleClick,
                                      ImVec2(0, 50)))
                {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        if (rp.valid)
                        {
                            if (m_projectManager->OpenProject(rp.path))
                            {
                                m_showModal = false;
                            }
                            else
                            {
                                m_openError = "Failed to open: " + rp.path;
                            }
                        }
                    }
                }

                // Draw content over the selectable
                ImVec2 itemMin = ImGui::GetItemRectMin();
                ImGui::SetCursorScreenPos(ImVec2(itemMin.x + 10, itemMin.y + 6));
                ImGui::Text("%s", rp.name.c_str());

                ImGui::SetCursorScreenPos(ImVec2(itemMin.x + 10, itemMin.y + 28));
                ImGui::TextDisabled("%s", rp.path.c_str());

                // Engine version on the right
                std::string verText = rp.engineVersion.empty() ? "" : ("v" + rp.engineVersion);
                if (!verText.empty())
                {
                    float verWidth = ImGui::CalcTextSize(verText.c_str()).x;
                    ImGui::SetCursorScreenPos(
                        ImVec2(itemMin.x + ImGui::GetContentRegionAvail().x - verWidth - 60, itemMin.y + 6));
                    ImGui::TextDisabled("%s", verText.c_str());
                }

                // Remove button
                ImGui::SetCursorScreenPos(ImVec2(itemMin.x + ImGui::GetContentRegionAvail().x - 30, itemMin.y + 14));
                if (ImGui::SmallButton("X"))
                {
                    removeIdx = static_cast<int>(i);
                }

                if (!rp.valid)
                {
                    ImGui::PopStyleColor();
                    // Show missing indicator
                    ImGui::SetCursorScreenPos(
                        ImVec2(itemMin.x + ImGui::GetContentRegionAvail().x - 100, itemMin.y + 28));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.2f, 1.0f));
                    ImGui::TextUnformatted("(missing)");
                    ImGui::PopStyleColor();
                }

                ImGui::PopID();
                ImGui::Separator();
            }

            if (removeIdx >= 0)
            {
                m_projectManager->RemoveRecentProject(recentProjects[removeIdx].path);
            }
        }
        ImGui::EndChild();
    }

    // ------------------------------------------------------------------
    // New Project tab
    // ------------------------------------------------------------------
    void ProjectBrowserPanel::RenderNewProjectForm()
    {
        ImGui::Spacing();

        // Template selection
        ImGui::Text("Choose a Template:");
        ImGui::Spacing();

        if (ImGui::BeginChild("TemplateSelection", ImVec2(0, 170), ImGuiChildFlags_Borders))
        {
            float cardWidth = 140.0f;
            float spacing = 10.0f;
            float totalWidth = ImGui::GetContentRegionAvail().x;
            int cols = std::max(1, static_cast<int>((totalWidth + spacing) / (cardWidth + spacing)));

            int idx = 0;
            ProjectTemplate templates[] = {ProjectTemplate::Blank3D, ProjectTemplate::FirstPerson,
                                           ProjectTemplate::ThirdPerson, ProjectTemplate::TopDown,
                                           ProjectTemplate::Empty};
            const char* icons[] = {"[ ]", "[FP]", "[TP]", "[TD]", "[ ]"};

            for (int i = 0; i < 5; ++i)
            {
                if (idx > 0 && (idx % cols) != 0)
                {
                    ImGui::SameLine(0, spacing);
                }
                RenderTemplateCard(templates[i], icons[i]);
                idx++;
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Project details
        ImGui::Text("Project Settings:");
        ImGui::Spacing();

        ImGui::SetNextItemWidth(400);
        ImGui::InputText("Project Name", m_newProjectName, sizeof(m_newProjectName));

        ImGui::SetNextItemWidth(400);
        ImGui::InputText("Location", m_newProjectPath, sizeof(m_newProjectPath));
        ImGui::SameLine();
        if (ImGui::Button("Browse...##location"))
        {
            std::string path;
            if (BrowseForFolder(path))
            {
                strncpy(m_newProjectPath, path.c_str(), sizeof(m_newProjectPath) - 1);
            }
        }

        // Show full project path preview
        if (strlen(m_newProjectName) > 0 && strlen(m_newProjectPath) > 0)
        {
            std::string fullPath = std::string(m_newProjectPath) + "/" + m_newProjectName;
            ImGui::TextDisabled("Project will be created at: %s", fullPath.c_str());
        }

        ImGui::SetNextItemWidth(400);
        ImGui::InputText("Description (optional)", m_newProjectDescription, sizeof(m_newProjectDescription));

        ImGui::Spacing();

        // Error message
        if (!m_createError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", m_createError.c_str());
            ImGui::PopStyleColor();
        }

        // Create button
        ImGui::Spacing();
        bool canCreate = strlen(m_newProjectName) > 0 && strlen(m_newProjectPath) > 0;
        if (!canCreate)
        {
            ImGui::BeginDisabled();
        }

        if (ImGui::Button("Create Project", ImVec2(160, 36)))
        {
            m_createError.clear();

            // Validate project name (no special filesystem characters)
            std::string name(m_newProjectName);
            for (char c : name)
            {
                if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
                    c == '|')
                {
                    m_createError = "Project name contains invalid characters.";
                    break;
                }
            }

            if (m_createError.empty())
            {
                if (m_projectManager->CreateProject(name, m_newProjectPath, m_selectedTemplate,
                                                    m_newProjectDescription))
                {
                    m_showModal = false;
                    std::cout << "Project created successfully!\n";
                }
                else
                {
                    m_createError = "Failed to create project. Check that the path is writable "
                                    "and the directory doesn't already contain a project.";
                }
            }
        }

        if (!canCreate)
        {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 36)))
        {
            m_showModal = false;
        }
    }

    void ProjectBrowserPanel::RenderTemplateCard(ProjectTemplate tmpl, const char* icon)
    {
        bool isSelected = (m_selectedTemplate == tmpl);
        std::string name = ProjectManager::GetProjectTemplateName(tmpl);
        std::string desc = ProjectManager::GetProjectTemplateDescription(tmpl);

        ImGui::PushID(static_cast<int>(tmpl));

        ImVec2 cardSize(130, 130);

        // Card background
        if (isSelected)
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.2f, 0.4f, 0.7f, 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.5f, 0.9f, 1.0f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
        }

        if (ImGui::BeginChild("##card", cardSize, ImGuiChildFlags_Borders))
        {
            // Make the child window clickable
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                m_selectedTemplate = tmpl;
            }

            // Center icon
            float iconWidth = ImGui::CalcTextSize(icon).x;
            ImGui::SetCursorPosX((cardSize.x - iconWidth) * 0.5f);
            ImGui::SetCursorPosY(20);
            ImGui::Text("%s", icon);

            // Center name
            float nameWidth = ImGui::CalcTextSize(name.c_str()).x;
            ImGui::SetCursorPosX((cardSize.x - nameWidth) * 0.5f);
            ImGui::SetCursorPosY(60);
            ImGui::Text("%s", name.c_str());

            // Short description
            ImGui::SetCursorPosY(85);
            ImGui::PushTextWrapPos(cardSize.x - 5);
            ImGui::TextDisabled("%s", desc.substr(0, 60).c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();

        ImGui::PopStyleColor(2);
        ImGui::PopID();
    }

    bool ProjectBrowserPanel::BrowseForFolder(std::string& outPath)
    {
#ifdef _WIN32
        BROWSEINFOA bi = {};
        bi.lpszTitle = "Select Project Folder";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
        if (pidl)
        {
            char path[MAX_PATH];
            if (SHGetPathFromIDListA(pidl, path))
            {
                outPath = path;
                CoTaskMemFree(pidl);
                return true;
            }
            CoTaskMemFree(pidl);
        }
        return false;
#else
        // On Linux/macOS, try zenity or kdialog for a native folder chooser
        FILE* pipe = popen("zenity --file-selection --directory --title=\"Select Project Folder\" 2>/dev/null", "r");
        if (!pipe)
        {
            // Try kdialog as fallback
            pipe = popen("kdialog --getexistingdirectory ~ 2>/dev/null", "r");
        }
        if (pipe)
        {
            char buf[1024];
            std::string result;
            while (fgets(buf, sizeof(buf), pipe) != nullptr)
            {
                result += buf;
            }
            int status = pclose(pipe);
            // Remove trailing newline
            while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
            {
                result.pop_back();
            }
            if (status == 0 && !result.empty())
            {
                outPath = result;
                return true;
            }
        }
        return false;
#endif
    }

} // namespace SparkEditor
