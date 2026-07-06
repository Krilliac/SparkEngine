/**
 * @file EditorUI.cpp
 * @brief Core editor UI system — lifecycle, rendering dispatch, layout, commands
 *
 * Panel creation is in EditorPanelFactory.cpp.
 * Menu bar and toolbar rendering are in EditorMenuBar.cpp.
 */

#include "EditorUI.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "SceneManager/ReflectedSceneSerializer.h" // Spark::SaveWorld/LoadWorld — full-fidelity scene round-trip (C4)
#include "EditorTheme.h"
#include "EditorFonts.h"
#include "EditorIcons.h"
#include "Core/FaultIsolation.h"
#include "Utils/SparkConsole.h"
#include "Utils/Validate.h"
#include "Utils/LogMacros.h"
#include "../Panels/SceneViewPanel.h"
#include "../Panels/ConsolePanel.h"
#include "../Panels/AssetAuditGraph.h"
#include "../Panels/HierarchyPanel.h"
#include "../Panels/SelectionManager.h"
#include "TutorialSystem.h"
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
#include "../Workflow/BuiltinWorkflows.h"
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
        m_notificationManager = std::make_unique<EditorNotificationManager>();
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

            // Register built-in editor workflows (Build, Scene, etc.)
            RegisterBuiltinWorkflows();

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

            // Apply startup theme (CLI --theme override, else Spark Ember — matches hi-fi design)
            const std::string themeName = config.startupTheme.empty() ? "Spark Ember" : config.startupTheme;
            console.LogInfo("Applying theme: " + themeName);
            ApplyTheme(themeName);
            console.LogSuccess("Theme applied");

            m_isInitialized = true;
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorUI initialized successfully");
            console.LogSuccess("Enhanced EditorUI initialized successfully");
            return true;
        }
        catch (const std::exception& e)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "EditorUI::Initialize exception: %s", e.what());
            console.LogError("Exception in EditorUI::Initialize: " + std::string(e.what()));
            return false;
        }
        catch (...)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "EditorUI::Initialize unknown exception");
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

        // Layout manager — disk-backed panel state persistence
        console.LogInfo("Initializing layout manager...");
        m_layoutManager = std::make_unique<EditorLayoutManager>();
        if (m_layoutManager->Initialize("Layouts"))
        {
            console.LogSuccess("Layout manager initialized (dir=Layouts)");
        }
        else
        {
            console.LogWarning("Layout manager initialization failed — layouts will not persist");
        }

        // Selection manager — centralized editor selection singleton.
        // HierarchyPanel mirrors its selection state into this singleton
        // (NotifySelectionChanged → SelectionManager::SelectMultiple) and
        // InspectorPanel observes it (OnSelectionChanged → SetInspected
        // ObjectByID). Both keys are now uint64_t after the EntityId
        // widening, so no narrowing conversion is required. Initialize
        // the manager BEFORE any panel's Initialize() call so that the
        // panels can register their callbacks against a live singleton.
        console.LogInfo("Initializing selection manager...");
        SelectionManager::GetInstance().Initialize();
        console.LogSuccess("Selection manager initialized");

        // Tutorial system — registers built-in tutorial sequences on
        // Initialize(). Update() is ticked from EditorUI::Update to drive
        // the auto-advance timers.
        console.LogInfo("Initializing tutorial system...");
        TutorialSystem::GetInstance().Initialize();
        console.LogSuccess("Tutorial system initialized (" +
                           std::to_string(TutorialSystem::GetInstance().GetAvailableTutorials().size()) +
                           " tutorials registered)");

        // Asset audit graph — editor-facing dependency / unused / size
        // budgeting graph. Not to be confused with
        // AssetPipeline::AssetDependencyGraph (build-system topological
        // graph); both classes coexist and serve different purposes.
        console.LogInfo("Initializing asset audit graph...");
        AssetAuditGraph::GetInstance().Initialize();
        console.LogSuccess("Asset audit graph initialized");

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
    // NOTE: Intentionally exceeds 50-line guideline — linear event subscription wiring
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
        m_notificationManager->Update(deltaTime);

        // Tick tutorial auto-advance timers so active tutorials progress.
        SPARK_GUARDED_UPDATE("TutorialSystem", "Editor", { TutorialSystem::GetInstance().Update(deltaTime); });

        // Tick play/simulate state machine (PIE and in-editor simulation stepping/stats)
        m_playModeManager.Update(deltaTime);

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
                m_playMode = m_playModeManager.IsPlaying()
                                 ? PlayMode::Playing
                                 : (m_playModeManager.IsSimulating()
                                        ? PlayMode::Simulating
                                        : (m_playModeManager.IsPaused() ? PlayMode::Paused : PlayMode::Stopped));
                ShowNotification(m_playMode == PlayMode::Playing
                                     ? "Playing..."
                                     : (m_playMode == PlayMode::Simulating ? "Simulating..." : "Stopped"),
                                 "info", 2.0f);
            }
        }

        // F6: Toggle simulation mode (physics/AI/etc. while retaining editor camera workflow)
        if (ImGui::IsKeyPressed(ImGuiKey_F6) && !io.WantTextInput)
        {
            if (io.KeyShift)
            {
                m_playModeManager.ExitPlayMode();
                m_playMode = PlayMode::Stopped;
                ShowNotification("Stopped simulation", "info", 2.0f);
            }
            else
            {
                m_playModeManager.ToggleSimulationMode();
                m_playMode = m_playModeManager.IsPlaying()
                                 ? PlayMode::Playing
                                 : (m_playModeManager.IsSimulating()
                                        ? PlayMode::Simulating
                                        : (m_playModeManager.IsPaused() ? PlayMode::Paused : PlayMode::Stopped));
                ShowNotification(m_playMode == PlayMode::Simulating ? "Simulation running..." : "Simulation stopped",
                                 "info", 2.0f);
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

    void EditorUI::Render()
    // NOTE: Intentionally exceeds 50-line guideline — linear rendering pipeline dispatch
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
        m_notificationManager->Render();
        RenderModalDialogs();
        RenderWelcomeScreen();

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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Setting up default dock layout");
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

        // Shutdown asset audit graph singleton
        console.LogInfo("Shutting down asset audit graph...");
        AssetAuditGraph::GetInstance().Shutdown();
        console.LogSuccess("Asset audit graph shutdown complete");

        // Shutdown tutorial system singleton
        console.LogInfo("Shutting down tutorial system...");
        TutorialSystem::GetInstance().Shutdown();
        console.LogSuccess("Tutorial system shutdown complete");

        // Shutdown selection manager singleton
        console.LogInfo("Shutting down selection manager...");
        SelectionManager::GetInstance().Shutdown();
        console.LogSuccess("Selection manager shutdown complete");

        // Shutdown layout manager
        if (m_layoutManager)
        {
            console.LogInfo("Shutting down layout manager...");
            m_layoutManager->Shutdown();
            m_layoutManager.reset();
            console.LogSuccess("Layout manager shutdown complete");
        }

        // Reset other systems
        m_undoRedoManager.reset();
        m_commandPalette.reset();

        // Note: Don't shutdown crash handler here as it's managed elsewhere

        m_isInitialized = false;
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorUI shutdown complete");
        console.LogSuccess("EditorUI shutdown complete");
    }


    void EditorUI::RenderStatusBar()
    // NOTE: Intentionally exceeds 50-line guideline — linear UI layout code
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

    void EditorUI::RenderWelcomeScreen()
    {
        if (!m_showWelcomeScreen)
            return;

        // Skip welcome screen in test mode (automated testing)
        if (m_config && m_config->testMode)
        {
            m_showWelcomeScreen = false;
            return;
        }

        ImGui::OpenPopup("Welcome to SparkEngine");

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_Appearing);

        if (ImGui::BeginPopupModal("Welcome to SparkEngine", &m_showWelcomeScreen,
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        {
            ImGui::TextWrapped("Welcome! SparkEngine is a C++23 open-source 3D game engine.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Getting Started");
            ImGui::BulletText("Scene View — navigate the 3D viewport (WASD + mouse)");
            ImGui::BulletText("Hierarchy — view and select entities in the scene");
            ImGui::BulletText("Inspector — edit properties of the selected entity");
            ImGui::BulletText("Asset Browser — browse and import project assets");
            ImGui::BulletText("Console — view logs and run debug commands");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Quick Actions");
            if (ImGui::Button("Open All Panels"))
            {
                for (auto& [name, panel] : m_panels)
                    panel->SetVisible(true);
                m_showWelcomeScreen = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep Minimal Layout"))
            {
                m_showWelcomeScreen = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Tip: Open more panels from the Window menu at any time.");
            ImGui::TextDisabled("Tip: Use Window > Reset Layout to restore the default.");

            ImGui::EndPopup();
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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Saving layout: %s", layoutName.c_str());
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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Loading layout: %s", layoutName.c_str());
        // Implementation for loading ImGui docking layout
        try
        {
            std::string filePath = "Layouts/" + layoutName + ".ini";

            if (!std::filesystem::exists(filePath))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Editor, "Layout file not found: %s", filePath.c_str());
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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Resetting to default layout");
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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Applying theme: %s", themeName.c_str());
        m_currentTheme = themeName;

        if (!EditorTheme::ApplyTheme(themeName))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Theme '%s' not found, falling back to ImGui dark",
                           themeName.c_str());
            // Fallback to basic ImGui dark style
            ImGui::StyleColorsDark();
        }
    }

    void EditorUI::ShowNotification(const std::string& message, const std::string& type, float duration)
    {
        m_notificationManager->Show(message, type, duration);
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

    void EditorUI::RewirePanelsToWorld()
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        // SceneView caches a raw ::World* — re-point it at the current
        // m_world (used both for the initial seed wiring in
        // SetGraphicsDevice() and for OpenScene(), which replaces m_world).
        auto svIt = m_panels.find("SceneView");
        if (svIt != m_panels.end())
        {
            auto* sceneView = dynamic_cast<SceneViewPanel*>(svIt->second.get());
            if (sceneView)
            {
                sceneView->SetWorld(m_world.get());
            }
        }

        // Hierarchy caches a raw ::World* too.
        auto hierarchyIt = m_panels.find("Hierarchy");
        if (hierarchyIt != m_panels.end())
        {
            auto* hierarchy = dynamic_cast<HierarchyPanel*>(hierarchyIt->second.get());
            if (hierarchy)
            {
                hierarchy->SetWorld(m_world.get());
            }
        }

        // Inspector reads EditorUI::GetWorld()/GetSelectedEntity() live each
        // frame — no re-wire needed.

        // The previously-selected entity belongs to the old World; clear it
        // so the Inspector doesn't try to reflect a stale/foreign handle.
        m_selectedEntity = entt::null;

        console.LogSuccess("Panels rewired to current World");
    }

    bool EditorUI::SaveCurrentScene(const std::string& path)
    {
        if (path.empty())
            return false;

        if (!m_world)
            return false;

        try
        {
            // Ensure parent directory exists
            auto parentPath = std::filesystem::path(path).parent_path();
            if (!parentPath.empty())
            {
                std::filesystem::create_directories(parentPath);
            }

            // Full-fidelity save via the reflection-driven scene serializer
            // (replaces the old lossy names-only JSON writer). The live ECS
            // World is the single source of truth for scene content.
            if (!Spark::SaveWorld(*m_world, path))
            {
                auto& console = Spark::SimpleConsole::GetInstance();
                console.LogError("Failed to save scene (Spark::SaveWorld): " + path);
                return false;
            }

            m_currentScenePath = path;
            m_sceneModified = false;

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

    bool EditorUI::OpenScene(const std::string& path)
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        if (path.empty())
            return false;

        // Load into a fresh World first so a failed/partial load never
        // corrupts the World currently being edited.
        auto fresh = std::make_unique<::World>();
        if (!Spark::LoadWorld(*fresh, path))
        {
            console.LogError("Failed to open scene (Spark::LoadWorld): " + path);
            return false;
        }

        m_world = std::move(fresh);

        // SceneView/Hierarchy cache a raw ::World*; re-point them now that
        // m_world has been replaced, or they'd dangle the freed old World.
        RewirePanelsToWorld();

        m_currentScenePath = path;
        m_currentSceneName = std::filesystem::path(path).stem().string();
        m_sceneModified = false;

        console.LogSuccess("Scene opened from: " + path);

        if (m_pluginManager)
        {
            m_pluginManager->NotifySceneLoad(path);
        }

        return true;
    }

#ifdef _WIN32
    void EditorUI::SetGraphicsDevice(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        // Attach a GraphicsEngine to the editor's own device (no swapchain —
        // the editor drives its own render targets). This lets SceneViewPanel
        // (and any future panel) render a Spark::World via the shared
        // Spark::RenderWorldBasic() basic-shader path.
        m_graphics = std::make_unique<GraphicsEngine>();
        if (FAILED(m_graphics->InitializeFromDevice(device, context)))
        {
            console.LogError("EditorUI: GraphicsEngine::InitializeFromDevice failed — Scene View will not render geometry");
            m_graphics.reset();
        }
        else
        {
            console.LogSuccess("EditorUI: GraphicsEngine attached to editor device");
        }

        // Create the single live ECS World (the document being edited) and seed
        // it with the demo entity, moved here from SceneViewPanel (Unit C1).
        // This is the shared World that Hierarchy/Inspector/Save (C2/C3/C4)
        // will operate on.
        if (!m_world)
        {
            m_world = std::make_unique<::World>();
            ::EntityID e = m_world->CreateEntity("Soldier");
            m_world->AddComponent<::Transform>(e); // identity at origin
            ::MeshRenderer& mr = m_world->AddComponent<::MeshRenderer>(e);
            // Non-empty path required — WorldMeshCache::GetOrLoad early-returns on
            // an empty path.
            mr.meshPath = "Assets/Models/MMOFPS/characters/soldier.obj";
        }

        auto it = m_panels.find("SceneView");
        if (it != m_panels.end())
        {
            auto* sceneView = dynamic_cast<SceneViewPanel*>(it->second.get());
            if (sceneView)
            {
                sceneView->SetDevice(device, context);
                sceneView->SetGraphics(m_graphics.get());
                console.LogSuccess("Graphics device passed to Scene View panel");
            }
        }

        // Wire the Hierarchy panel's selection sink (one-time; the panel
        // object persists across future World swaps, so this doesn't need
        // to be repeated by RewirePanelsToWorld()). List/create/delete/select
        // real ECS entities instead of the legacy (dormant) SceneFile tree.
        auto hierarchyIt = m_panels.find("Hierarchy");
        if (hierarchyIt != m_panels.end())
        {
            auto* hierarchy = dynamic_cast<HierarchyPanel*>(hierarchyIt->second.get());
            if (hierarchy)
            {
                hierarchy->SetSelectionSink(this);
            }
        }

        // Seed both panels' cached ::World* (this call also handles the
        // SceneView/Hierarchy SetWorld() previously done inline here) and
        // any subsequent OpenScene() reuses the same path.
        RewirePanelsToWorld();

        // Wire the Inspector panel to EditorUI (Unit C3) so it can read the
        // live World + selected entity each frame and render/edit the
        // entity's real engine components via reflection, instead of the
        // legacy (dormant) SceneFile-backed inspector.
        auto inspectorIt = m_panels.find("Inspector");
        if (inspectorIt != m_panels.end())
        {
            auto* inspector = dynamic_cast<InspectorPanel*>(inspectorIt->second.get());
            if (inspector)
            {
                inspector->SetEditorUI(this);
                console.LogSuccess("EditorUI wired to Inspector panel (World-backed ECS inspector)");
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

} // namespace SparkEditor
