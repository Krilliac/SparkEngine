/**
 * @file EditorMenuBar.cpp
 * @brief Main menu bar and toolbar rendering for the SparkEditor UI
 *
 * Contains EditorUI::RenderMainMenuBar() and EditorUI::RenderToolbar().
 * Split from EditorUI.cpp for maintainability.
 */
#include "EditorUI.h"
#include "EditorPanel.h"
#include "EditorPluginManager.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "../Panels/HierarchyPanel.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "EditorApplication.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cerrno>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#else
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace SparkEditor
{
#ifndef _WIN32
    namespace
    {
        bool LaunchDocumentationViewer()
        {
            const pid_t pid = fork();
            if (pid == -1)
                return false;
            if (pid == 0)
            {
                if (setsid() == -1)
                    _exit(127);
                const pid_t detachedPid = fork();
                if (detachedPid == -1)
                    _exit(127);
                if (detachedPid != 0)
                    _exit(0);
#ifdef __APPLE__
                execlp("open", "open", "docs/", nullptr);
#else
                execlp("xdg-open", "xdg-open", "docs/", nullptr);
#endif
                _exit(127);
            }

            int status = 0;
            while (waitpid(pid, &status, 0) == -1)
            {
                if (errno == EINTR)
                    continue;
                return errno == ECHILD;
            }
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
    } // namespace
#endif

    void EditorUI::ShowOpenSceneDialog()
    {
        RequestDocumentTransition(DocumentTransitionAction::OpenSceneDialog);
    }

    void EditorUI::ShowOpenSceneDialogNow()
    {
#ifdef _WIN32
        wchar_t pathBuffer[32768] = {};
        std::wstring initialDirectory;
        if (m_projectManager && m_projectManager->HasOpenProject())
            initialDirectory = std::filesystem::path(m_projectManager->GetProjectScenesPath()).wstring();
        const wchar_t filter[] = L"Spark Scenes (*.sparkscene)\0*.sparkscene\0All Files (*.*)\0*.*\0\0";
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.lpstrFile = pathBuffer;
        dialog.nMaxFile = static_cast<DWORD>(std::size(pathBuffer));
        dialog.lpstrFilter = filter;
        dialog.lpstrInitialDir = initialDirectory.empty() ? nullptr : initialDirectory.c_str();
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog) && !OpenScene(std::filesystem::path(pathBuffer).string()))
            ShowNotification("Failed to open scene", "error");
#else
        ShowNotification("Open Scene dialog is not available on this platform", "warning");
#endif
    }

    void EditorUI::RenderMainMenuBar()
    {
        if (ImGui::BeginMainMenuBar())
        {
            RenderFileMenu();
            RenderEditMenu();
            RenderGameObjectMenu();
            RenderWindowMenu();
            RenderFPSToolsMenu();
            RenderBuildMenu();
            RenderHelpMenu();

            // Render plugin-contributed menu bar items
            if (m_pluginManager)
            {
                m_pluginManager->RenderMenuBarItems();
            }

            ImGui::EndMainMenuBar();
        }
    }

    void EditorUI::RenderFileSceneItems()
    {
        if (ImGui::MenuItem("New Scene", "Ctrl+N"))
        {
            NewScene();
        }
        if (ImGui::MenuItem("Open Scene...", "Ctrl+O"))
            ShowOpenSceneDialog();
        if (ImGui::MenuItem("Save Scene", "Ctrl+S"))
        {
            SaveScene();
        }
    }

    void EditorUI::RenderFileProjectItems()
    {
        if (ImGui::MenuItem("New Project..."))
        {
            ShowNewProjectDialog();
        }
        if (ImGui::MenuItem("Open Project..."))
        {
            ShowOpenProjectDialog();
        }
        if (ImGui::MenuItem("Save Project", nullptr, false, m_projectManager && m_projectManager->HasOpenProject()))
        {
            if (m_projectManager->SaveProject())
            {
                ShowNotification("Project saved!", "success");
            }
            else
            {
                ShowNotification("Failed to save project", "error");
            }
        }

        if (m_projectManager && !m_projectManager->GetRecentProjects().empty())
        {
            if (ImGui::BeginMenu("Recent Projects"))
            {
                for (const auto& rp : m_projectManager->GetRecentProjects())
                {
                    std::string label = rp.name + "  (" + rp.path + ")";
                    if (!rp.valid)
                        label += "  [missing]";
                    if (ImGui::MenuItem(label.c_str(), nullptr, false, rp.valid))
                    {
                        // The request may be waiting on Save/Discard/Cancel;
                        // the project-open callback reports success only after
                        // the transition actually commits.
                        RequestOpenProject(rp.path);
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear Recent Projects"))
                {
                    m_projectManager->ClearRecentProjects();
                }
                ImGui::EndMenu();
            }
        }
    }

    void EditorUI::RenderFileMenu()
    {
        if (!ImGui::BeginMenu("File"))
            return;

        RenderFileSceneItems();
        ImGui::Separator();
        RenderFileProjectItems();

        ImGui::Separator();
        if (ImGui::MenuItem("Import Asset"))
        {
            SetPanelVisible("AssetBrowser", true);
            ShowNotification("Use the Asset Browser Import button to add assets", "info");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Build Settings..."))
        {
            SetPanelVisible("BuildCook", true);
            ShowNotification("Build & Cook settings opened", "info");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4"))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Exit requested via File menu");
            RequestExitWithConfirmation();
        }
        ImGui::EndMenu();
    }

    void EditorUI::RenderEditMenu()
    {
        if (!ImGui::BeginMenu("Edit"))
            return;

        // Operates on the process-wide CommandHistory — the same history the
        // edit surfaces execute into and the one SwapWorld() clears.
        auto& history = Spark::Editor::CommandHistory::GetInstance();
        const bool canUndo = history.CanUndo();
        const bool canRedo = history.CanRedo();
        const std::string undoDesc = history.GetUndoDescription();
        const std::string redoDesc = history.GetRedoDescription();
        std::string undoLabel = "Undo";
        std::string redoLabel = "Redo";
        if (canUndo)
        {
            undoLabel += " (" + undoDesc + ")";
        }
        if (canRedo)
        {
            redoLabel += " (" + redoDesc + ")";
        }

        if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, canUndo))
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Undo: %s", undoDesc.c_str());
            history.Undo();
            ShowNotification("Undo: " + undoDesc, "info");
        }
        if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, canRedo))
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Redo: %s", redoDesc.c_str());
            history.Redo();
            ShowNotification("Redo: " + redoDesc, "info");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X"))
        {
            ShowNotification("Cut operation!", "info");
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C"))
        {
            ShowNotification("Copy operation!", "info");
        }
        if (ImGui::MenuItem("Paste", "Ctrl+V"))
        {
            ShowNotification("Paste operation!", "info");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", "Ctrl+A"))
        {
            ShowNotification("Select All operation!", "info");
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_SEARCH " Search...", "Ctrl+F"))
        {
            SetPanelVisible("Search", true);
        }
        if (ImGui::MenuItem(ICON_FA_BOLT " Command Palette", "Ctrl+P"))
        {
            if (m_commandPalette)
            {
                m_commandPalette->Open();
            }
        }
        ImGui::EndMenu();
    }

    void EditorUI::RenderGameObject3DSubMenu(const std::function<void(const std::string&)>& createObject)
    {
        if (ImGui::BeginMenu("3D Object"))
        {
            if (ImGui::MenuItem("Cube"))
                createObject("Cube");
            if (ImGui::MenuItem("Sphere"))
                createObject("Sphere");
            if (ImGui::MenuItem("Cylinder"))
                createObject("Cylinder");
            if (ImGui::MenuItem("Plane"))
                createObject("Plane");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Light"))
        {
            if (ImGui::MenuItem("Directional Light"))
                createObject("Directional Light");
            if (ImGui::MenuItem("Point Light"))
                createObject("Point Light");
            if (ImGui::MenuItem("Spot Light"))
                createObject("Spot Light");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Camera"))
        {
            if (ImGui::MenuItem("Camera"))
                createObject("Camera");
            ImGui::EndMenu();
        }
    }

    void EditorUI::RenderGameObject2DSubMenu()
    {
        if (!ImGui::BeginMenu("2D Object"))
            return;

        const char* items[] = {"Sprite",    "Animated Sprite",     "Tilemap",
                               "Camera 2D", "Parallax Background", "Nine-Slice Sprite"};
        for (const char* item : items)
        {
            if (ImGui::MenuItem(item))
            {
                const bool created = CreateDocumentEntity(item);
                ShowNotification(created ? std::string("Created ") + item : std::string("Unsupported object: ") + item,
                                 created ? "success" : "error");
            }
        }
        ImGui::EndMenu();
    }

    void EditorUI::RenderGameObjectVolumeSubMenu(const std::function<void(const std::string&)>& createObject)
    {
        if (ImGui::BeginMenu("Volume"))
        {
            if (ImGui::MenuItem("Trigger Volume"))
                createObject("Trigger Volume");
            if (ImGui::MenuItem("Post-Process Volume"))
                createObject("Post-Process Volume");
            if (ImGui::MenuItem("Fog Volume"))
                createObject("Fog Volume");
            if (ImGui::MenuItem("Audio Reverb Zone"))
                createObject("Audio Reverb Zone");
            if (ImGui::MenuItem("Wind Zone"))
                createObject("Wind Zone");
            if (ImGui::MenuItem("Cinematic Trigger"))
                createObject("Cinematic Trigger");
            if (ImGui::MenuItem("Area Boundary"))
                createObject("Area Boundary");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Probe"))
        {
            if (ImGui::MenuItem("Reflection Probe"))
                createObject("Reflection Probe");
            if (ImGui::MenuItem("Light Probe"))
                createObject("Light Probe");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Environment"))
        {
            if (ImGui::MenuItem("Water Plane"))
                createObject("Water Plane");
            if (ImGui::MenuItem("Spawn Point"))
                createObject("Spawn Point");
            if (ImGui::MenuItem("NavMesh Obstacle"))
                createObject("NavMesh Obstacle");
            if (ImGui::MenuItem("Occluder"))
                createObject("Occluder");
            if (ImGui::MenuItem("Billboard"))
                createObject("Billboard");
            ImGui::EndMenu();
        }
    }

    void EditorUI::RenderGameObjectSpecializedSubMenus(const std::function<void(const std::string&)>& createObject)
    {
        if (ImGui::BeginMenu("Gameplay"))
        {
            if (ImGui::MenuItem("Destructible"))
                createObject("Destructible");
            if (ImGui::MenuItem("Dialogue Trigger"))
                createObject("Dialogue Trigger");
            if (ImGui::MenuItem("Physics Joint"))
                createObject("Physics Joint");
            if (ImGui::MenuItem("Character Controller"))
                createObject("Character Controller");
            if (ImGui::MenuItem("Vehicle"))
                createObject("Vehicle");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("AI"))
        {
            if (ImGui::MenuItem("Cover Point"))
                createObject("Cover Point");
            if (ImGui::MenuItem("Tactical Point"))
                createObject("Tactical Point");
            if (ImGui::MenuItem("Nav Region"))
                createObject("Nav Region");
            if (ImGui::MenuItem("Nav Link"))
                createObject("Nav Link");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Rendering"))
        {
            if (ImGui::MenuItem("Skybox"))
                createObject("Skybox");
            if (ImGui::MenuItem("Line Renderer"))
                createObject("Line Renderer");
            if (ImGui::MenuItem("Trail Renderer"))
                createObject("Trail Renderer");
            if (ImGui::MenuItem("Text 3D"))
                createObject("Text 3D");
            if (ImGui::MenuItem("Billboard"))
                createObject("Billboard");
            if (ImGui::MenuItem("Foliage Volume"))
                createObject("Foliage Volume");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Physics"))
        {
            if (ImGui::MenuItem("Ragdoll"))
                createObject("Ragdoll");
            if (ImGui::MenuItem("Soft Body / Cloth"))
                createObject("Soft Body");
            if (ImGui::MenuItem("Constant Force"))
                createObject("Constant Force");
            if (ImGui::MenuItem("Force Region"))
                createObject("Force Region");
            if (ImGui::MenuItem("Buoyancy Volume"))
                createObject("Buoyancy Volume");
            if (ImGui::MenuItem("Spring Arm"))
                createObject("Spring Arm");
            ImGui::EndMenu();
        }
    }

    void EditorUI::RenderGameObjectMenu()
    {
        if (!ImGui::BeginMenu("GameObject"))
            return;

        auto createObject = [this](const std::string& name)
        {
            const bool created = CreateDocumentEntity(name);
            ShowNotification(created ? "Created " + name : "Unsupported object: " + name, created ? "success" : "error",
                             2.0f);
        };

        if (ImGui::MenuItem("Create Empty"))
        {
            createObject("Empty");
        }
        if (ImGui::MenuItem(ICON_FA_CUBE " Create Prefab from Selection"))
        {
            if (m_prefabManager)
            {
                m_prefabManager->CreatePrefabFromEntity(0, "New Prefab");
                ShowNotification("Prefab created from selection!", "success");
            }
        }
        if (ImGui::BeginMenu(ICON_FA_CUBE " Instantiate Prefab"))
        {
            if (m_prefabManager)
            {
                auto names = m_prefabManager->GetPrefabNames();
                for (const auto& name : names)
                {
                    if (ImGui::MenuItem(name.c_str()))
                    {
                        m_prefabManager->InstantiatePrefab(name);
                        ShowNotification("Instantiated prefab: " + name, "success");
                    }
                }
                if (names.empty())
                {
                    ImGui::TextDisabled("No prefabs available");
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();

        RenderGameObject3DSubMenu(createObject);
        RenderGameObject2DSubMenu();
        RenderGameObjectVolumeSubMenu(createObject);
        RenderGameObjectSpecializedSubMenus(createObject);

        ImGui::EndMenu();
    }

    // Category-driven Window menu: groups all panels by their PanelCategory
    // instead of hardcoded submenu functions. Adding a new panel only requires
    // registering it in the factory and assigning a category — no menu changes.
    void EditorUI::RenderWindowMenu()
    {
        if (!ImGui::BeginMenu("Window"))
            return;

        struct CategoryInfo
        {
            PanelCategory cat;
            const char* label;
        };
        constexpr CategoryInfo categories[] = {
            {PanelCategory::Viewport, "Viewports"},
            {PanelCategory::Inspector, "Inspectors"},
            {PanelCategory::Tool, "Tools"},
            {PanelCategory::Config, "Configuration"},
            {PanelCategory::Debug, "Debug & Profiling"},
            {PanelCategory::Other, "Other"},
        };

        for (const auto& [cat, label] : categories)
        {
            // Collect panels in this category
            std::vector<std::pair<std::string, EditorPanel*>> catPanels;
            for (auto& [name, panel] : m_panels)
            {
                if (panel->GetCategory() == cat && panel->IsVisibleInMenu())
                    catPanels.emplace_back(name, panel.get());
            }
            if (catPanels.empty())
                continue;

            // Sort alphabetically for consistent ordering
            std::sort(catPanels.begin(), catPanels.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });

            if (ImGui::BeginMenu(label))
            {
                for (auto& [name, panel] : catPanels)
                {
                    std::string menuLabel =
                        panel->GetIcon().empty() ? panel->GetName() : panel->GetIcon() + " " + panel->GetName();
                    if (ImGui::MenuItem(menuLabel.c_str(), nullptr, panel->IsVisible()))
                        panel->SetVisible(!panel->IsVisible());
                }
                ImGui::EndMenu();
            }
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout"))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Layout reset to default");
            ResetToDefaultLayout();
            ShowNotification("Layout reset!", "success");
        }
        if (ImGui::MenuItem("Save Layout"))
        {
            if (SaveLayout("Custom Layout"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Layout saved: Custom Layout");
                ShowNotification("Layout saved!", "success");
            }
            else
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Could not save layout: Custom Layout");
                ShowNotification("Layout save failed", "error");
            }
        }
        if (ImGui::BeginMenu("Load Layout"))
        {
            const auto layouts = m_layoutManager ? m_layoutManager->GetSavedLayouts() : std::vector<LayoutInfo>{};
            if (layouts.empty())
                ImGui::TextDisabled("No saved layouts");
            for (const LayoutInfo& layout : layouts)
            {
                if (ImGui::MenuItem(layout.name.c_str()))
                {
                    const bool loaded = LoadLayout(layout.name);
                    ShowNotification(loaded ? "Layout loaded: " + layout.name : "Layout load failed: " + layout.name,
                                     loaded ? "success" : "error");
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    void EditorUI::RenderFPSToolsMenu()
    {
        if (!ImGui::BeginMenu("FPS Tools"))
            return;

        if (ImGui::MenuItem(ICON_FA_CROSSHAIRS " Weapon Editor", nullptr, IsPanelVisible("WeaponEditor")))
        {
            SetPanelVisible("WeaponEditor", !IsPanelVisible("WeaponEditor"));
            if (IsPanelVisible("WeaponEditor"))
            {
                ShowNotification("Weapon Editor opened!", "success");
            }
        }
        if (ImGui::MenuItem(ICON_FA_ROCKET " FPS Tools Panel", nullptr, IsPanelVisible("FPSTools")))
        {
            SetPanelVisible("FPSTools", !IsPanelVisible("FPSTools"));
            if (IsPanelVisible("FPSTools"))
            {
                ShowNotification("FPS Tools panel opened!", "success");
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_FLAG " Spawn Points"))
        {
            SetPanelVisible("FPSTools", true);
            ShowNotification("Open FPS Tools panel > Spawns tab", "info");
        }
        if (ImGui::MenuItem(ICON_FA_BULLSEYE " Objectives"))
        {
            SetPanelVisible("FPSTools", true);
            ShowNotification("Open FPS Tools panel > Level tab", "info");
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_BOMB " Explosive Volumes"))
        {
            SetPanelVisible("FPSTools", true);
            ShowNotification("Open FPS Tools panel > Level tab", "info");
        }
        if (ImGui::MenuItem(ICON_FA_SHIELD " Cover Points"))
        {
            SetPanelVisible("FPSTools", true);
            ShowNotification("Open FPS Tools panel > Level tab", "info");
        }
        ImGui::EndMenu();
    }

    void EditorUI::RenderBuildMenu()
    {
        if (!ImGui::BeginMenu("Build"))
            return;

        if (ImGui::MenuItem(ICON_FA_COG " Build Settings..."))
        {
            SetPanelVisible("BuildCook", true);
            ShowNotification("Build & Cook panel opened", "info");
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_LIGHTBULB " Build Lighting"))
        {
            ShowNotification("Build Lighting started...", "info");
        }
        if (ImGui::MenuItem(ICON_FA_MAP " Build NavMesh"))
        {
            ShowNotification("Build NavMesh started...", "info");
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_HAMMER " Build All"))
        {
            ShowNotification("Build All started...", "info");
        }
        if (ImGui::MenuItem(ICON_FA_FIRE " Cook Content"))
        {
            SetPanelVisible("BuildCook", true);
            ShowNotification("Open Build & Cook panel to configure cooking", "info");
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_ROCKET " Package Project..."))
        {
            SetPanelVisible("BuildCook", true);
            ShowNotification("Configure packaging in Build & Cook panel", "info");
        }
        ImGui::EndMenu();
    }

    void EditorUI::RenderHelpMenu()
    {
        if (!ImGui::BeginMenu("Help"))
            return;

        if (ImGui::MenuItem("Show Demo Window", nullptr, m_showDemoWindow))
        {
            m_showDemoWindow = !m_showDemoWindow;
        }
        if (ImGui::BeginMenu("Themes"))
        {
            auto themes = EditorTheme::GetAvailableThemes();
            for (const auto& name : themes)
            {
                bool isSelected = (m_currentTheme == name);
                if (ImGui::MenuItem(name.c_str(), nullptr, isSelected))
                {
                    ApplyTheme(name);
                    ShowNotification("Theme: " + name, "success", 2.0f);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("About"))
        {
            ShowNotification(ICON_FA_BOLT " Spark Engine Editor v1.0 — FPS Game Engine", "info", 5.0f);
        }
        if (ImGui::MenuItem("Documentation"))
        {
#ifdef _WIN32
            ShellExecuteA(nullptr, "open", "docs", nullptr, nullptr, SW_SHOWNORMAL);
            ShowNotification("Opening documentation...", "info", 2.0f);
#else
            if (LaunchDocumentationViewer())
                ShowNotification("Opening documentation...", "info", 2.0f);
            else
                ShowNotification("Could not launch the documentation viewer", "error", 3.0f);
#endif
        }
        ImGui::EndMenu();
    }

    void EditorUI::RenderToolbarTransformTools(float btnSize, ImDrawList* dl, const ImVec4& accentTeal,
                                               const ImVec4& pillBg)
    {
        ImVec2 btnDim(btnSize, btnSize);

        auto ToolButton = [&](const char* icon, TransformTool tool, const char* tooltip)
        {
            bool active = (m_currentTool == tool);
            if (active)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, accentTeal);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(accentTeal.x * 1.1f, accentTeal.y * 1.1f, accentTeal.z * 1.1f, 1.0f));
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, pillBg);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(pillBg.x + 0.06f, pillBg.y + 0.06f, pillBg.z + 0.06f, 1.0f));
            }
            if (ImGui::Button(icon, btnDim))
                m_currentTool = tool;
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tooltip);
            ImGui::SameLine();
        };

        ToolButton(ICON_FA_ARROWS_ALT, TransformTool::Move, "Move (W)");
        ToolButton(ICON_FA_SYNC_ALT, TransformTool::Rotate, "Rotate (E)");
        ToolButton(ICON_FA_EXPAND, TransformTool::Scale, "Scale (R)");

        {
            ImGui::SameLine(0, 8);
            ImVec2 sepPos = ImGui::GetCursorScreenPos();
            dl->AddLine(ImVec2(sepPos.x, sepPos.y + 2), ImVec2(sepPos.x, sepPos.y + btnSize - 2),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(0.1f, 0.69f, 0.74f, 0.25f)), 1.0f);
            ImGui::Dummy(ImVec2(2, btnSize));
            ImGui::SameLine(0, 8);
        }

        bool isLocal = (m_transformSpace == TransformSpace::Local);
        ImGui::PushStyleColor(ImGuiCol_Button, pillBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(pillBg.x + 0.06f, pillBg.y + 0.06f, pillBg.z + 0.06f, 1.0f));
        if (ImGui::Button(isLocal ? ICON_FA_CUBE " Local" : ICON_FA_GLOBE " World", ImVec2(80, btnSize)))
        {
            m_transformSpace = isLocal ? TransformSpace::World : TransformSpace::Local;
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Toggle World/Local space");

        ImGui::SameLine(0, 8);

        {
            ImVec2 sepPos = ImGui::GetCursorScreenPos();
            dl->AddLine(ImVec2(sepPos.x, sepPos.y + 2), ImVec2(sepPos.x, sepPos.y + btnSize - 2),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(0.1f, 0.69f, 0.74f, 0.25f)), 1.0f);
            ImGui::Dummy(ImVec2(2, btnSize));
            ImGui::SameLine(0, 8);
        }
    }

    void EditorUI::RenderToolbarPlayControls(float btnSize, ImDrawList* dl, const ImVec4& playGreen,
                                             const ImVec4& accentAmber, const ImVec4& stopRed, const ImVec4& pillBg)
    {
        ImVec2 btnDim(btnSize, btnSize);

        float windowWidth = ImGui::GetWindowContentRegionMax().x;
        float playWidth = btnSize * 3 + 12;
        float cursorX = (windowWidth - playWidth) * 0.5f;
        if (cursorX > ImGui::GetCursorPosX())
            ImGui::SetCursorPosX(cursorX);

        bool isPlaying = (m_playMode == PlayMode::Playing || m_playMode == PlayMode::Simulating);
        if (isPlaying)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, playGreen);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(playGreen.x, playGreen.y, playGreen.z, 0.85f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, pillBg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(playGreen.x * 0.4f, playGreen.y * 0.4f, playGreen.z * 0.4f, 1.0f));
        }
        if (ImGui::Button(ICON_FA_PLAY, btnDim))
        {
            m_playModeManager.TogglePlayMode();
            m_playMode = m_playModeManager.IsPlaying()
                             ? PlayMode::Playing
                             : (m_playModeManager.IsSimulating()
                                    ? PlayMode::Simulating
                                    : (m_playModeManager.IsPaused() ? PlayMode::Paused : PlayMode::Stopped));
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Play mode toggled: %s",
                           m_playMode == PlayMode::Playing
                               ? "Playing"
                               : (m_playMode == PlayMode::Simulating
                                      ? "Simulating"
                                      : (m_playMode == PlayMode::Paused ? "Paused" : "Stopped")));
            ShowNotification(m_playMode == PlayMode::Stopped ? "Stopped" : "Running...",
                             m_playMode == PlayMode::Stopped ? "info" : "success", 2.0f);
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Play (F5)");
        ImGui::SameLine();

        bool isPaused = (m_playMode == PlayMode::Paused);
        if (isPaused)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, accentAmber);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accentAmber.x, accentAmber.y, accentAmber.z, 0.85f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, pillBg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(accentAmber.x * 0.3f, accentAmber.y * 0.3f, accentAmber.z * 0.3f, 1.0f));
        }
        if (ImGui::Button(ICON_FA_PAUSE, btnDim))
        {
            m_playModeManager.TogglePause();
            if (m_playModeManager.IsPaused())
                m_playMode = PlayMode::Paused;
            else if (m_playModeManager.IsSimulating())
                m_playMode = PlayMode::Simulating;
            else if (m_playModeManager.IsInPlayMode())
                m_playMode = PlayMode::Playing;
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pause");
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, pillBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(stopRed.x * 0.3f, stopRed.y * 0.3f, stopRed.z * 0.3f, 1.0f));
        if (ImGui::Button(ICON_FA_STOP, btnDim))
        {
            if (m_playMode != PlayMode::Stopped)
            {
                m_playModeManager.ExitPlayMode();
                m_playMode = PlayMode::Stopped;
                ShowNotification("Stopped", "info", 2.0f);
            }
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stop (Shift+F5)");

        ImGui::SameLine(0, 8);

        {
            ImVec2 sepPos = ImGui::GetCursorScreenPos();
            dl->AddLine(ImVec2(sepPos.x, sepPos.y + 2), ImVec2(sepPos.x, sepPos.y + btnSize - 2),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(0.1f, 0.69f, 0.74f, 0.25f)), 1.0f);
            ImGui::Dummy(ImVec2(2, btnSize));
            ImGui::SameLine(0, 8);
        }
    }

    void EditorUI::RenderToolbarSnapControls(float btnSize, const ImVec4& pillBg)
    {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(pillBg.x, pillBg.y, pillBg.z, 1.0f));
        ImGui::Checkbox(ICON_FA_MAGNET " Snap", &m_snapEnabled);
        ImGui::PopStyleColor();
        if (m_snapEnabled)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::DragFloat("##SnapVal", &m_snapValue, 0.1f, 0.1f, 100.0f, "%.1f");
        }
    }

    void EditorUI::RenderToolbar()
    {
        ImGuiWindowFlags toolbarFlags =
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Begin("##Toolbar", nullptr, toolbarFlags))
        {
            float btnSize = 30.0f;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            ImVec4 accentTeal(0.102f, 0.686f, 0.737f, 1.0f);
            ImVec4 accentAmber(0.941f, 0.659f, 0.188f, 1.0f);
            ImVec4 playGreen(0.239f, 0.839f, 0.549f, 1.0f);
            ImVec4 stopRed(0.910f, 0.251f, 0.251f, 1.0f);
            ImVec4 pillBg(0.157f, 0.173f, 0.212f, 1.0f);

            RenderToolbarTransformTools(btnSize, dl, accentTeal, pillBg);
            RenderToolbarPlayControls(btnSize, dl, playGreen, accentAmber, stopRed, pillBg);
            RenderToolbarSnapControls(btnSize, pillBg);
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
    }

} // namespace SparkEditor
