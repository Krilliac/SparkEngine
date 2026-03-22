/**
 * @file EditorMenuBar.cpp
 * @brief Main menu bar and toolbar rendering for the SparkEditor UI
 *
 * Contains EditorUI::RenderMainMenuBar() and EditorUI::RenderToolbar().
 * Split from EditorUI.cpp for maintainability.
 */
#include "EditorUI.h"
#include "EditorPluginManager.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "../Panels/HierarchyPanel.h"
#include "../Utils/SparkConsole.h"
#include "EditorApplication.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <cstdlib>
#endif

namespace SparkEditor
{

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
            auto it = m_panels.find("Hierarchy");
            if (it != m_panels.end())
            {
                auto* hierarchy = dynamic_cast<HierarchyPanel*>(it->second.get());
                if (hierarchy)
                {
                    hierarchy->ResetToDefault();
                }
            }
            m_currentScenePath.clear();
            m_currentSceneName = "Untitled";
            m_sceneModified = false;

            if (m_pluginManager)
            {
                m_pluginManager->NotifySceneLoad("Untitled");
            }

            ShowNotification("New scene created", "success");
        }
        if (ImGui::MenuItem("Save Scene", "Ctrl+S"))
        {
            if (m_projectManager && m_projectManager->HasOpenProject())
            {
                if (m_currentScenePath.empty())
                {
                    m_currentScenePath =
                        m_projectManager->GetProjectScenesPath() + "/" + m_currentSceneName + ".sparkscene";
                }
                if (SaveCurrentScene(m_currentScenePath))
                {
                    m_sceneModified = false;
                    ShowNotification("Scene saved: " + m_currentSceneName, "success");
                }
                else
                {
                    ShowNotification("Failed to save scene", "error");
                }
            }
            else
            {
                ShowNotification("Open a project first before saving a scene", "warning");
            }
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
                        if (m_projectManager->OpenProject(rp.path))
                            ShowNotification("Opened project: " + rp.name, "success");
                        else
                            ShowNotification("Failed to open project: " + rp.name, "error");
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
            m_exitRequested = true;
        }
        ImGui::EndMenu();
    }

    void EditorUI::RenderEditMenu()
    {
        if (!ImGui::BeginMenu("Edit"))
            return;

        bool canUndo = m_undoRedoManager && m_undoRedoManager->CanUndo();
        bool canRedo = m_undoRedoManager && m_undoRedoManager->CanRedo();
        std::string undoLabel = "Undo";
        std::string redoLabel = "Redo";
        if (canUndo)
        {
            undoLabel += " (" + m_undoRedoManager->GetUndoDescription() + ")";
        }
        if (canRedo)
        {
            redoLabel += " (" + m_undoRedoManager->GetRedoDescription() + ")";
        }

        if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, canUndo))
        {
            m_undoRedoManager->Undo();
            ShowNotification("Undo: " + m_undoRedoManager->GetUndoDescription(), "info");
        }
        if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, canRedo))
        {
            m_undoRedoManager->Redo();
            ShowNotification("Redo: " + m_undoRedoManager->GetRedoDescription(), "info");
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

        if (ImGui::MenuItem("Sprite"))
            ShowNotification("Created Sprite!", "success");
        if (ImGui::MenuItem("Animated Sprite"))
            ShowNotification("Created Animated Sprite!", "success");
        if (ImGui::MenuItem("Tilemap"))
            ShowNotification("Created Tilemap!", "success");
        if (ImGui::MenuItem("Camera 2D"))
            ShowNotification("Created 2D Camera!", "success");
        if (ImGui::MenuItem("Parallax Background"))
            ShowNotification("Created Parallax Background!", "success");
        if (ImGui::MenuItem("Nine-Slice Sprite"))
            ShowNotification("Created Nine-Slice Sprite!", "success");
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
            auto it = m_panels.find("Hierarchy");
            if (it != m_panels.end())
            {
                auto* hierarchy = dynamic_cast<HierarchyPanel*>(it->second.get());
                if (hierarchy)
                {
                    hierarchy->CreateObject(name);
                    m_sceneModified = true;
                }
            }
            ShowNotification("Created " + name, "success", 2.0f);
        };

        if (ImGui::MenuItem("Create Empty"))
        {
            createObject("Empty GameObject");
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

    void EditorUI::RenderWindowCorePanels()
    {
        if (ImGui::MenuItem("Hierarchy", nullptr, IsPanelVisible("Hierarchy")))
            SetPanelVisible("Hierarchy", !IsPanelVisible("Hierarchy"));
        if (ImGui::MenuItem("Inspector", nullptr, IsPanelVisible("Inspector")))
            SetPanelVisible("Inspector", !IsPanelVisible("Inspector"));
        if (ImGui::MenuItem("Scene View", nullptr, IsPanelVisible("SceneView")))
            SetPanelVisible("SceneView", !IsPanelVisible("SceneView"));
        if (ImGui::MenuItem("Asset Browser", nullptr, IsPanelVisible("AssetBrowser")))
            SetPanelVisible("AssetBrowser", !IsPanelVisible("AssetBrowser"));
        if (ImGui::MenuItem("Console", nullptr, IsPanelVisible("Console")))
            SetPanelVisible("Console", !IsPanelVisible("Console"));
        if (ImGui::MenuItem("Game View", nullptr, IsPanelVisible("GameView")))
            SetPanelVisible("GameView", !IsPanelVisible("GameView"));
        if (ImGui::MenuItem(ICON_FA_PALETTE " Material Editor", nullptr, IsPanelVisible("MaterialEditor")))
            SetPanelVisible("MaterialEditor", !IsPanelVisible("MaterialEditor"));
        if (ImGui::MenuItem(ICON_FA_PLAY " Play Mode Toolbar", nullptr, IsPanelVisible("PlayModeToolbar")))
            SetPanelVisible("PlayModeToolbar", !IsPanelVisible("PlayModeToolbar"));
        if (ImGui::MenuItem("Profiler", nullptr, IsPanelVisible("Profiler")))
            SetPanelVisible("Profiler", !IsPanelVisible("Profiler"));
    }

    void EditorUI::RenderWindowToolPanels()
    {
        ImGui::TextDisabled("Tools & Debug");
        if (ImGui::MenuItem(ICON_FA_BUG " Debug Visualizer", nullptr, IsPanelVisible("DebugVisualizer")))
            SetPanelVisible("DebugVisualizer", !IsPanelVisible("DebugVisualizer"));
        if (ImGui::MenuItem(ICON_FA_CHART_BAR " Scene Stats", nullptr, IsPanelVisible("SceneStats")))
            SetPanelVisible("SceneStats", !IsPanelVisible("SceneStats"));
        if (ImGui::MenuItem(ICON_FA_CUBE " Object Placement", nullptr, IsPanelVisible("ObjectPlacement")))
            SetPanelVisible("ObjectPlacement", !IsPanelVisible("ObjectPlacement"));
        if (ImGui::MenuItem(ICON_FA_HAMMER " Build & Cook", nullptr, IsPanelVisible("BuildCook")))
            SetPanelVisible("BuildCook", !IsPanelVisible("BuildCook"));
        if (ImGui::MenuItem(ICON_FA_MOUNTAIN " Terrain Editor", nullptr, IsPanelVisible("TerrainEditor")))
            SetPanelVisible("TerrainEditor", !IsPanelVisible("TerrainEditor"));

        ImGui::TextDisabled("Tools & Analysis");
        if (ImGui::MenuItem(ICON_FA_UNDO " Undo History", nullptr, IsPanelVisible("UndoHistory")))
            SetPanelVisible("UndoHistory", !IsPanelVisible("UndoHistory"));
        if (ImGui::MenuItem(ICON_FA_CHART_BAR " Scene Statistics", nullptr, IsPanelVisible("SceneStats")))
            SetPanelVisible("SceneStats", !IsPanelVisible("SceneStats"));
        if (ImGui::MenuItem(ICON_FA_CUBE " Prefab Editor", nullptr, IsPanelVisible("PrefabEditor")))
            SetPanelVisible("PrefabEditor", !IsPanelVisible("PrefabEditor"));
        if (ImGui::MenuItem(ICON_FA_SEARCH " Search", nullptr, IsPanelVisible("Search")))
            SetPanelVisible("Search", !IsPanelVisible("Search"));
    }

    void EditorUI::RenderWindow2DAndGamePanels()
    {
        ImGui::TextDisabled("2D / 2.5D Panels");
        if (ImGui::MenuItem("Sprite Editor", nullptr, IsPanelVisible("SpriteEditor")))
            SetPanelVisible("SpriteEditor", !IsPanelVisible("SpriteEditor"));
        if (ImGui::MenuItem("Tilemap Editor", nullptr, IsPanelVisible("TilemapEditor")))
            SetPanelVisible("TilemapEditor", !IsPanelVisible("TilemapEditor"));
        if (ImGui::MenuItem("Sprite Animation", nullptr, IsPanelVisible("SpriteAnimEditor")))
            SetPanelVisible("SpriteAnimEditor", !IsPanelVisible("SpriteAnimEditor"));
        if (ImGui::MenuItem("Physics 2D", nullptr, IsPanelVisible("Physics2D")))
            SetPanelVisible("Physics2D", !IsPanelVisible("Physics2D"));
        if (ImGui::MenuItem("Physics 3D", nullptr, IsPanelVisible("Physics3D")))
            SetPanelVisible("Physics3D", !IsPanelVisible("Physics3D"));

        ImGui::Separator();
        ImGui::TextDisabled("Game Modules");
        if (ImGui::MenuItem(ICON_FA_PUZZLE_PIECE " Game Module Selector", nullptr,
                            IsPanelVisible("GameModuleSelector")))
            SetPanelVisible("GameModuleSelector", !IsPanelVisible("GameModuleSelector"));

        ImGui::Separator();
        ImGui::TextDisabled("FPS Panels");
        if (ImGui::MenuItem("Weapon Editor", nullptr, IsPanelVisible("WeaponEditor")))
            SetPanelVisible("WeaponEditor", !IsPanelVisible("WeaponEditor"));
        if (ImGui::MenuItem("FPS Tools", nullptr, IsPanelVisible("FPSTools")))
            SetPanelVisible("FPSTools", !IsPanelVisible("FPSTools"));
    }

    void EditorUI::RenderWindowMenu()
    {
        if (!ImGui::BeginMenu("Window"))
            return;

        RenderWindowCorePanels();
        ImGui::Separator();
        RenderWindowToolPanels();
        ImGui::Separator();
        RenderWindow2DAndGamePanels();

        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout"))
        {
            ResetToDefaultLayout();
            ShowNotification("Layout reset!", "success");
        }
        if (ImGui::MenuItem("Save Layout"))
        {
            SaveLayout("Custom Layout");
            ShowNotification("Layout saved!", "success");
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
#else
            // Use fork/execlp instead of system() to avoid shell injection risks
            pid_t pid = fork();
            if (pid == 0)
            {
                execlp("xdg-open", "xdg-open", "docs/", nullptr);
                _exit(1);
            }
#endif
            ShowNotification("Opening documentation...", "info", 2.0f);
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

        bool isPlaying = (m_playMode == PlayMode::Playing);
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
            m_playMode = m_playModeManager.IsInPlayMode() ? PlayMode::Playing : PlayMode::Stopped;
            ShowNotification(m_playModeManager.IsInPlayMode() ? "Playing..." : "Stopped",
                             m_playModeManager.IsInPlayMode() ? "success" : "info", 2.0f);
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
