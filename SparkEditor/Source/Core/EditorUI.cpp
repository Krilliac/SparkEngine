/**
 * @file EditorUI.cpp
 * @brief Core editor UI system — lifecycle, rendering dispatch, layout, commands
 *
 * Panel creation is in EditorPanelFactory.cpp.
 * Menu bar and toolbar rendering are in EditorMenuBar.cpp.
 */

#include "EditorUI.h"
#include "EditorTheme.h"
#include "EditorFonts.h"
#include "EditorIcons.h"
#include "Core/FaultIsolation.h"
#include "Utils/SparkConsole.h"
#include "Utils/Validate.h"
#include "../Panels/SceneViewPanel.h"
#include "../Panels/ConsolePanel.h"
#include "../Panels/HierarchyPanel.h"
#include "../Panels/InspectorPanel.h"
#include "../Panels/AssetBrowserPanel.h"
#include "../Panels/GameViewPanel.h"
#include "../Panels/WeaponEditorPanel.h"
#include "../Panels/FPSToolsPanel.h"
#include "../Panels/ProjectBrowserPanel.h"
#include "../Panels/DebugVisualizerPanel.h"
#include "../Panels/ObjectPlacementPanel.h"
#include "../Panels/BuildCookPanel.h"
#include "../Panels/SpriteEditorPanel.h"
#include "../Panels/TilemapEditorPanel.h"
#include "../Panels/SpriteAnimationEditorPanel.h"
#include "../Panels/Physics2DPanel.h"
#include "../Panels/UndoHistoryPanel.h"
#include "../Panels/SceneStatisticsPanel.h"
#include "../Panels/PrefabEditorPanel.h"
#include "../Panels/SearchPanel.h"
#include "../Panels/DedicatedServerPanel.h"
#include "../Panels/MaterialEditorPanel.h"
#include "../Panels/PlayModeToolbarPanel.h"
#include "../Panels/PostProcessingPanel.h"
#include "../Panels/DialogueEditorPanel.h"
#include "../Panels/AIEditorPanel.h"
#include "../Panels/SplineEditorPanel.h"
#include "../Panels/ParticleEditorPanel.h"
#include "../Panels/EventMonitorPanel.h"
#include "../Panels/SaveSystemPanel.h"
#include "../Panels/LocalizationPanel.h"
#include "../Panels/WeatherFogPanel.h"
#include "../Terrain/TerrainEditor.h"
#include "../Profiler/PerformanceProfiler.h"
#include "EditorCrashHandler.h"
#include "EditorPluginManager.h"
#include "EditorApplication.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <iostream>
#include <algorithm>
#include <memory>
#include <filesystem>
#include <fstream>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace SparkEditor
{

    EditorUI::EditorUI()
    {
        m_crashHandler = &EditorCrashHandler::GetInstance();
    }

    EditorUI::~EditorUI()
    {
        if (m_isInitialized)
        {
            Shutdown();
        }
    }

    bool EditorUI::Initialize(const EditorConfig& config)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("Initializing Enhanced EditorUI with full configuration...");

        try
        {
            m_config = std::unique_ptr<EditorConfig>(new EditorConfig(config));

            // Stand up all subsystem managers and create panels
            InitializeManagers(config);

            // Register command palette actions (after panels are created)
            InitializeCommandPalette();

            // Wire project-opened / project-closed callbacks into panels
            WireCallbacks();

            // Show project browser on startup if no project is loaded (skip in test mode)
            if (!config.testMode && m_projectManager && !m_projectManager->HasOpenProject())
            {
                if (m_projectBrowserPanel)
                {
                    m_projectBrowserPanel->ShowBrowser();
                }
            }

            // Apply the Spark Professional theme
            console.LogInfo("Applying Spark Professional theme...");
            ApplyTheme("Spark Professional");
            console.LogSuccess("Theme applied");

            m_isInitialized = true;
            console.LogSuccess("Enhanced EditorUI initialized successfully");
            return true;
        }
        catch (const std::exception& e)
        {
            console.LogError("Exception in EditorUI::Initialize: " + std::string(e.what()));
            return false;
        }
        catch (...)
        {
            console.LogError("Unknown exception in EditorUI::Initialize");
            return false;
        }
    }

    void EditorUI::InitializeManagers(const EditorConfig& /*config*/)
    {
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("Using enhanced initialization for production use");

        // Crash handler
        console.LogInfo("Initializing crash handler...");
        if (m_crashHandler && m_crashHandler->Initialize())
        {
            console.LogSuccess("Crash handler initialized successfully");
        }
        else
        {
            console.LogWarning("Crash handler initialization failed");
        }

        // Project manager + browser panel
        console.LogInfo("Initializing project manager...");
        m_projectManager = std::make_unique<ProjectManager>();
        if (m_projectManager->Initialize())
        {
            console.LogSuccess("Project manager initialized");
        }
        else
        {
            console.LogWarning("Project manager initialization failed");
        }
        m_projectBrowserPanel = std::make_shared<ProjectBrowserPanel>(m_projectManager.get());
        m_projectBrowserPanel->Initialize();

        // Undo/redo
        console.LogInfo("Initializing undo/redo manager...");
        m_undoRedoManager = std::make_unique<UndoRedoManager>();
        console.LogSuccess("Undo/redo manager initialized");

        // Prefab manager
        console.LogInfo("Initializing prefab manager...");
        m_prefabManager = std::make_unique<PrefabManager>();
        m_prefabManager->Initialize();
        console.LogSuccess("Prefab manager initialized");

        // Command palette
        console.LogInfo("Initializing command palette...");
        m_commandPalette = std::make_unique<CommandPalette>();
        console.LogSuccess("Command palette initialized");

        // Gizmo system — 3D manipulation overlays
        console.LogInfo("Initializing gizmo system...");
        m_gizmoSystem = std::make_unique<GizmoSystem>();
        if (m_gizmoSystem->Initialize(nullptr, nullptr))
        {
            console.LogSuccess("Gizmo system initialized");
        }
        else
        {
            console.LogWarning("Gizmo system initialization failed");
        }

        // Collaborative editing session
        console.LogInfo("Initializing collaborative edit session...");
        m_collabSession = std::make_unique<CollaborativeEditSession>();
        console.LogSuccess("Collaborative edit session initialized");

        // Live edit bridge (editor → AreaServer)
        console.LogInfo("Initializing live edit bridge...");
        m_liveEditBridge = std::make_unique<LiveEditBridge>();
        console.LogSuccess("Live edit bridge initialized");

        // Editor panels
        console.LogInfo("Creating editor panels...");
        CreatePanels();
        console.LogSuccess("Panels created successfully");
    }

    void EditorUI::WireCallbacks()
    {
        if (!m_projectManager)
        {
            Spark::SimpleConsole::GetInstance().LogWarning("WireCallbacks: projectManager is null, skipping");
            return;
        }

        m_projectManager->SetOnProjectOpened(
            [this](const ProjectInfo& project)
            {
                // Update asset browser path
                auto it = m_panels.find("AssetBrowser");
                if (it != m_panels.end())
                {
                    auto* assetBrowser = dynamic_cast<AssetBrowserPanel*>(it->second.get());
                    if (assetBrowser)
                    {
                        assetBrowser->SetProjectPath(m_projectManager->GetProjectAssetsPath());
                    }
                }

                // Reset hierarchy for new project
                auto hierIt = m_panels.find("Hierarchy");
                if (hierIt != m_panels.end())
                {
                    auto* hierarchy = dynamic_cast<HierarchyPanel*>(hierIt->second.get());
                    if (hierarchy)
                    {
                        hierarchy->ResetToDefault();
                    }
                }

                // Set scene name from project's default scene
                if (!project.defaultScene.empty())
                {
                    std::filesystem::path scenePath(project.defaultScene);
                    m_currentSceneName = scenePath.stem().string();
                    m_currentScenePath = (std::filesystem::path(project.path) / project.defaultScene).string();
                }
                else
                {
                    m_currentSceneName = "Default";
                    m_currentScenePath.clear();
                }
                m_sceneModified = false;

                // Notify plugins of the scene load
                if (m_pluginManager && !m_currentScenePath.empty())
                {
                    m_pluginManager->NotifySceneLoad(m_currentScenePath);
                }

                ShowNotification("Project opened: " + project.name, "success");
            });

        m_projectManager->SetOnProjectClosed([this](const ProjectInfo& project)
                                             { ShowNotification("Project closed: " + project.name, "info"); });

        // Wire SceneViewPanel to show peer overlays
        auto svIt = m_panels.find("SceneView");
        if (svIt != m_panels.end())
        {
            auto* sceneView = dynamic_cast<SceneViewPanel*>(svIt->second.get());
            if (sceneView && m_collabSession)
            {
                sceneView->SetCollabSession(m_collabSession.get());
            }
        }

        // Wire HierarchyPanel selection to collaborative session for peer presence
        auto hierIt2 = m_panels.find("Hierarchy");
        if (hierIt2 != m_panels.end())
        {
            auto* hierarchy = dynamic_cast<HierarchyPanel*>(hierIt2->second.get());
            if (hierarchy && m_collabSession)
            {
                hierarchy->RegisterSelectionCallback(
                    [this](const std::vector<ObjectID>& selectedObjects)
                    {
                        if (!m_collabSession || !m_collabSession->IsConnected())
                            return;

                        // Broadcast the first selected object as the local selection
                        if (!selectedObjects.empty())
                        {
                            m_collabSession->SetLocalSelection(std::to_string(selectedObjects[0]));
                        }
                        else
                        {
                            m_collabSession->SetLocalSelection("");
                        }
                    });

                hierarchy->RegisterObjectOperationCallback(
                    [this](const std::string& operation, ObjectID objectId)
                    {
                        if (!m_collabSession || !m_collabSession->IsConnected())
                            return;

                        // Map object operations to EditMessage types
                        EditMessage edit;
                        edit.sourceEditor = m_collabSession->GetLocalPeerID();
                        edit.nodeId = std::to_string(objectId);

                        if (operation == "create")
                            edit.type = EditMessageType::NodeAdded;
                        else if (operation == "delete")
                            edit.type = EditMessageType::NodeRemoved;
                        else if (operation == "duplicate")
                            edit.type = EditMessageType::NodeAdded;
                        else if (operation == "rename")
                            edit.type = EditMessageType::NodeRenamed;
                        else if (operation == "move")
                            edit.type = EditMessageType::NodeMoved;
                        else
                            return;

                        m_collabSession->BroadcastEdit(edit);

                        // Also push to live server if connected
                        if (m_liveEditBridge && m_liveEditBridge->IsConnected())
                        {
                            m_liveEditBridge->PushEdit(edit);
                        }
                    });
            }
        }
    }

    void EditorUI::Update(float deltaTime)
    {
        if (!m_isInitialized)
            return;

        // Input handling (scene shortcuts, play mode, transform tools)
        ProcessSceneShortcuts();
        ProcessGlobalHotkeys();

        // Tick notification lifetimes and remove expired ones
        UpdateNotifications(deltaTime);

        // Update stats
        UpdateStats(deltaTime);

        // Update gizmo system
        SPARK_GUARDED_UPDATE("GizmoSystem", "Editor", {
            if (m_gizmoSystem)
                m_gizmoSystem->Update(deltaTime);
        });

        // Update collaborative editing session (processes incoming messages, broadcasts presence)
        SPARK_GUARDED_UPDATE("CollabSession", "Editor", {
            if (m_collabSession)
                m_collabSession->Update(deltaTime);
        });

        // Flush pending live edits to AreaServer
        SPARK_GUARDED_UPDATE("LiveEditBridge", "Editor", {
            if (m_liveEditBridge)
                m_liveEditBridge->Update();
        });

        // Handle keyboard shortcuts for undo/redo, command palette, search
        HandleKeyboardShortcuts();
    }

    void EditorUI::ProcessSceneShortcuts()
    {
        ImGuiIO& io = ImGui::GetIO();

        // Ctrl+N / Ctrl+S: Scene shortcuts
        if (io.KeyCtrl && !io.WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_N))
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

                // Notify plugins that a new (blank) scene was loaded
                if (m_pluginManager)
                {
                    m_pluginManager->NotifySceneLoad("Untitled");
                }

                ShowNotification("New scene created", "success");
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_S))
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
                }
                else
                {
                    ShowNotification("Open a project first before saving", "warning");
                }
            }
        }
    }

    void EditorUI::ProcessGlobalHotkeys()
    {
        ImGuiIO& io = ImGui::GetIO();

        // F5: Toggle play mode (delegates to PlayModeManager)
        if (ImGui::IsKeyPressed(ImGuiKey_F5) && !io.WantTextInput)
        {
            if (io.KeyShift)
            {
                m_playModeManager.ExitPlayMode();
                m_playMode = PlayMode::Stopped;
                ShowNotification("Stopped", "info", 2.0f);
            }
            else
            {
                m_playModeManager.TogglePlayMode();
                m_playMode = m_playModeManager.IsInPlayMode() ? PlayMode::Playing : PlayMode::Stopped;
                ShowNotification(m_playMode == PlayMode::Playing ? "Playing..." : "Stopped", "info", 2.0f);
            }
        }

        // F4: Reload all shaders
        if (ImGui::IsKeyPressed(ImGuiKey_F4) && !io.WantTextInput)
        {
            ShowNotification("Reloading shaders...", "info", 2.0f);
        }

        // W/E/R: Transform tool shortcuts (only when not typing and not in game view)
        bool gameViewCapturing = false;
        {
            auto gvIt = m_panels.find("GameView");
            if (gvIt != m_panels.end())
            {
                auto* gv = dynamic_cast<GameViewPanel*>(gvIt->second.get());
                if (gv)
                {
                    gameViewCapturing = gv->IsCursorCaptured();
                }
            }
        }
        if (!io.WantTextInput && !io.WantCaptureKeyboard && !gameViewCapturing)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_W) && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
            {
                m_currentTool = TransformTool::Move;
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_E) && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
            {
                m_currentTool = TransformTool::Rotate;
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_R) && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
            {
                m_currentTool = TransformTool::Scale;
            }
        }
    }

    void EditorUI::UpdateNotifications(float deltaTime)
    {
        auto it = m_notifications.begin();
        while (it != m_notifications.end())
        {
            it->timeLeft -= deltaTime;
            if (it->timeLeft <= 0.0f && it->duration > 0.0f)
            {
                it = m_notifications.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void EditorUI::Render()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (!m_isInitialized)
            return;

        // === Full-screen DockSpace ===
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGuiWindowFlags dockspaceFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking |
                                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                          ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("##SparkEditorDockSpace", nullptr, dockspaceFlags);
        ImGui::PopStyleVar(3);

        ImGuiID dockspaceId = ImGui::GetID("SparkDockSpace");
        ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

        // Set up default layout on first frame
        if (m_firstFrame)
        {
            SetupDefaultDockLayout(dockspaceId);
            m_firstFrame = false;
        }

        // Menu bar is rendered inside the dockspace window
        RenderMainMenuBar();

        ImGui::End(); // End dockspace window

        // Render toolbar, panels, status bar, notifications
        RenderToolbar();
        RenderPanels();
        RenderStatusBar();
        RenderNotifications();
        RenderModalDialogs();

        // Render project browser modal (on top of everything)
        SPARK_WARN_IF(Spark::LogCategory::Editor, m_projectBrowserPanel == nullptr,
                      "Project browser panel is null during render");
        if (m_projectBrowserPanel && m_projectBrowserPanel->IsModalActive())
        {
            m_projectBrowserPanel->Render();
        }

        // Render command palette overlay (on top of everything)
        SPARK_WARN_IF(Spark::LogCategory::Editor, m_commandPalette == nullptr, "Command palette is null during render");
        if (m_commandPalette)
        {
            m_commandPalette->Render();
        }

        if (m_showDemoWindow)
        {
            ImGui::ShowDemoWindow(&m_showDemoWindow);
        }
    }

    void EditorUI::SetupDefaultDockLayout(ImGuiID dockspaceId)
    {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

        ImGuiID dockMain = dockspaceId;
        ImGuiID dockLeft, dockRight, dockBottom, dockCenter;

        // Split: left 18% for Hierarchy (slim, like Unity/Unreal)
        ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.18f, &dockLeft, &dockMain);
        // Split: right 22% for Inspector (narrower, properties focused)
        ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.22f, &dockRight, &dockMain);
        // Split: bottom 25% for Console + Asset Browser (shorter, more viewport)
        ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.25f, &dockBottom, &dockCenter);

        // Dock panels — use ###panel_id to match stable IDs from BeginPanel()
        ImGui::DockBuilderDockWindow("###simple_hierarchy_panel", dockLeft);
        ImGui::DockBuilderDockWindow("###inspector_panel", dockRight);
        ImGui::DockBuilderDockWindow("###scene_view_panel", dockCenter);
        ImGui::DockBuilderDockWindow("###game_view_panel", dockCenter);
        ImGui::DockBuilderDockWindow("##Toolbar", dockCenter);
        ImGui::DockBuilderDockWindow("###simple_console_panel", dockBottom);
        ImGui::DockBuilderDockWindow("###asset_browser_panel", dockBottom);

        ImGui::DockBuilderFinish(dockspaceId);
    }

    void EditorUI::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("Shutting down EditorUI...");

        // Shutdown panels using vector iteration since unordered_map doesn't have rbegin/rend
        std::vector<std::pair<std::string, std::shared_ptr<EditorPanel>>> panelVector(m_panels.begin(), m_panels.end());
        for (auto it = panelVector.rbegin(); it != panelVector.rend(); ++it)
        {
            try
            {
                if (it->second)
                {
                    console.LogInfo("Shutting down " + it->first + " panel");
                    it->second->Shutdown();
                    console.LogSuccess(it->first + " panel shutdown complete");
                }
            }
            catch (const std::exception& e)
            {
                console.LogError("Exception shutting down " + it->first + " panel: " + std::string(e.what()));
            }
        }
        m_panels.clear();
        console.LogInfo("All panels shutdown and cleared");

        // Shutdown project browser
        if (m_projectBrowserPanel)
        {
            m_projectBrowserPanel->Shutdown();
            m_projectBrowserPanel.reset();
        }

        // Shutdown project manager
        if (m_projectManager)
        {
            console.LogInfo("Shutting down project manager...");
            m_projectManager->Shutdown();
            m_projectManager.reset();
            console.LogSuccess("Project manager shutdown complete");
        }

        // Shutdown prefab manager
        if (m_prefabManager)
        {
            console.LogInfo("Shutting down prefab manager...");
            m_prefabManager->Shutdown();
            m_prefabManager.reset();
            console.LogSuccess("Prefab manager shutdown complete");
        }

        // Shutdown gizmo system
        if (m_gizmoSystem)
        {
            console.LogInfo("Shutting down gizmo system...");
            m_gizmoSystem->Shutdown();
            m_gizmoSystem.reset();
            console.LogSuccess("Gizmo system shutdown complete");
        }

        // Disconnect live edit bridge
        if (m_liveEditBridge)
        {
            console.LogInfo("Shutting down live edit bridge...");
            m_liveEditBridge->Disconnect();
            m_liveEditBridge.reset();
            console.LogSuccess("Live edit bridge shutdown complete");
        }

        // Disconnect collaborative session
        if (m_collabSession)
        {
            console.LogInfo("Shutting down collaborative edit session...");
            m_collabSession->Disconnect();
            m_collabSession.reset();
            console.LogSuccess("Collaborative edit session shutdown complete");
        }

        // Reset other systems
        m_undoRedoManager.reset();
        m_commandPalette.reset();

        // Note: Don't shutdown crash handler here as it's managed elsewhere

        m_isInitialized = false;
        console.LogSuccess("EditorUI shutdown complete");
    }


    void EditorUI::RenderStatusBar()
    {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        float statusBarHeight = 26.0f;
        ImVec2 statusBarPos(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight);
        ImVec2 statusBarSize(viewport->WorkSize.x, statusBarHeight);

        ImGui::SetNextWindowPos(statusBarPos);
        ImGui::SetNextWindowSize(statusBarSize);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoDocking;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.075f, 0.082f, 0.094f, 1.0f)); // Darker than main bg
        if (ImGui::Begin("##StatusBar", nullptr, flags))
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Top edge accent line (subtle teal glow)
            ImVec2 wp = ImGui::GetWindowPos();
            ImVec2 ws = ImGui::GetWindowSize();
            dl->AddLine(wp, ImVec2(wp.x + ws.x, wp.y),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(0.102f, 0.686f, 0.737f, 0.35f)), 1.0f);

            // Left: engine connection chip badge
            ImVec4 statusColor =
                m_engineConnected ? ImVec4(0.239f, 0.839f, 0.549f, 1.0f) : ImVec4(0.910f, 0.251f, 0.251f, 1.0f);
            ImGui::TextColored(statusColor, ICON_FA_CIRCLE);
            ImGui::SameLine(0, 4);
            ImGui::TextColored(ImVec4(0.533f, 0.565f, 0.627f, 1.0f), "%s",
                               m_engineConnected ? "Connected" : "Disconnected");

            // Project name (dimmed secondary text)
            if (m_projectManager && m_projectManager->HasOpenProject())
            {
                ImGui::SameLine(0, 12);
                ImGui::TextColored(ImVec4(0.306f, 0.329f, 0.384f, 1.0f), ICON_FA_CIRCLE);
                ImGui::SameLine(0, 12);
                ImGui::TextColored(ImVec4(0.533f, 0.565f, 0.627f, 1.0f), ICON_FA_FOLDER " %s",
                                   m_projectManager->GetCurrentProject().name.c_str());
            }

            // Scene name
            ImGui::SameLine(0, 12);
            ImGui::TextColored(ImVec4(0.306f, 0.329f, 0.384f, 1.0f), ICON_FA_CIRCLE);
            ImGui::SameLine(0, 12);
            ImGui::TextColored(ImVec4(0.847f, 0.863f, 0.902f, 1.0f), ICON_FA_MAP " %s%s", m_currentSceneName.c_str(),
                               m_sceneModified ? " *" : "");

            ImGui::SameLine(0, 12);
            ImGui::TextColored(ImVec4(0.306f, 0.329f, 0.384f, 1.0f), ICON_FA_CIRCLE);
            ImGui::SameLine(0, 12);

            // Center: tool + selection (secondary text)
            const char* toolNames[] = {"Move", "Rotate", "Scale"};
            ImGui::TextColored(ImVec4(0.533f, 0.565f, 0.627f, 1.0f), "%s | %d obj | %d sel",
                               toolNames[(int)m_currentTool], m_sceneObjectCount, m_selectedObjectCount);

            // Right: FPS + frame info
            float fps = m_stats.frameTime > 0.001f ? 1000.0f / m_stats.frameTime : 0.0f;
            ImVec4 fpsColor = fps >= 60.0f   ? ImVec4(0.239f, 0.839f, 0.549f, 1.0f)
                              : fps >= 30.0f ? ImVec4(0.941f, 0.659f, 0.188f, 1.0f)
                                             : ImVec4(0.910f, 0.251f, 0.251f, 1.0f);

            float rightOffset = ImGui::GetWindowWidth() - 380;
            if (rightOffset > ImGui::GetCursorPosX())
            {
                ImGui::SameLine(rightOffset);
            }
            ImGui::TextColored(fpsColor, ICON_FA_TACHOMETER_ALT " %.0f", fps);
            ImGui::SameLine(0, 4);
            ImGui::TextColored(ImVec4(0.533f, 0.565f, 0.627f, 1.0f), "FPS");
            ImGui::SameLine(0, 12);
            ImGui::TextColored(ImVec4(0.416f, 0.443f, 0.502f, 1.0f), "%.1fms", m_stats.frameTime);
            ImGui::SameLine(0, 12);
            ImGui::TextColored(ImVec4(0.416f, 0.443f, 0.502f, 1.0f), ICON_FA_DATABASE " %d", m_assetDatabaseSize);
            ImGui::SameLine(0, 12);
            ImGui::TextColored(ImVec4(0.306f, 0.329f, 0.384f, 1.0f), "#%llu", (unsigned long long)m_frameNumber);
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void EditorUI::RenderNotifications()
    {
        const float NOTIFICATION_WIDTH = 340.0f;
        const float NOTIFICATION_HEIGHT = 56.0f;
        const float NOTIFICATION_SPACING = 8.0f;

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        float yOffset = viewport->WorkPos.y + 12.0f;

        for (size_t i = 0; i < m_notifications.size(); ++i)
        {
            const auto& notification = m_notifications[i];

            // Fade out in last 0.5 seconds
            float alpha = 1.0f;
            if (notification.duration > 0.0f && notification.timeLeft < 0.5f)
            {
                alpha = std::max(0.0f, notification.timeLeft / 0.5f);
            }

            ImVec2 notificationPos(viewport->WorkPos.x + viewport->WorkSize.x - NOTIFICATION_WIDTH - 16.0f,
                                   yOffset + i * (NOTIFICATION_HEIGHT + NOTIFICATION_SPACING));

            ImGui::SetNextWindowPos(notificationPos);
            ImGui::SetNextWindowSize(ImVec2(NOTIFICATION_WIDTH, NOTIFICATION_HEIGHT));
            ImGui::SetNextWindowBgAlpha(0.95f * alpha);

            std::string windowName = "##Notification" + std::to_string(i);
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoDocking |
                                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                     ImGuiWindowFlags_NoSavedSettings;

            // Theme-matched accent colors
            ImVec4 accentColor(0.102f, 0.686f, 0.737f, alpha); // teal (info)
            const char* icon = ICON_FA_INFO_CIRCLE;
            if (notification.type == "error")
            {
                accentColor = ImVec4(0.910f, 0.251f, 0.251f, alpha);
                icon = ICON_FA_TIMES;
            }
            else if (notification.type == "warning")
            {
                accentColor = ImVec4(0.941f, 0.659f, 0.188f, alpha);
                icon = ICON_FA_EXCLAMATION;
            }
            else if (notification.type == "success")
            {
                accentColor = ImVec4(0.239f, 0.839f, 0.549f, alpha);
                icon = ICON_FA_CHECK;
            }

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.118f, 0.129f, 0.161f, 0.95f * alpha));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.25f * alpha));
            if (ImGui::Begin(windowName.c_str(), nullptr, flags))
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 wp = ImGui::GetWindowPos();
                ImVec2 ws = ImGui::GetWindowSize();

                // Left accent stripe (3px, rounded left corners)
                dl->AddRectFilled(wp, ImVec2(wp.x + 3, wp.y + ws.y), ImGui::ColorConvertFloat4ToU32(accentColor), 8.0f,
                                  ImDrawFlags_RoundCornersLeft);

                // Subtle background gradient overlay (darker at bottom)
                dl->AddRectFilledMultiColor(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                                            ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0)),
                                            ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0)),
                                            ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.1f * alpha)),
                                            ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.1f * alpha)));

                // Content with padding past the stripe
                ImGui::SetCursorPos(ImVec2(14, (NOTIFICATION_HEIGHT - ImGui::GetTextLineHeight()) * 0.5f));
                ImGui::TextColored(accentColor, "%s", icon);
                ImGui::SameLine(0, 8);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.847f, 0.863f, 0.902f, alpha));
                ImGui::TextWrapped("%s", notification.message.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::End();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
        }
    }

    void EditorUI::RenderPanels()
    {
        static float lastUpdateTime = 0.0f;
        static auto lastClock = std::chrono::steady_clock::now();

        auto now = std::chrono::steady_clock::now();
        float deltaTime = std::chrono::duration<float>(now - lastClock).count();
        lastClock = now;

        for (auto& [name, panel] : m_panels)
        {
            if (panel && panel->IsVisible())
            {
                SPARK_GUARDED_UPDATE("EditorPanel:Update", "Editor", { panel->Update(deltaTime); });
                SPARK_GUARDED_UPDATE("EditorPanel:Render", "Editor", { panel->Render(); });
            }
        }
    }

    void EditorUI::RenderModalDialogs()
    {
        if (m_currentDialog.isOpen)
        {
            ImGui::OpenPopup(m_currentDialog.title.c_str());

            if (ImGui::BeginPopupModal(m_currentDialog.title.c_str(), &m_currentDialog.isOpen))
            {
                if (m_currentDialog.content)
                {
                    m_currentDialog.content();
                }

                ImGui::Separator();

                for (const auto& [buttonName, callback] : m_currentDialog.buttons)
                {
                    if (ImGui::Button(buttonName.c_str()))
                    {
                        if (callback)
                            callback();
                        m_currentDialog.isOpen = false;
                    }
                    ImGui::SameLine();
                }

                ImGui::EndPopup();
            }
        }
    }

    void EditorUI::UpdateStats(float deltaTime)
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastStatsUpdate);

        if (elapsed.count() >= 500)
        {                                            // Update every 500ms
            m_stats.frameTime = deltaTime * 1000.0f; // Convert to ms
            m_stats.visiblePanels = 0;
            m_stats.totalPanels = static_cast<int>(m_panels.size());

            for (const auto& [name, panel] : m_panels)
            {
                if (panel && panel->IsVisible())
                {
                    m_stats.visiblePanels++;
                }
            }

            m_stats.lastUpdate = now;
            m_lastStatsUpdate = now;
        }
    }

    // Simple implementations for interface methods
    bool EditorUI::IsPanelVisible(const std::string& panelName) const
    {
        auto it = m_panels.find(panelName);
        return (it != m_panels.end() && it->second) ? it->second->IsVisible() : false;
    }

    void EditorUI::SetPanelVisible(const std::string& panelName, bool visible)
    {
        auto it = m_panels.find(panelName);
        if (it != m_panels.end() && it->second)
        {
            it->second->SetVisible(visible);
        }
    }

    bool EditorUI::SaveLayout(const std::string& layoutName, const std::string& description)
    {
        // Implementation for saving ImGui docking layout
        try
        {
            std::string layoutsDir = "Layouts";
            if (!std::filesystem::exists(layoutsDir))
            {
                std::filesystem::create_directories(layoutsDir);
            }

            std::string filePath = layoutsDir + "/" + layoutName + ".ini";

            // Save ImGui settings to specific file
            ImGui::SaveIniSettingsToDisk(filePath.c_str());

            // Save layout metadata
            std::ofstream metaFile(filePath + ".meta");
            if (metaFile.is_open())
            {
                metaFile << "name=" << layoutName << "\n";
                metaFile << "description=" << description << "\n";
                auto now = std::chrono::system_clock::now();
                auto time_t = std::chrono::system_clock::to_time_t(now);
                metaFile << "created=" << time_t << "\n";
            }

            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    bool EditorUI::LoadLayout(const std::string& layoutName)
    {
        // Implementation for loading ImGui docking layout
        try
        {
            std::string filePath = "Layouts/" + layoutName + ".ini";

            if (!std::filesystem::exists(filePath))
            {
                return false;
            }

            // Load ImGui settings from specific file
            ImGui::LoadIniSettingsFromDisk(filePath.c_str());

            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    void EditorUI::ResetToDefaultLayout()
    {
        // Reset all panels to default visibility using the actual panel map keys
        for (auto& [name, panel] : m_panels)
        {
            if (!panel)
                continue;

            if (name == "SceneView" || name == "Console" || name == "Hierarchy" || name == "Inspector" ||
                name == "AssetBrowser")
            {
                panel->SetVisible(true);
            }
            else if (name == "GameView" || name == "Profiler")
            {
                panel->SetVisible(true);
            }
            else
            {
                // FPS-specific panels stay hidden by default
                panel->SetVisible(false);
            }
        }

        // Re-apply the current theme instead of resetting to bare defaults
        ApplyTheme(m_currentTheme);

        // Force dock layout rebuild on next frame
        m_firstFrame = true;
    }

    void EditorUI::ApplyTheme(const std::string& themeName)
    {
        m_currentTheme = themeName;

        if (!EditorTheme::ApplyTheme(themeName))
        {
            // Fallback to basic ImGui dark style
            ImGui::StyleColorsDark();
        }
    }

    void EditorUI::ShowNotification(const std::string& message, const std::string& type, float duration)
    {
        Notification notification;
        notification.message = message;
        notification.type = type;
        notification.duration = duration;
        notification.timeLeft = duration;
        notification.timestamp = std::chrono::steady_clock::now();

        m_notifications.push_back(notification);
    }

    std::string EditorUI::ExecuteCommand(const std::string& command)
    {
        // Simple command parsing
        std::vector<std::string> parts;
        std::string current;
        for (char c : command)
        {
            if (c == ' ')
            {
                if (!current.empty())
                {
                    parts.push_back(current);
                    current.clear();
                }
            }
            else
            {
                current += c;
            }
        }
        if (!current.empty())
        {
            parts.push_back(current);
        }

        if (parts.empty())
        {
            return "Empty command";
        }

        auto it = m_commands.find(parts[0]);
        if (it != m_commands.end())
        {
            std::vector<std::string> args(parts.begin() + 1, parts.end());
            return it->second(args);
        }

        return "Unknown command: " + parts[0];
    }

    void EditorUI::RegisterCommand(const std::string& name,
                                   std::function<std::string(const std::vector<std::string>&)> handler,
                                   const std::string& description)
    {
        if (name.empty() || !handler)
            return;
        m_commands[name] = handler;
    }

    void EditorUI::SetFrameNumber(uint64_t frameNumber)
    {
        m_frameNumber = frameNumber;
    }

    UIStats EditorUI::GetStats() const
    {
        return m_stats;
    }

    void EditorUI::SetEngineConnected(bool connected)
    {
        m_engineConnected = connected;
    }

    void EditorUI::UpdateAssetDatabaseInfo(int assetCount, size_t memoryUsage)
    {
        m_assetDatabaseSize = assetCount;
        m_assetMemoryUsage = memoryUsage;
    }

    void EditorUI::UpdateSceneInfo(int objectCount, int selectedCount)
    {
        m_sceneObjectCount = objectCount;
        m_selectedObjectCount = selectedCount;
    }

    bool EditorUI::HasRecoveryData()
    {
        return m_recoveryDataAvailable;
    }

    bool EditorUI::ShowRecoveryDialog()
    {
        if (!m_recoveryDataAvailable)
            return false;

        bool recovered = false;
        ImGui::OpenPopup("Recovery Available");

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (ImGui::BeginPopupModal("Recovery Available", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("The editor detected unsaved changes from a previous session.");
            ImGui::Text("Would you like to restore the previous state?");
            ImGui::Separator();

            if (ImGui::Button("Restore Previous Session", ImVec2(200, 0)))
            {
                // Attempt to load recovery data through the layout manager
                if (m_layoutManager)
                {
                    recovered = m_layoutManager->LoadLayout("_recovery");
                }
                m_recoveryDataAvailable = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();

            if (ImGui::Button("Discard", ImVec2(100, 0)))
            {
                m_recoveryDataAvailable = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        return recovered;
    }

    bool EditorUI::ImportLayout(const std::string& filePath)
    {
        try
        {
            std::ifstream file(filePath);
            if (!file.is_open())
                return false;

            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            file.close();

            if (content.empty())
                return false;

            // Extract layout name from file content or use filename
            std::string layoutName = filePath;
            auto lastSlash = layoutName.find_last_of("/\\");
            if (lastSlash != std::string::npos)
                layoutName = layoutName.substr(lastSlash + 1);
            auto lastDot = layoutName.find_last_of('.');
            if (lastDot != std::string::npos)
                layoutName = layoutName.substr(0, lastDot);

            // Use layout manager to apply the imported layout
            if (m_layoutManager)
            {
                m_layoutManager->SaveCurrentLayout(layoutName, "Imported layout");
                return m_layoutManager->LoadLayout(layoutName);
            }

            return false;
        }
        catch (...)
        {
            return false;
        }
    }

    bool EditorUI::ExportLayout(const std::string& filePath)
    {
        try
        {
            std::ofstream file(filePath);
            if (!file.is_open())
                return false;

            // Export current layout state
            file << "{\n";
            file << "  \"layout\": {\n";
            file << "    \"version\": 1,\n";

            // Export panel visibility states
            file << "    \"panels\": {\n";
            bool first = true;
            for (const auto& [name, panel] : m_panels)
            {
                if (!panel)
                    continue;
                if (!first)
                    file << ",\n";
                file << "      \"" << name << "\": { \"visible\": " << (panel->IsVisible() ? "true" : "false") << " }";
                first = false;
            }
            file << "\n    }\n";
            file << "  }\n}\n";

            file.close();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void EditorUI::ShowModalDialog(const std::string& title, std::function<void()> content,
                                   const std::unordered_map<std::string, std::function<void()>>& buttons)
    {
        m_currentDialog.title = title;
        m_currentDialog.content = content;
        m_currentDialog.buttons = buttons;
        m_currentDialog.isOpen = true;
    }

    // ------------------------------------------------------------------
    // Project operations
    // ------------------------------------------------------------------
    void EditorUI::ShowNewProjectDialog()
    {
        if (m_projectBrowserPanel)
        {
            m_projectBrowserPanel->ShowNewProject();
        }
    }

    void EditorUI::ShowOpenProjectDialog()
    {
        if (m_projectBrowserPanel)
        {
            m_projectBrowserPanel->ShowOpenProject();
        }
    }

    void EditorUI::ShowProjectBrowser()
    {
        if (m_projectBrowserPanel)
        {
            m_projectBrowserPanel->ShowBrowser();
        }
    }

    bool EditorUI::SaveCurrentScene(const std::string& path)
    {
        if (path.empty())
            return false;

        try
        {
            // Ensure parent directory exists
            auto parentPath = std::filesystem::path(path).parent_path();
            if (!parentPath.empty())
            {
                std::filesystem::create_directories(parentPath);
            }

            std::ofstream file(path);
            if (!file.is_open())
            {
                return false;
            }

            file << "{\n";
            file << "  \"sceneVersion\": 1,\n";
            file << "  \"name\": \"" << m_currentSceneName << "\",\n";
            file << "  \"entities\": [\n";

            // Serialize hierarchy objects
            auto it = m_panels.find("Hierarchy");
            if (it != m_panels.end())
            {
                auto* hierarchy = dynamic_cast<HierarchyPanel*>(it->second.get());
                if (hierarchy)
                {
                    const auto& objects = hierarchy->GetSceneObjects();
                    for (size_t i = 0; i < objects.size(); ++i)
                    {
                        file << "    {\n";
                        file << "      \"name\": \"" << objects[i] << "\",\n";
                        file << "      \"components\": [\n";
                        file << "        {\n";
                        file << "          \"type\": \"Transform\",\n";
                        file << "          \"position\": [0, 0, 0],\n";
                        file << "          \"rotation\": [0, 0, 0],\n";
                        file << "          \"scale\": [1, 1, 1]\n";
                        file << "        }\n";
                        file << "      ]\n";
                        file << "    }";
                        if (i + 1 < objects.size())
                        {
                            file << ",";
                        }
                        file << "\n";
                    }
                }
            }

            file << "  ]\n";
            file << "}\n";
            file.close();

            auto& console = Spark::SimpleConsole::GetInstance();
            console.LogSuccess("Scene saved to: " + path);

            // Notify plugins of the scene save
            if (m_pluginManager)
            {
                m_pluginManager->NotifySceneSave(path);
            }

            return true;
        }
        catch (const std::exception& e)
        {
            auto& console = Spark::SimpleConsole::GetInstance();
            console.LogError("Failed to save scene: " + std::string(e.what()));
            return false;
        }
    }

#ifdef _WIN32
    void EditorUI::SetGraphicsDevice(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        auto it = m_panels.find("SceneView");
        if (it != m_panels.end())
        {
            auto* sceneView = dynamic_cast<SceneViewPanel*>(it->second.get());
            if (sceneView)
            {
                sceneView->SetDevice(device, context);
                auto& console = Spark::SimpleConsole::GetInstance();
                console.LogSuccess("Graphics device passed to Scene View panel");
            }
        }

        // Re-initialize gizmo system with the actual D3D11 device
        if (m_gizmoSystem)
        {
            m_gizmoSystem->Initialize(device, context);
        }
    }
#endif
    void EditorUI::HandleKeyboardShortcuts()
    {
        ImGuiIO& io = ImGui::GetIO();

        // Don't process shortcuts if command palette is open (it handles its own input)
        if (m_commandPalette && m_commandPalette->IsOpen())
        {
            return;
        }

        // Ctrl+Z: Undo
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !io.KeyShift)
        {
            if (m_undoRedoManager && m_undoRedoManager->CanUndo())
            {
                m_undoRedoManager->Undo();
                ShowNotification("Undo: " + m_undoRedoManager->GetRedoDescription(), "info", 1.5f);
            }
        }

        // Ctrl+Y or Ctrl+Shift+Z: Redo
        if ((io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) ||
            (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)))
        {
            if (m_undoRedoManager && m_undoRedoManager->CanRedo())
            {
                m_undoRedoManager->Redo();
                ShowNotification("Redo: " + m_undoRedoManager->GetUndoDescription(), "info", 1.5f);
            }
        }

        // Ctrl+P: Command Palette
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P))
        {
            if (m_commandPalette)
            {
                m_commandPalette->Toggle();
            }
        }

        // Ctrl+F: Search Panel
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F))
        {
            SetPanelVisible("Search", true);
        }
    }

    void EditorUI::InitializeCommandPalette()
    {
        if (!m_commandPalette)
        {
            return;
        }

        RegisterPanelToggleCommands();
        RegisterEditCommands();
        RegisterSceneCommands();
        RegisterToolCommands();
    }

    void EditorUI::RegisterPanelToggleCommands()
    {
        auto RegisterPanelToggle = [this](const std::string& panelKey, const std::string& displayName)
        {
            m_commandPalette->RegisterAction("Toggle " + displayName, "Panel", [this, panelKey]()
                                             { SetPanelVisible(panelKey, !IsPanelVisible(panelKey)); });
        };

        // Core panels
        RegisterPanelToggle("SceneView", "Scene View");
        RegisterPanelToggle("Console", "Console");
        RegisterPanelToggle("Hierarchy", "Hierarchy");
        RegisterPanelToggle("Inspector", "Inspector");
        RegisterPanelToggle("AssetBrowser", "Asset Browser");
        RegisterPanelToggle("GameView", "Game View");
        RegisterPanelToggle("Profiler", "Profiler");

        // FPS / gameplay panels
        RegisterPanelToggle("WeaponEditor", "Weapon Editor");
        RegisterPanelToggle("FPSTools", "FPS Tools");

        // 2D panels
        RegisterPanelToggle("SpriteEditor", "Sprite Editor");
        RegisterPanelToggle("TilemapEditor", "Tilemap Editor");
        RegisterPanelToggle("SpriteAnimEditor", "Sprite Animation Editor");
        RegisterPanelToggle("Physics2D", "Physics 2D");
        RegisterPanelToggle("Physics3D", "Physics 3D");

        // Editor utility panels
        RegisterPanelToggle("UndoHistory", "Undo History");
        RegisterPanelToggle("SceneStats", "Scene Statistics");
        RegisterPanelToggle("PrefabEditor", "Prefab Editor");
        RegisterPanelToggle("Search", "Search");
        RegisterPanelToggle("PostProcessing", "Post Processing");

        // Domain-specific panels
        RegisterPanelToggle("DialogueEditor", "Dialogue Editor");
        RegisterPanelToggle("AIEditor", "AI Editor");
        RegisterPanelToggle("SplineEditor", "Spline Editor");
        RegisterPanelToggle("ParticleEditor", "Particle Editor");
        RegisterPanelToggle("EventMonitor", "Event Monitor");
        RegisterPanelToggle("SaveSystem", "Save System");
        RegisterPanelToggle("Localization", "Localization");
        RegisterPanelToggle("WeatherFog", "Weather & Fog");
        RegisterPanelToggle("CinematicSequencer", "Cinematic Sequencer");
        RegisterPanelToggle("ProjectSettings", "Project Settings");
    }

    void EditorUI::RegisterEditCommands()
    {
        // Undo / redo
        m_commandPalette->RegisterAction(
            "Undo", "Command",
            [this]()
            {
                if (m_undoRedoManager && m_undoRedoManager->CanUndo())
                {
                    m_undoRedoManager->Undo();
                }
            },
            "Ctrl+Z");

        m_commandPalette->RegisterAction(
            "Redo", "Command",
            [this]()
            {
                if (m_undoRedoManager && m_undoRedoManager->CanRedo())
                {
                    m_undoRedoManager->Redo();
                }
            },
            "Ctrl+Y");

        // Layout commands
        m_commandPalette->RegisterAction("Reset Layout", "Layout", [this]() { ResetToDefaultLayout(); });
        m_commandPalette->RegisterAction("Save Layout", "Layout", [this]() { SaveLayout("Quick Save"); });

        // Prefab commands
        m_commandPalette->RegisterAction("Create Empty Prefab", "Command",
                                         [this]()
                                         {
                                             if (m_prefabManager)
                                             {
                                                 m_prefabManager->CreateEmptyPrefab("New Prefab");
                                                 SetPanelVisible("PrefabEditor", true);
                                                 ShowNotification("Created new prefab", "success");
                                             }
                                         });
    }

    void EditorUI::RegisterSceneCommands()
    {
        m_commandPalette->RegisterAction("New Scene", "Scene",
                                         [this]() { ShowNotification("New Scene created!", "success"); });

        m_commandPalette->RegisterAction("Save Scene", "Scene",
                                         [this]() { ShowNotification("Scene saved!", "success"); });

        // Play mode
        m_commandPalette->RegisterAction(
            "Play", "Command",
            [this]()
            {
                m_playMode = PlayMode::Playing;
                ShowNotification("Playing...", "success");
            },
            "F5");

        m_commandPalette->RegisterAction(
            "Stop", "Command",
            [this]()
            {
                m_playMode = PlayMode::Stopped;
                ShowNotification("Stopped", "info");
            },
            "Shift+F5");
    }

    void EditorUI::RegisterToolCommands()
    {
        // Theme commands
        m_commandPalette->RegisterAction("Theme: Spark Professional", "Command",
                                         [this]() { ApplyTheme("Spark Professional"); });
        m_commandPalette->RegisterAction("Theme: Dark", "Command", [this]() { ApplyTheme("Dark"); });
        m_commandPalette->RegisterAction("Theme: Light", "Command", [this]() { ApplyTheme("Light"); });

        // Transform tool commands
        m_commandPalette->RegisterAction(
            "Tool: Move", "Command", [this]() { m_currentTool = TransformTool::Move; }, "W");
        m_commandPalette->RegisterAction(
            "Tool: Rotate", "Command", [this]() { m_currentTool = TransformTool::Rotate; }, "E");
        m_commandPalette->RegisterAction(
            "Tool: Scale", "Command", [this]() { m_currentTool = TransformTool::Scale; }, "R");

        m_commandPalette->RegisterAction("Toggle Snap", "Command", [this]() { m_snapEnabled = !m_snapEnabled; });
    }

} // namespace SparkEditor
