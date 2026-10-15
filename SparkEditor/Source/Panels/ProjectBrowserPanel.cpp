/**
 * @file ProjectBrowserPanel.cpp
 * @brief Implementation of the project browser panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "ProjectBrowserPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include "../Utils/FolderDialog.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <iostream>
#include <chrono>
#include <ctime>
#include <cstring>
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace SparkEditor
{
    namespace
    {
        /// @brief Template card size: an icon row, a name row and three description rows, so the
        ///        card grows with the font instead of clipping at larger UI scales.
        ImVec2 TemplateCardSize()
        {
            const float line = ImGui::GetTextLineHeightWithSpacing();
            return ImVec2(Spark::Ui::Em(9.0f), Spark::Ui::Px(12.0f) * 2.0f + line * 6.0f);
        }
    } // namespace

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
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "ProjectBrowserPanel initialized");
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

    bool ProjectBrowserPanel::RequestOpenProject(const std::string& projectPath)
    {
        if (m_openProjectRequestHandler)
            return m_openProjectRequestHandler(projectPath);
        return m_projectManager && m_projectManager->OpenProject(projectPath);
    }

    bool ProjectBrowserPanel::RequestCreateProject(const std::string& projectName, const std::string& parentDirectory,
                                                   ProjectTemplate templateType, const std::string& description)
    {
        if (m_createProjectRequestHandler)
            return m_createProjectRequestHandler(projectName, parentDirectory, templateType, description);
        return m_projectManager &&
               m_projectManager->CreateProject(projectName, parentDirectory, templateType, description);
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
        ImVec2 modalSize = Spark::Ui::Px2(820.0f, 560.0f);

        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(modalSize, ImGuiCond_Always);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                                 ImGuiWindowFlags_NoSavedSettings;

        // Dim background
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            viewport->WorkPos,
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y),
            Spark::Ui::Col(Spark::Ui::Role::ModalScrim));

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
        if (ImGui::Button("Browse...", Spark::Ui::Px2(100, 30)))
        {
            std::string path;
            if (BrowseForFolder(path))
            {
                if (RequestOpenProject(path))
                {
                    SPARK_LOG_INFO(Spark::LogCategory::Editor, "ProjectBrowserPanel: accepted project request at '%s'",
                                   path.c_str());
                    m_showModal = false;
                }
                else
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Editor, "ProjectBrowserPanel: failed to open project at '%s'",
                                    path.c_str());
                    m_openError = "Failed to open project at: " + path;
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Open a project by browsing to its folder or .sparkproject file");

        if (!m_openError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Spark::Ui::ColV(Spark::Ui::Role::ErrorText));
            ImGui::TextWrapped("%s", m_openError.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Separator();
        ImGui::Spacing();

        if (recentProjects.empty())
        {
            Spark::Ui::EmptyState(ICON_FA_FOLDER_OPEN, "No recent projects",
                                  "Create a new project or open an existing one to get started.");
            return;
        }

        // Recent projects header
        // "Clear All" sits flush right, measured from its own label so it never clips.
        const float headerRightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const float clearAllWidth = ImGui::CalcTextSize("Clear All").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::Text("Recent Projects");
        ImGui::SameLine(headerRightEdge - clearAllWidth);
        if (ImGui::SmallButton("Clear All"))
        {
            m_projectManager->ClearRecentProjects();
        }

        ImGui::Spacing();

        // Project list
        if (ImGui::BeginChild("RecentProjectsList", ImVec2(0, 0), ImGuiChildFlags_Borders))
        {
            int removeIdx = -1;

            // Two text rows per entry; every offset derives from the font so rows never overlap.
            const float rowPad = Spark::Ui::Px(6.0f);
            const float rowIndent = Spark::Ui::Px(10.0f);
            const float lineStep = ImGui::GetTextLineHeightWithSpacing();
            const float rowHeight = rowPad * 2.0f + lineStep + ImGui::GetTextLineHeight();
            const float removeWidth = ImGui::CalcTextSize(ICON_FA_TIMES).x + ImGui::GetStyle().FramePadding.x * 2.0f;

            for (size_t i = 0; i < recentProjects.size(); ++i)
            {
                const auto& rp = recentProjects[i];

                ImGui::PushID(static_cast<int>(i));

                // Highlight invalid projects
                if (!rp.valid)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, Spark::Ui::ColV(Spark::Ui::Role::TextSecondary));
                }

                // Clickable selectable for the whole row
                bool selected = false;
                if (ImGui::Selectable("##project_entry", &selected, ImGuiSelectableFlags_AllowDoubleClick,
                                      ImVec2(0.0f, rowHeight)))
                {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        if (rp.valid)
                        {
                            if (RequestOpenProject(rp.path))
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
                const float rowRight = itemMin.x + ImGui::GetContentRegionAvail().x;
                const float secondLineY = itemMin.y + rowPad + lineStep;
                ImGui::SetCursorScreenPos(ImVec2(itemMin.x + rowIndent, itemMin.y + rowPad));
                ImGui::Text("%s", rp.name.c_str());

                ImGui::SetCursorScreenPos(ImVec2(itemMin.x + rowIndent, secondLineY));
                Spark::Ui::SecondaryText("%s", rp.path.c_str());

                // Engine version on the right
                std::string verText = rp.engineVersion.empty() ? "" : ("v" + rp.engineVersion);
                if (!verText.empty())
                {
                    float verWidth = ImGui::CalcTextSize(verText.c_str()).x;
                    ImGui::SetCursorScreenPos(
                        ImVec2(rowRight - verWidth - removeWidth - rowIndent * 2.0f, itemMin.y + rowPad));
                    Spark::Ui::SecondaryText("%s", verText.c_str());
                }

                // Remove button
                ImGui::SetCursorScreenPos(ImVec2(rowRight - removeWidth - rowIndent,
                                                 itemMin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f));
                if (ImGui::SmallButton(ICON_FA_TIMES))
                {
                    removeIdx = static_cast<int>(i);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Remove from recent projects");
                }

                if (!rp.valid)
                {
                    ImGui::PopStyleColor();
                    // Show missing indicator
                    const float missingWidth =
                        ImGui::CalcTextSize(Spark::Ui::StatusGlyph(Spark::Ui::Status::Warning)).x +
                        Spark::Ui::Em(0.4f) + ImGui::CalcTextSize("Missing").x;
                    ImGui::SetCursorScreenPos(
                        ImVec2(rowRight - missingWidth - removeWidth - rowIndent * 2.0f, secondLineY));
                    Spark::Ui::StatusText(Spark::Ui::Status::Warning, "Missing");
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
        Spark::Ui::SubHeader("Choose a Template");

        // One row of cards plus padding and a horizontal scrollbar's worth of slack.
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec2 cardSize = TemplateCardSize();
        const float selectionHeight = cardSize.y + style.WindowPadding.y * 2.0f + style.ScrollbarSize;
        if (ImGui::BeginChild("TemplateSelection", ImVec2(0.0f, selectionHeight), ImGuiChildFlags_Borders))
        {
            float cardWidth = cardSize.x;
            float spacing = style.ItemSpacing.x;
            float totalWidth = ImGui::GetContentRegionAvail().x;
            int cols = std::max(1, static_cast<int>((totalWidth + spacing) / (cardWidth + spacing)));

            int idx = 0;
            for (const auto& descriptor : ProjectManager::GetProjectTemplateDescriptors())
            {
                if (idx > 0 && (idx % cols) != 0)
                {
                    ImGui::SameLine(0, spacing);
                }
                RenderTemplateCard(descriptor.type, descriptor.iconText.data());
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

        ImGui::SetNextItemWidth(Spark::Ui::Em(24.0f));
        ImGui::InputText("Project Name", m_newProjectName, sizeof(m_newProjectName));

        ImGui::SetNextItemWidth(Spark::Ui::Em(24.0f));
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

        ImGui::SetNextItemWidth(Spark::Ui::Em(24.0f));
        ImGui::InputText("Description (optional)", m_newProjectDescription, sizeof(m_newProjectDescription));

        ImGui::Spacing();

        // Error message
        if (!m_createError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Spark::Ui::ColV(Spark::Ui::Role::ErrorText));
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

        if (ImGui::Button("Create Project", Spark::Ui::Px2(160, 36)))
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
                if (RequestCreateProject(name, m_newProjectPath, m_selectedTemplate, m_newProjectDescription))
                {
                    m_showModal = false;
                    SPARK_LOG_INFO(Spark::LogCategory::Editor,
                                   "ProjectBrowserPanel: accepted create request for '%s' at '%s'", name.c_str(),
                                   m_newProjectPath);
                }
                else
                {
                    m_createError = "Failed to create project. Check that the path is writable "
                                    "and the directory doesn't already contain a project.";
                    SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                                    "ProjectBrowserPanel: failed to create project '%s' at '%s'", name.c_str(),
                                    m_newProjectPath);
                }
            }
        }

        if (!canCreate)
        {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", Spark::Ui::Px2(100, 36)))
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

        const ImVec2 cardSize = TemplateCardSize();
        const float cardPad = Spark::Ui::Px(12.0f);
        const float line = ImGui::GetTextLineHeightWithSpacing();

        // Card background: the selected card is filled and outlined in the accent roles, and a
        // thicker border marks it too so the selection never relies on hue alone.
        const Spark::Ui::ScopedRoleColor cardBg(ImGuiCol_ChildBg, isSelected ? Spark::Ui::Role::SurfaceSelected
                                                                             : Spark::Ui::Role::SurfaceRaised);
        const Spark::Ui::ScopedRoleColor cardBorder(ImGuiCol_Border, isSelected ? Spark::Ui::Role::AccentFill
                                                                                : Spark::Ui::Role::BorderSubtle);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, isSelected ? 2.0f * Spark::Ui::Scale() : 1.0f);

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
            ImGui::SetCursorPosY(cardPad);
            ImGui::Text("%s", icon);

            // Center name
            float nameWidth = ImGui::CalcTextSize(name.c_str()).x;
            ImGui::SetCursorPosX((cardSize.x - nameWidth) * 0.5f);
            ImGui::SetCursorPosY(cardPad + line * 1.5f);
            ImGui::Text("%s", name.c_str());

            // Short description
            ImGui::SetCursorPosY(cardPad + line * 2.75f);
            ImGui::PushTextWrapPos(cardSize.x - ImGui::GetStyle().WindowPadding.x);
            Spark::Ui::SecondaryText("%s", desc.substr(0, 60).c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();

        ImGui::PopStyleVar();
        ImGui::PopID();
    }

    bool ProjectBrowserPanel::BrowseForFolder(std::string& outPath)
    {
        return SparkEditor::Utils::BrowseForFolder(outPath, "Select Project Folder");
    }

} // namespace SparkEditor
