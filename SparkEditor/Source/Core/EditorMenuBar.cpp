/**
 * @file EditorMenuBar.cpp
 * @brief Main menu bar and toolbar rendering for the SparkEditor UI
 *
 * Contains EditorUI::RenderMainMenuBar() and EditorUI::RenderToolbar().
 * Split from EditorUI.cpp for maintainability.
 */
#include "EditorUI.h"
#include "EditorDockLayout.h"
#include "EditorPanel.h"
#include "EditorPluginManager.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include <SparkUI/Fonts.h>
#include <SparkUI/Runtime.h>
#include <SparkUI/Widgets.h>
#include "../Panels/HierarchyPanel.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "EditorApplication.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <vector>

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

    namespace
    {
        void RenderUnavailableMenuItem(const char* label, const char* shortcut, const char* explanation)
        {
            ImGui::BeginDisabled();
            ImGui::MenuItem(label, shortcut);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", explanation);
        }
    } // namespace

    void EditorUI::ShowOpenSceneDialog()
    {
        RequestDocumentTransition(DocumentTransitionAction::OpenSceneDialog);
    }

    void EditorUI::ShowOpenSceneDialogNow()
    {
#ifdef _WIN32
        std::vector<wchar_t> pathBuffer(32768, L'\0');
        std::wstring initialDirectory;
        if (m_projectManager && m_projectManager->HasOpenProject())
            initialDirectory = std::filesystem::path(m_projectManager->GetProjectScenesPath()).wstring();
        const wchar_t filter[] = L"Spark Scenes (*.sparkscene)\0*.sparkscene\0All Files (*.*)\0*.*\0\0";
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.lpstrFile = pathBuffer.data();
        dialog.nMaxFile = static_cast<DWORD>(pathBuffer.size());
        dialog.lpstrFilter = filter;
        dialog.lpstrInitialDir = initialDirectory.empty() ? nullptr : initialDirectory.c_str();
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog) && !OpenScene(std::filesystem::path(pathBuffer.data()).string()))
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
            RenderViewMenu();
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
        RenderUnavailableMenuItem("Cut", "Ctrl+X",
                                  "Selection clipboard actions are not available in this editor build.");
        RenderUnavailableMenuItem("Copy", "Ctrl+C",
                                  "Selection clipboard actions are not available in this editor build.");
        RenderUnavailableMenuItem("Paste", "Ctrl+V",
                                  "Selection clipboard actions are not available in this editor build.");
        ImGui::Separator();
        RenderUnavailableMenuItem("Select All", "Ctrl+A",
                                  "Selection clipboard actions are not available in this editor build.");
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
                    const std::string menuLabel =
                        (panel->GetIcon().empty() ? std::string(EditorPanel::CategoryIcon(panel->GetCategory()))
                                                  : panel->GetIcon()) +
                        " " + panel->GetName();
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
        if (ImGui::MenuItem("Save Layout As..."))
            m_saveLayoutAsRequested = true;
        const auto layouts = m_layoutManager ? m_layoutManager->GetSavedLayouts() : std::vector<LayoutInfo>{};
        if (ImGui::BeginMenu("Load Layout"))
        {
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
        if (ImGui::BeginMenu("Delete Layout", !layouts.empty()))
        {
            for (const LayoutInfo& layout : layouts)
            {
                if (ImGui::MenuItem(layout.name.c_str()))
                {
                    m_layoutPendingDelete = layout.name;
                    m_deleteLayoutRequested = true;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    void EditorUI::RenderViewMenu()
    {
        if (!ImGui::BeginMenu("View"))
            return;

        if (ImGui::MenuItem("Appearance..."))
            OpenAppearanceWindow();
        if (ImGui::BeginMenu("UI Scale"))
        {
            if (ImGui::MenuItem("Increase", "Ctrl+="))
                StepUiScale(1);
            if (ImGui::MenuItem("Decrease", "Ctrl+-"))
                StepUiScale(-1);
            if (ImGui::MenuItem("Reset to 100%", "Ctrl+0"))
                StepUiScale(0);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Status Bar", nullptr, m_preferences.showStatusBar))
        {
            m_preferences.showStatusBar = !m_preferences.showStatusBar;
            SavePreferences();
        }
        if (ImGui::MenuItem("Start Page", nullptr, m_startPage.IsOpen()))
        {
            if (m_startPage.IsOpen())
                m_startPage.Close();
            else
                OpenStartPage();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Command Palette", "Ctrl+P") && m_commandPalette)
            m_commandPalette->Open();
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
        RenderUnavailableMenuItem(
            ICON_FA_LIGHTBULB " Build Lighting", nullptr,
            "Lighting builds are not available in this editor build. Use Build Settings for supported tasks.");
        RenderUnavailableMenuItem(
            ICON_FA_MAP " Build NavMesh", nullptr,
            "NavMesh builds are not available in this editor build. Use Build Settings for supported tasks.");
        ImGui::Separator();
        RenderUnavailableMenuItem(
            ICON_FA_HAMMER " Build All", nullptr,
            "Build All is unavailable because lighting and NavMesh builds are not available yet.");
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
        if (ImGui::MenuItem("Welcome..."))
            OpenStartPage();
        // Kept where users look for it; themes live in View > Appearance now.
        if (ImGui::MenuItem("Themes..."))
            OpenAppearanceWindow();
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

    void EditorUI::RenderToolbarTransformTools()
    {
        namespace Ui = Spark::Ui;

        static constexpr const char* kToolLabels[] = {ICON_FA_ARROWS_ALT " Move", ICON_FA_SYNC_ALT " Rotate",
                                                      ICON_FA_EXPAND " Scale"};
        int tool = static_cast<int>(m_currentTool);
        ImGui::BeginGroup();
        if (Ui::SegmentedControl("##transformTool", kToolLabels, 3, &tool))
            SetTransformTool(static_cast<TransformTool>(tool));
        ImGui::EndGroup();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Transform tool: Move (W), Rotate (E), Scale (R)");

        Ui::ToolbarSeparator();

        static constexpr const char* kSpaceLabels[] = {ICON_FA_GLOBE " World", ICON_FA_CUBE " Local"};
        int space = m_transformSpace == TransformSpace::Local ? 1 : 0;
        ImGui::BeginGroup();
        if (Ui::SegmentedControl("##transformSpace", kSpaceLabels, 2, &space))
            m_transformSpace = space == 1 ? TransformSpace::Local : TransformSpace::World;
        ImGui::EndGroup();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Gizmo space: world axes or the selection's local axes");

        Ui::ToolbarSeparator();
    }

    void EditorUI::RenderToolbarPlayControls(float btnSize)
    {
        namespace Ui = Spark::Ui;
        // The editor is not an in-process game host. PlayModeManager is an
        // intentional state-preview aid, but its counters are not gameplay.
        // Keep the prominent toolbar affordance on the verified out-of-process
        // path so this button cannot look like a running game.
        const bool previewing = m_playModeManager.IsInPlayMode();
        const float previewWidth =
            previewing ? ImGui::CalcTextSize("Preview").x + ImGui::GetStyle().FramePadding.x * 4.0f : 0.0f;
        const float clusterWidth = btnSize + previewWidth;
        const float centredX = (ImGui::GetWindowWidth() - clusterWidth) * 0.5f;
        if (centredX > ImGui::GetCursorPosX())
            ImGui::SetCursorPosX(centredX);

        {
            Ui::ScopedPrimaryButtonColors primary;
            if (ImGui::Button(ICON_FA_PLAY "##OpenPlayControl", ImVec2(btnSize, btnSize)))
            {
                SetPanelVisible("PlayControl", true);
                ShowNotification("Play Control opened — select a module, then Launch Game", "info", 3.0f);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Open Play Control; Launch Game runs actual gameplay in SparkEngine.exe");

        // The state preview is not gameplay; say so beside the button while it runs.
        if (previewing)
        {
            ImGui::SameLine();
            Ui::StatusChip(Ui::Status::Warning, "Preview",
                           "Editor state preview is running. It restores the scene when it stops; it is not "
                           "gameplay.");
        }

        Ui::ToolbarSeparator();
    }

    void EditorUI::RenderToolbarSnapControls()
    {
        namespace Ui = Spark::Ui;
        if (Ui::IconButton("##snapToggle", ICON_FA_MAGNET,
                           m_snapEnabled ? "Snap is on: click to turn it off" : "Snap is off: click to turn it on",
                           m_snapEnabled))
        {
            m_snapEnabled = !m_snapEnabled;
        }
        ImGui::SameLine();
        {
            Ui::ScopedDisabled disabled(!m_snapEnabled, "Turn snap on to change the increment");
            Ui::ScopedFont mono(Ui::FontRole::Mono);
            ImGui::SetNextItemWidth(Ui::Em(4.0f));
            ImGui::DragFloat("##SnapVal", &m_snapValue, 0.1f, 0.1f, 100.0f, "%.1f");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Snap increment in world units");
    }

    void EditorUI::RenderToolbarLayoutMenu()
    // NOTE: Intentionally exceeds the 60-line guideline -- one dropdown, read top to bottom.
    {
        namespace Ui = Spark::Ui;
        Ui::ToolbarSeparator();

        const std::string current = m_preferences.lastLayout.empty() ? "Default" : m_preferences.lastLayout;
        char preview[96];
        std::snprintf(preview, sizeof(preview), "%s  %s", ICON_FA_COLUMNS, current.c_str());
        ImGui::SetNextItemWidth(Ui::Em(11.0f));
        if (!ImGui::BeginCombo("##layoutMenu", preview, ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Workspace layout");
            return;
        }

        if (ImGui::Selectable("Default", m_preferences.lastLayout.empty()))
        {
            ResetToDefaultLayout();
        }

        const auto layouts = m_layoutManager ? m_layoutManager->GetSavedLayouts() : std::vector<LayoutInfo>{};
        if (!layouts.empty())
            ImGui::Separator();
        for (const LayoutInfo& layout : layouts)
        {
            if (ImGui::Selectable(layout.name.c_str(), layout.name == m_preferences.lastLayout))
            {
                if (LoadLayout(layout.name))
                {
                    Notify(NotificationSeverity::Success, "Layout loaded", layout.name);
                }
                else
                {
                    Notify(NotificationSeverity::Error, "Layout not loaded",
                           m_layoutManager ? m_layoutManager->GetLastError() : std::string());
                }
            }
            if (!layout.description.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("%s", layout.description.c_str());
        }

        ImGui::Separator();
        if (ImGui::Selectable("Save Layout As..."))
            m_saveLayoutAsRequested = true;
        if (ImGui::BeginMenu("Delete...", !layouts.empty()))
        {
            for (const LayoutInfo& layout : layouts)
            {
                if (ImGui::MenuItem(layout.name.c_str()))
                {
                    m_layoutPendingDelete = layout.name;
                    m_deleteLayoutRequested = true;
                }
            }
            ImGui::EndMenu();
        }
        // The menu item closes only its submenu; close the dropdown too before the confirmation opens.
        if (m_deleteLayoutRequested)
            ImGui::CloseCurrentPopup();
        if (ImGui::Selectable("Reset"))
        {
            ResetToDefaultLayout();
            Notify(NotificationSeverity::Info, "Layout reset", "Panels are back in their default places.");
        }
        ImGui::EndCombo();
    }

    void EditorUI::RenderToolbar()
    {
        namespace Ui = Spark::Ui;
        const ImGuiWindowFlags toolbarFlags =
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Ui::Px2(8.0f, EditorToolbarPaddingY));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Ui::Px2(4.0f, 4.0f));

        if (ImGui::Begin("##Toolbar", nullptr, toolbarFlags))
        {
            const float btnSize = Ui::Px(EditorToolbarButtonSize);
            // One row: centre every control on the play button's height.
            const float rowTop = ImGui::GetCursorPosY();
            const float controlOffset = std::max(0.0f, (btnSize - ImGui::GetFrameHeight()) * 0.5f);

            ImGui::SetCursorPosY(rowTop + controlOffset);
            RenderToolbarTransformTools();
            ImGui::SetCursorPosY(rowTop);
            RenderToolbarPlayControls(btnSize);
            ImGui::SetCursorPosY(rowTop + controlOffset);
            RenderToolbarSnapControls();
            ImGui::SameLine();
            ImGui::SetCursorPosY(rowTop + controlOffset);
            RenderToolbarLayoutMenu();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

} // namespace SparkEditor
