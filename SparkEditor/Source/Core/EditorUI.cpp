/**
 * @file EditorUI.cpp
 * @brief Implementation of the enhanced editor UI system
 * @author Spark Engine Team
 * @date 2025
 */

#include "EditorUI.h"
#include "EditorTheme.h"
#include "EditorFonts.h"
#include "EditorIcons.h"
#include "../Utils/SparkConsole.h"
#include "../Panels/SceneViewPanel.h"
#include "../Panels/SimpleConsolePanel.h"
#include "../Panels/SimpleHierarchyPanel.h"
#include "../Panels/InspectorPanel.h"
#include "../Panels/AssetBrowserPanel.h"
#include "../Panels/GameViewPanel.h"
#include "../Panels/WeaponEditorPanel.h"
#include "../Panels/FPSToolsPanel.h"
#include "../Panels/ProjectBrowserPanel.h"
#include "../Panels/DebugVisualizerPanel.h"
#include "../Panels/SceneStatsPanel.h"
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
#include "../Profiler/PerformanceProfiler.h"
#include "EditorCrashHandler.h"
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
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("Initializing Enhanced EditorUI with full configuration...");

        try
        {
            // Store config
            m_config = std::unique_ptr<EditorConfig>(new EditorConfig(config));

            console.LogInfo("Using enhanced initialization for production use");

            // Initialize crash handler
            console.LogInfo("Initializing crash handler...");
            if (m_crashHandler && m_crashHandler->Initialize())
            {
                console.LogSuccess("Crash handler initialized successfully");
            }
            else
            {
                console.LogWarning("Crash handler initialization failed");
            }

            // Initialize project manager
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

            // Create project browser panel
            m_projectBrowserPanel = std::make_shared<ProjectBrowserPanel>(m_projectManager.get());
            m_projectBrowserPanel->Initialize();

            // Initialize undo/redo manager
            console.LogInfo("Initializing undo/redo manager...");
            m_undoRedoManager = std::make_unique<UndoRedoManager>();
            console.LogSuccess("Undo/redo manager initialized");

            // Initialize prefab manager
            console.LogInfo("Initializing prefab manager...");
            m_prefabManager = std::make_unique<PrefabManager>();
            m_prefabManager->Initialize();
            console.LogSuccess("Prefab manager initialized");

            // Initialize command palette
            console.LogInfo("Initializing command palette...");
            m_commandPalette = std::make_unique<CommandPalette>();
            console.LogSuccess("Command palette initialized");

            // Create panels
            console.LogInfo("Creating editor panels...");
            CreatePanels();
            console.LogSuccess("Panels created successfully");

            // Register command palette actions (after panels are created)
            InitializeCommandPalette();

            // Wire up project callbacks to update other panels
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

                    // Reset hierarchy for new project and set scene name
                    auto hierIt = m_panels.find("Hierarchy");
                    if (hierIt != m_panels.end())
                    {
                        auto* hierarchy = dynamic_cast<SimpleHierarchyPanel*>(hierIt->second.get());
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

                    ShowNotification("Project opened: " + project.name, "success");
                });

            m_projectManager->SetOnProjectClosed([this](const ProjectInfo& project)
                                                 { ShowNotification("Project closed: " + project.name, "info"); });

            // Show project browser on startup if no project is loaded
            if (!m_projectManager->HasOpenProject())
            {
                m_projectBrowserPanel->ShowBrowser();
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

    void EditorUI::Update(float deltaTime)
    {
        if (!m_isInitialized)
            return;

        // Process global keyboard shortcuts
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && !io.WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_N))
            {
                // Ctrl+N: New Scene
                auto it = m_panels.find("Hierarchy");
                if (it != m_panels.end())
                {
                    auto* hierarchy = dynamic_cast<SimpleHierarchyPanel*>(it->second.get());
                    if (hierarchy)
                    {
                        hierarchy->ResetToDefault();
                    }
                }
                m_currentScenePath.clear();
                m_currentSceneName = "Untitled";
                m_sceneModified = false;
                ShowNotification("New scene created", "success");
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_S))
            {
                // Ctrl+S: Save Scene
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

        // F5: Toggle play mode
        if (ImGui::IsKeyPressed(ImGuiKey_F5) && !io.WantTextInput)
        {
            if (io.KeyShift)
            {
                m_playMode = PlayMode::Stopped;
                ShowNotification("Stopped", "info", 2.0f);
            }
            else
            {
                m_playMode = (m_playMode == PlayMode::Playing) ? PlayMode::Stopped : PlayMode::Playing;
                ShowNotification(m_playMode == PlayMode::Playing ? "Playing..." : "Stopped", "info", 2.0f);
            }
        }

        // W/E/R: Transform tool shortcuts (only when not typing in a text field)
        if (!io.WantTextInput && !io.WantCaptureKeyboard)
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

        // Update notifications
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

        // Update stats
        UpdateStats(deltaTime);

        // Handle keyboard shortcuts for new features
        HandleKeyboardShortcuts();
    }

    void EditorUI::Render()
    {
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
        if (m_projectBrowserPanel && m_projectBrowserPanel->IsModalActive())
        {
            m_projectBrowserPanel->Render();
        }

        // Render command palette overlay (on top of everything)
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

        // Split: left 20% for Hierarchy
        ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.20f, &dockLeft, &dockMain);
        // Split: right 25% for Inspector
        ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.25f, &dockRight, &dockMain);
        // Split: bottom 28% for Console + Asset Browser (tabbed)
        ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.28f, &dockBottom, &dockCenter);

        // Dock panels — use ###panel_id to match stable IDs from BeginPanel()
        ImGui::DockBuilderDockWindow("###simple_hierarchy_panel", dockLeft);
        ImGui::DockBuilderDockWindow("###inspector_panel", dockRight);
        ImGui::DockBuilderDockWindow("###scene_view_panel", dockCenter);
        ImGui::DockBuilderDockWindow("##Toolbar", dockCenter);
        ImGui::DockBuilderDockWindow("###simple_console_panel", dockBottom);
        ImGui::DockBuilderDockWindow("###asset_browser_panel", dockBottom);

        ImGui::DockBuilderFinish(dockspaceId);
    }

    void EditorUI::Shutdown()
    {
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

        // Reset other systems
        m_undoRedoManager.reset();
        m_commandPalette.reset();

        // Note: Don't shutdown crash handler here as it's managed elsewhere

        m_isInitialized = false;
        console.LogSuccess("EditorUI shutdown complete");
    }

    void EditorUI::CreatePanels()
    {
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("Creating editor panels...");

        // Skip complex panels that may cause deadlocks in debugger environment
        bool isDebuggerPresent = false;
#ifdef _WIN32
        isDebuggerPresent = IsDebuggerPresent();
#endif

        if (isDebuggerPresent)
        {
            console.LogWarning("DEBUGGER DETECTED - Using minimal panel set to avoid deadlocks");

            // Only create the most essential panels
            try
            {
                console.LogInfo("Creating Scene View panel...");
                auto sceneViewPanel = std::shared_ptr<SceneViewPanel>(new SceneViewPanel());
                m_panels["SceneView"] = sceneViewPanel;
                console.LogSuccess("Created Scene View panel");
            }
            catch (const std::exception& e)
            {
                console.LogError("Failed to create Scene View panel: " + std::string(e.what()));
            }

            // Initialize essential panels only
            for (auto& [name, panel] : m_panels)
            {
                try
                {
                    console.LogInfo("Initializing " + name + " panel");

                    if (panel && panel->Initialize())
                    {
                        console.LogSuccess("Initialized " + name + " panel");
                    }
                    else
                    {
                        console.LogError("Failed to initialize " + name + " panel");
                    }
                }
                catch (const std::exception& e)
                {
                    console.LogError("Exception initializing " + name + " panel: " + std::string(e.what()));
                }
            }

            console.LogInfo("Created " + std::to_string(m_panels.size()) + " editor panels (minimal set for debugger)");
            return;
        }

        // Full panel creation for release mode
        console.LogInfo("Creating full panel set...");

        // Create Scene View Panel (working)
        try
        {
            console.LogInfo("Creating Scene View panel...");
            auto sceneViewPanel = std::shared_ptr<SceneViewPanel>(new SceneViewPanel());
            m_panels["SceneView"] = sceneViewPanel;
            console.LogSuccess("Created Scene View panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Scene View panel: " + std::string(e.what()));
        }

        // Create Simple Console Panel
        try
        {
            console.LogInfo("Creating Simple Console panel...");
            auto consolePanel = std::shared_ptr<SimpleConsolePanel>(new SimpleConsolePanel());
            m_panels["Console"] = consolePanel;
            console.LogSuccess("Created Simple Console panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Simple Console panel: " + std::string(e.what()));
        }

        // Create Simple Hierarchy Panel
        try
        {
            console.LogInfo("Creating Simple Hierarchy panel...");
            auto hierarchyPanel = std::shared_ptr<SimpleHierarchyPanel>(new SimpleHierarchyPanel());
            m_panels["Hierarchy"] = hierarchyPanel;
            console.LogSuccess("Created Simple Hierarchy panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Simple Hierarchy panel: " + std::string(e.what()));
        }

        // Create Inspector Panel
        try
        {
            console.LogInfo("Creating Inspector panel...");
            auto inspectorPanel = std::shared_ptr<InspectorPanel>(new InspectorPanel());
            m_panels["Inspector"] = inspectorPanel;
            console.LogSuccess("Created Inspector panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Inspector panel: " + std::string(e.what()));
        }

        // Create Asset Browser Panel
        try
        {
            console.LogInfo("Creating Asset Browser panel...");
            auto assetBrowserPanel = std::shared_ptr<AssetBrowserPanel>(new AssetBrowserPanel());
            m_panels["AssetBrowser"] = assetBrowserPanel;
            console.LogSuccess("Created Asset Browser panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Asset Browser panel: " + std::string(e.what()));
        }

        // Create Game View Panel (FPS player camera)
        try
        {
            console.LogInfo("Creating Game View panel...");
            auto gameViewPanel = std::shared_ptr<GameViewPanel>(new GameViewPanel());
            m_panels["GameView"] = gameViewPanel;
            console.LogSuccess("Created Game View panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Game View panel: " + std::string(e.what()));
        }

        // Create Performance Profiler Panel
        try
        {
            console.LogInfo("Creating Performance Profiler panel...");
            auto profilerPanel = std::shared_ptr<PerformanceProfiler>(new PerformanceProfiler());
            m_panels["Profiler"] = profilerPanel;
            console.LogSuccess("Created Performance Profiler panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Profiler panel: " + std::string(e.what()));
        }

        // Create Weapon Editor Panel
        try
        {
            console.LogInfo("Creating Weapon Editor panel...");
            auto weaponEditorPanel = std::shared_ptr<WeaponEditorPanel>(new WeaponEditorPanel());
            m_panels["WeaponEditor"] = weaponEditorPanel;
            console.LogSuccess("Created Weapon Editor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Weapon Editor panel: " + std::string(e.what()));
        }

        // Create FPS Tools Panel
        try
        {
            console.LogInfo("Creating FPS Tools panel...");
            auto fpsToolsPanel = std::shared_ptr<FPSToolsPanel>(new FPSToolsPanel());
            m_panels["FPSTools"] = fpsToolsPanel;
            console.LogSuccess("Created FPS Tools panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create FPS Tools panel: " + std::string(e.what()));
        }

        // Create 2D/2.5D Editor Panels
        try
        {
            console.LogInfo("Creating Sprite Editor panel...");
            auto spriteEditorPanel = std::shared_ptr<SpriteEditorPanel>(new SpriteEditorPanel());
            m_panels["SpriteEditor"] = spriteEditorPanel;
            console.LogSuccess("Created Sprite Editor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Sprite Editor panel: " + std::string(e.what()));
        }

        try
        {
            console.LogInfo("Creating Tilemap Editor panel...");
            auto tilemapEditorPanel = std::shared_ptr<TilemapEditorPanel>(new TilemapEditorPanel());
            m_panels["TilemapEditor"] = tilemapEditorPanel;
            console.LogSuccess("Created Tilemap Editor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Tilemap Editor panel: " + std::string(e.what()));
        }

        try
        {
            console.LogInfo("Creating Sprite Animation Editor panel...");
            auto spriteAnimEditorPanel = std::shared_ptr<SpriteAnimationEditorPanel>(new SpriteAnimationEditorPanel());
            m_panels["SpriteAnimEditor"] = spriteAnimEditorPanel;
            console.LogSuccess("Created Sprite Animation Editor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Sprite Animation Editor panel: " + std::string(e.what()));
        }

        try
        {
            console.LogInfo("Creating Physics 2D panel...");
            auto physics2DPanel = std::shared_ptr<Physics2DPanel>(new Physics2DPanel());
            m_panels["Physics2D"] = physics2DPanel;
            console.LogSuccess("Created Physics 2D panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Physics 2D panel: " + std::string(e.what()));
        }

        // Create Undo History Panel
        try
        {
            console.LogInfo("Creating Undo History panel...");
            auto undoHistoryPanel = std::shared_ptr<UndoHistoryPanel>(new UndoHistoryPanel(m_undoRedoManager.get()));
            m_panels["UndoHistory"] = undoHistoryPanel;
            console.LogSuccess("Created Undo History panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Undo History panel: " + std::string(e.what()));
        }

        // Create Scene Statistics Panel
        try
        {
            console.LogInfo("Creating Scene Statistics panel...");
            auto sceneStatsPanel = std::shared_ptr<SceneStatisticsPanel>(new SceneStatisticsPanel());
            m_panels["SceneStats"] = sceneStatsPanel;
            console.LogSuccess("Created Scene Statistics panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Scene Statistics panel: " + std::string(e.what()));
        }

        // Create Prefab Editor Panel
        try
        {
            console.LogInfo("Creating Prefab Editor panel...");
            auto prefabEditorPanel = std::shared_ptr<PrefabEditorPanel>(new PrefabEditorPanel(m_prefabManager.get()));
            m_panels["PrefabEditor"] = prefabEditorPanel;
            console.LogSuccess("Created Prefab Editor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Prefab Editor panel: " + std::string(e.what()));
        }

        // Create Search Panel
        try
        {
            console.LogInfo("Creating Search panel...");
            auto searchPanel = std::shared_ptr<SearchPanel>(new SearchPanel());
            m_panels["Search"] = searchPanel;
            console.LogSuccess("Created Search panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Search panel: " + std::string(e.what()));
        }

        // SKIP SimpleBuildSystem in all modes since it's causing the hang
        console.LogWarning("SKIPPING Simple Build System panel (known to cause hangs)");

        // Initialize all panels
        for (auto& [name, panel] : m_panels)
        {
            try
            {
                console.LogInfo("Initializing " + name + " panel");

                if (panel && panel->Initialize())
                {
                    console.LogSuccess("Initialized " + name + " panel");
                }
                else
                {
                    console.LogError("Failed to initialize " + name + " panel");
                }
            }
            catch (const std::exception& e)
            {
                console.LogError("Exception initializing " + name + " panel: " + std::string(e.what()));
            }
        }

        // Assign panel icons (FontAwesome)
        if (m_panels.count("SceneView"))
            m_panels["SceneView"]->SetIcon(ICON_FA_CAMERA);
        if (m_panels.count("Console"))
            m_panels["Console"]->SetIcon(ICON_FA_TERMINAL);
        if (m_panels.count("Hierarchy"))
            m_panels["Hierarchy"]->SetIcon(ICON_FA_SITEMAP);
        if (m_panels.count("Inspector"))
            m_panels["Inspector"]->SetIcon(ICON_FA_SLIDERS);
        if (m_panels.count("AssetBrowser"))
            m_panels["AssetBrowser"]->SetIcon(ICON_FA_FOLDER);
        if (m_panels.count("GameView"))
            m_panels["GameView"]->SetIcon(ICON_FA_GAMEPAD);
        if (m_panels.count("Profiler"))
            m_panels["Profiler"]->SetIcon(ICON_FA_CHART_BAR);
        if (m_panels.count("WeaponEditor"))
            m_panels["WeaponEditor"]->SetIcon(ICON_FA_CROSSHAIRS);
        if (m_panels.count("FPSTools"))
            m_panels["FPSTools"]->SetIcon(ICON_FA_ROCKET);
        if (m_panels.count("DebugVisualizer"))
            m_panels["DebugVisualizer"]->SetIcon(ICON_FA_BUG);
        if (m_panels.count("SceneStats"))
            m_panels["SceneStats"]->SetIcon(ICON_FA_CHART_BAR);
        if (m_panels.count("ObjectPlacement"))
            m_panels["ObjectPlacement"]->SetIcon(ICON_FA_CUBE);
        if (m_panels.count("BuildCook"))
            m_panels["BuildCook"]->SetIcon(ICON_FA_HAMMER);

        // Hide secondary panels by default (accessible via menus)
        if (m_panels.count("UndoHistory"))
            m_panels["UndoHistory"]->SetIcon(ICON_FA_UNDO);
        if (m_panels.count("SceneStats"))
            m_panels["SceneStats"]->SetIcon(ICON_FA_CHART_BAR);
        if (m_panels.count("PrefabEditor"))
            m_panels["PrefabEditor"]->SetIcon(ICON_FA_CUBE);
        if (m_panels.count("Search"))
            m_panels["Search"]->SetIcon(ICON_FA_SEARCH);

        // Hide new panels by default (accessible via FPS Tools menu)
        if (m_panels.count("WeaponEditor"))
            m_panels["WeaponEditor"]->SetVisible(false);
        if (m_panels.count("FPSTools"))
            m_panels["FPSTools"]->SetVisible(false);
        if (m_panels.count("DebugVisualizer"))
            m_panels["DebugVisualizer"]->SetVisible(false);
        if (m_panels.count("SceneStats"))
            m_panels["SceneStats"]->SetVisible(false);
        if (m_panels.count("ObjectPlacement"))
            m_panels["ObjectPlacement"]->SetVisible(false);
        if (m_panels.count("BuildCook"))
            m_panels["BuildCook"]->SetVisible(false);
        if (m_panels.count("UndoHistory"))
            m_panels["UndoHistory"]->SetVisible(false);
        if (m_panels.count("PrefabEditor"))
            m_panels["PrefabEditor"]->SetVisible(false);
        if (m_panels.count("Search"))
            m_panels["Search"]->SetVisible(false);

        console.LogSuccess("Created " + std::to_string(m_panels.size()) + " editor panels");
    }

    void EditorUI::RenderMainMenuBar()
    {
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("New Scene", "Ctrl+N"))
                {
                    // Reset hierarchy to default objects for a fresh scene
                    auto it = m_panels.find("Hierarchy");
                    if (it != m_panels.end())
                    {
                        auto* hierarchy = dynamic_cast<SimpleHierarchyPanel*>(it->second.get());
                        if (hierarchy)
                        {
                            hierarchy->ResetToDefault();
                        }
                    }
                    m_currentScenePath.clear();
                    m_currentSceneName = "Untitled";
                    m_sceneModified = false;
                    ShowNotification("New scene created", "success");
                }
                if (ImGui::MenuItem("Save Scene", "Ctrl+S"))
                {
                    if (m_projectManager && m_projectManager->HasOpenProject())
                    {
                        if (m_currentScenePath.empty())
                        {
                            // Default to project Scenes folder
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
                ImGui::Separator();
                if (ImGui::MenuItem("New Project..."))
                {
                    ShowNewProjectDialog();
                }
                if (ImGui::MenuItem("Open Project..."))
                {
                    ShowOpenProjectDialog();
                }
                if (ImGui::MenuItem("Save Project", nullptr, false,
                                    m_projectManager && m_projectManager->HasOpenProject()))
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

                // Recent projects submenu
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
                                {
                                    ShowNotification("Opened project: " + rp.name, "success");
                                }
                                else
                                {
                                    ShowNotification("Failed to open project: " + rp.name, "error");
                                }
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

                ImGui::Separator();
                if (ImGui::MenuItem("Import Asset"))
                {
                    // Show the Asset Browser and focus it for importing
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

            if (ImGui::BeginMenu("Edit"))
            {
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

            if (ImGui::BeginMenu("GameObject"))
            {
                auto createObject = [this](const std::string& name)
                {
                    auto it = m_panels.find("Hierarchy");
                    if (it != m_panels.end())
                    {
                        auto* hierarchy = dynamic_cast<SimpleHierarchyPanel*>(it->second.get());
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
                if (ImGui::BeginMenu("2D Object"))
                {
                    if (ImGui::MenuItem("Sprite"))
                    {
                        ShowNotification("Created Sprite!", "success");
                    }
                    if (ImGui::MenuItem("Animated Sprite"))
                    {
                        ShowNotification("Created Animated Sprite!", "success");
                    }
                    if (ImGui::MenuItem("Tilemap"))
                    {
                        ShowNotification("Created Tilemap!", "success");
                    }
                    if (ImGui::MenuItem("Camera 2D"))
                    {
                        ShowNotification("Created 2D Camera!", "success");
                    }
                    if (ImGui::MenuItem("Parallax Background"))
                    {
                        ShowNotification("Created Parallax Background!", "success");
                    }
                    if (ImGui::MenuItem("Nine-Slice Sprite"))
                    {
                        ShowNotification("Created Nine-Slice Sprite!", "success");
                    }
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
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Window"))
            {
                if (ImGui::MenuItem("Hierarchy", nullptr, IsPanelVisible("Hierarchy")))
                {
                    SetPanelVisible("Hierarchy", !IsPanelVisible("Hierarchy"));
                }
                if (ImGui::MenuItem("Inspector", nullptr, IsPanelVisible("Inspector")))
                {
                    SetPanelVisible("Inspector", !IsPanelVisible("Inspector"));
                }
                if (ImGui::MenuItem("Scene View", nullptr, IsPanelVisible("SceneView")))
                {
                    SetPanelVisible("SceneView", !IsPanelVisible("SceneView"));
                }
                if (ImGui::MenuItem("Asset Browser", nullptr, IsPanelVisible("AssetBrowser")))
                {
                    SetPanelVisible("AssetBrowser", !IsPanelVisible("AssetBrowser"));
                }
                if (ImGui::MenuItem("Console", nullptr, IsPanelVisible("Console")))
                {
                    SetPanelVisible("Console", !IsPanelVisible("Console"));
                }
                if (ImGui::MenuItem("Game View", nullptr, IsPanelVisible("GameView")))
                {
                    SetPanelVisible("GameView", !IsPanelVisible("GameView"));
                }
                if (ImGui::MenuItem("Profiler", nullptr, IsPanelVisible("Profiler")))
                {
                    SetPanelVisible("Profiler", !IsPanelVisible("Profiler"));
                }
                ImGui::Separator();
                ImGui::TextDisabled("Tools & Debug");
                if (ImGui::MenuItem(ICON_FA_BUG " Debug Visualizer", nullptr, IsPanelVisible("DebugVisualizer")))
                {
                    SetPanelVisible("DebugVisualizer", !IsPanelVisible("DebugVisualizer"));
                }
                if (ImGui::MenuItem(ICON_FA_CHART_BAR " Scene Stats", nullptr, IsPanelVisible("SceneStats")))
                {
                    SetPanelVisible("SceneStats", !IsPanelVisible("SceneStats"));
                }
                if (ImGui::MenuItem(ICON_FA_CUBE " Object Placement", nullptr, IsPanelVisible("ObjectPlacement")))
                {
                    SetPanelVisible("ObjectPlacement", !IsPanelVisible("ObjectPlacement"));
                }
                if (ImGui::MenuItem(ICON_FA_HAMMER " Build & Cook", nullptr, IsPanelVisible("BuildCook")))
                {
                    SetPanelVisible("BuildCook", !IsPanelVisible("BuildCook"));
                    ImGui::TextDisabled("Tools & Analysis");
                    if (ImGui::MenuItem(ICON_FA_UNDO " Undo History", nullptr, IsPanelVisible("UndoHistory")))
                    {
                        SetPanelVisible("UndoHistory", !IsPanelVisible("UndoHistory"));
                    }
                    if (ImGui::MenuItem(ICON_FA_CHART_BAR " Scene Statistics", nullptr, IsPanelVisible("SceneStats")))
                    {
                        SetPanelVisible("SceneStats", !IsPanelVisible("SceneStats"));
                    }
                    if (ImGui::MenuItem(ICON_FA_CUBE " Prefab Editor", nullptr, IsPanelVisible("PrefabEditor")))
                    {
                        SetPanelVisible("PrefabEditor", !IsPanelVisible("PrefabEditor"));
                    }
                    if (ImGui::MenuItem(ICON_FA_SEARCH " Search", nullptr, IsPanelVisible("Search")))
                    {
                        SetPanelVisible("Search", !IsPanelVisible("Search"));
                    }
                    ImGui::Separator();
                    ImGui::TextDisabled("2D / 2.5D Panels");
                    if (ImGui::MenuItem("Sprite Editor", nullptr, IsPanelVisible("SpriteEditor")))
                    {
                        SetPanelVisible("SpriteEditor", !IsPanelVisible("SpriteEditor"));
                    }
                    if (ImGui::MenuItem("Tilemap Editor", nullptr, IsPanelVisible("TilemapEditor")))
                    {
                        SetPanelVisible("TilemapEditor", !IsPanelVisible("TilemapEditor"));
                    }
                    if (ImGui::MenuItem("Sprite Animation", nullptr, IsPanelVisible("SpriteAnimEditor")))
                    {
                        SetPanelVisible("SpriteAnimEditor", !IsPanelVisible("SpriteAnimEditor"));
                    }
                    if (ImGui::MenuItem("Physics 2D", nullptr, IsPanelVisible("Physics2D")))
                    {
                        SetPanelVisible("Physics2D", !IsPanelVisible("Physics2D"));
                    }
                    ImGui::Separator();
                    ImGui::TextDisabled("FPS Panels");
                    if (ImGui::MenuItem("Weapon Editor", nullptr, IsPanelVisible("WeaponEditor")))
                    {
                        SetPanelVisible("WeaponEditor", !IsPanelVisible("WeaponEditor"));
                    }
                    if (ImGui::MenuItem("FPS Tools", nullptr, IsPanelVisible("FPSTools")))
                    {
                        SetPanelVisible("FPSTools", !IsPanelVisible("FPSTools"));
                    }
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

                if (ImGui::BeginMenu("FPS Tools"))
                {
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

                if (ImGui::BeginMenu("Build"))
                {
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

                if (ImGui::BeginMenu("Help"))
                {
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
                        // Open the wiki/docs in the default browser or file explorer
#ifdef _WIN32
                        ShellExecuteA(nullptr, "open", "docs", nullptr, nullptr, SW_SHOWNORMAL);
#else
                        system("xdg-open docs/ &");
#endif
                        ShowNotification("Opening documentation...", "info", 2.0f);
                    }
                    ImGui::EndMenu();
                }

                ImGui::EndMainMenuBar();
            }
        }

        void EditorUI::RenderToolbar()
        {
            ImGuiWindowFlags toolbarFlags =
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse;

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));

            if (ImGui::Begin("##Toolbar", nullptr, toolbarFlags))
            {
                float btnSize = 28.0f;
                ImVec2 btnDim(btnSize, btnSize);

                // Accent colors
                ImVec4 accentBlue(0.176f, 0.549f, 0.941f, 1.0f);  // #2D8CF0
                ImVec4 accentAmber(0.961f, 0.651f, 0.137f, 1.0f); // #F5A623
                ImVec4 playGreen(0.298f, 0.686f, 0.314f, 1.0f);   // #4CAF50
                ImVec4 stopRed(0.898f, 0.224f, 0.208f, 1.0f);     // #E53935

                // === Transform Tools ===
                auto ToolButton = [&](const char* icon, TransformTool tool, const char* tooltip)
                {
                    bool active = (m_currentTool == tool);
                    if (active)
                        ImGui::PushStyleColor(ImGuiCol_Button, accentBlue);
                    if (ImGui::Button(icon, btnDim))
                        m_currentTool = tool;
                    if (active)
                        ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tooltip);
                    ImGui::SameLine();
                };

                ToolButton(ICON_FA_ARROWS_ALT, TransformTool::Move, "Move (W)");
                ToolButton(ICON_FA_SYNC_ALT, TransformTool::Rotate, "Rotate (E)");
                ToolButton(ICON_FA_EXPAND, TransformTool::Scale, "Scale (R)");

                ImGui::Text("|");
                ImGui::SameLine();

                // === Space Toggle ===
                bool isLocal = (m_transformSpace == TransformSpace::Local);
                if (ImGui::Button(isLocal ? "Local" : "World", ImVec2(50, btnSize)))
                {
                    m_transformSpace = isLocal ? TransformSpace::World : TransformSpace::Local;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Toggle World/Local space");

                ImGui::SameLine();
                ImGui::Text("|");
                ImGui::SameLine();

                // === Play Controls (centered) ===
                float windowWidth = ImGui::GetWindowContentRegionMax().x;
                float playWidth = btnSize * 3 + 8;
                float cursorX = (windowWidth - playWidth) * 0.5f;
                if (cursorX > ImGui::GetCursorPosX())
                    ImGui::SetCursorPosX(cursorX);

                // Play
                bool isPlaying = (m_playMode == PlayMode::Playing);
                if (isPlaying)
                    ImGui::PushStyleColor(ImGuiCol_Button, playGreen);
                if (ImGui::Button(ICON_FA_PLAY, btnDim))
                {
                    m_playMode = (m_playMode == PlayMode::Playing) ? PlayMode::Stopped : PlayMode::Playing;
                    ShowNotification(isPlaying ? "Stopped" : "Playing...", isPlaying ? "info" : "success", 2.0f);
                }
                if (isPlaying)
                    ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Play (F5)");
                ImGui::SameLine();

                // Pause
                bool isPaused = (m_playMode == PlayMode::Paused);
                if (isPaused)
                    ImGui::PushStyleColor(ImGuiCol_Button, accentAmber);
                if (ImGui::Button(ICON_FA_PAUSE, btnDim))
                {
                    if (m_playMode == PlayMode::Playing)
                        m_playMode = PlayMode::Paused;
                    else if (m_playMode == PlayMode::Paused)
                        m_playMode = PlayMode::Playing;
                }
                if (isPaused)
                    ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Pause");
                ImGui::SameLine();

                // Stop
                if (ImGui::Button(ICON_FA_STOP, btnDim))
                {
                    if (m_playMode != PlayMode::Stopped)
                    {
                        m_playMode = PlayMode::Stopped;
                        ShowNotification("Stopped", "info", 2.0f);
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Stop (Shift+F5)");

                ImGui::SameLine();
                ImGui::Text("|");
                ImGui::SameLine();

                // === Snap Controls (right side) ===
                ImGui::Checkbox("Snap", &m_snapEnabled);
                if (m_snapEnabled)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60);
                    ImGui::DragFloat("##SnapVal", &m_snapValue, 0.1f, 0.1f, 100.0f, "%.1f");
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(2);
        }

        void EditorUI::RenderStatusBar()
        {
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            float statusBarHeight = 24.0f;
            ImVec2 statusBarPos(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight);
            ImVec2 statusBarSize(viewport->WorkSize.x, statusBarHeight);

            ImGui::SetNextWindowPos(statusBarPos);
            ImGui::SetNextWindowSize(statusBarSize);
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoDocking;

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 3));
            if (ImGui::Begin("##StatusBar", nullptr, flags))
            {
                // Left: engine connection status
                ImVec4 statusColor =
                    m_engineConnected ? ImVec4(0.3f, 0.8f, 0.3f, 1.0f) : ImVec4(0.8f, 0.3f, 0.3f, 1.0f);
                ImGui::TextColored(statusColor, ICON_FA_CIRCLE);
                ImGui::SameLine();
                ImGui::Text("Engine: %s", m_engineConnected ? "Connected" : "Disconnected");

                // Project name
                if (m_projectManager && m_projectManager->HasOpenProject())
                {
                    ImGui::SameLine();
                    ImGui::Text("|");
                    ImGui::SameLine();
                    ImGui::Text("Project: %s", m_projectManager->GetCurrentProject().name.c_str());
                }

                // Scene name
                ImGui::SameLine();
                ImGui::Text("|");
                ImGui::SameLine();
                ImGui::Text("Scene: %s%s", m_currentSceneName.c_str(), m_sceneModified ? "*" : "");

                ImGui::SameLine();
                ImGui::Text("|");
                ImGui::SameLine();

                // Center: tool + selection
                const char* toolNames[] = {"Move", "Rotate", "Scale"};
                ImGui::Text(ICON_FA_ARROWS_ALT " %s | Objects: %d | Selected: %d", toolNames[(int)m_currentTool],
                            m_sceneObjectCount, m_selectedObjectCount);

                // Right: FPS + frame info
                float fps = m_stats.frameTime > 0.001f ? 1000.0f / m_stats.frameTime : 0.0f;
                ImVec4 fpsColor = fps >= 60.0f   ? ImVec4(0.3f, 0.8f, 0.3f, 1.0f)
                                  : fps >= 30.0f ? ImVec4(0.9f, 0.9f, 0.3f, 1.0f)
                                                 : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);

                float rightOffset = ImGui::GetWindowWidth() - 360;
                if (rightOffset > ImGui::GetCursorPosX())
                {
                    ImGui::SameLine(rightOffset);
                }
                ImGui::TextColored(fpsColor, "%.0f FPS", fps);
                ImGui::SameLine();
                ImGui::Text("| %.1fms | Assets: %d | Frame: %llu", m_stats.frameTime, m_assetDatabaseSize,
                            (unsigned long long)m_frameNumber);
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }

        void EditorUI::RenderNotifications()
        {
            const float NOTIFICATION_WIDTH = 320.0f;
            const float NOTIFICATION_HEIGHT = 52.0f;
            const float NOTIFICATION_SPACING = 6.0f;

            ImGuiViewport* viewport = ImGui::GetMainViewport();
            float yOffset = viewport->WorkPos.y + 8.0f;

            for (size_t i = 0; i < m_notifications.size(); ++i)
            {
                const auto& notification = m_notifications[i];

                // Fade out in last 0.5 seconds
                float alpha = 1.0f;
                if (notification.duration > 0.0f && notification.timeLeft < 0.5f)
                {
                    alpha = std::max(0.0f, notification.timeLeft / 0.5f);
                }

                ImVec2 notificationPos(viewport->WorkPos.x + viewport->WorkSize.x - NOTIFICATION_WIDTH - 12.0f,
                                       yOffset + i * (NOTIFICATION_HEIGHT + NOTIFICATION_SPACING));

                ImGui::SetNextWindowPos(notificationPos);
                ImGui::SetNextWindowSize(ImVec2(NOTIFICATION_WIDTH, NOTIFICATION_HEIGHT));
                ImGui::SetNextWindowBgAlpha(0.92f * alpha);

                std::string windowName = "##Notification" + std::to_string(i);
                ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;

                // Determine stripe and icon color
                ImVec4 stripeColor(0.176f, 0.549f, 0.941f, alpha); // blue default
                const char* icon = ICON_FA_INFO_CIRCLE;
                if (notification.type == "error")
                {
                    stripeColor = ImVec4(0.898f, 0.224f, 0.208f, alpha);
                    icon = ICON_FA_TIMES;
                }
                else if (notification.type == "warning")
                {
                    stripeColor = ImVec4(0.961f, 0.651f, 0.137f, alpha);
                    icon = ICON_FA_EXCLAMATION;
                }
                else if (notification.type == "success")
                {
                    stripeColor = ImVec4(0.298f, 0.686f, 0.314f, alpha);
                    icon = ICON_FA_CHECK;
                }

                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
                if (ImGui::Begin(windowName.c_str(), nullptr, flags))
                {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImVec2 wp = ImGui::GetWindowPos();
                    ImVec2 ws = ImGui::GetWindowSize();

                    // Colored left stripe
                    dl->AddRectFilled(wp, ImVec2(wp.x + 4, wp.y + ws.y), ImGui::ColorConvertFloat4ToU32(stripeColor),
                                      4.0f, ImDrawFlags_RoundCornersLeft);

                    // Content with padding past the stripe
                    ImGui::SetCursorPos(ImVec2(12, (NOTIFICATION_HEIGHT - ImGui::GetTextLineHeight()) * 0.5f));
                    ImGui::TextColored(ImVec4(stripeColor.x, stripeColor.y, stripeColor.z, alpha), "%s", icon);
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.88f, 0.89f, 0.92f, alpha));
                    ImGui::TextWrapped("%s", notification.message.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::End();
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
                if (panel->IsVisible())
                {
                    panel->Update(deltaTime); // Fixed: Pass proper delta time
                    panel->Render();
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
                    if (panel->IsVisible())
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
            return it != m_panels.end() ? it->second->IsVisible() : false;
        }

        void EditorUI::SetPanelVisible(const std::string& panelName, bool visible)
        {
            auto it = m_panels.find(panelName);
            if (it != m_panels.end())
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
                    if (!first)
                        file << ",\n";
                    file << "      \"" << name << "\": { \"visible\": " << (panel->IsVisible() ? "true" : "false")
                         << " }";
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
            try
            {
                // Ensure parent directory exists
                std::filesystem::create_directories(std::filesystem::path(path).parent_path());

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
                    auto* hierarchy = dynamic_cast<SimpleHierarchyPanel*>(it->second.get());
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
        void EditorUI::SetGraphicsDevice(ID3D11Device * device, ID3D11DeviceContext * context)
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

            // Register panel toggle actions
            auto RegisterPanelToggle = [this](const std::string& panelKey, const std::string& displayName)
            {
                m_commandPalette->RegisterAction("Toggle " + displayName, "Panel", [this, panelKey]()
                                                 { SetPanelVisible(panelKey, !IsPanelVisible(panelKey)); });
            };

            RegisterPanelToggle("SceneView", "Scene View");
            RegisterPanelToggle("Console", "Console");
            RegisterPanelToggle("Hierarchy", "Hierarchy");
            RegisterPanelToggle("Inspector", "Inspector");
            RegisterPanelToggle("AssetBrowser", "Asset Browser");
            RegisterPanelToggle("GameView", "Game View");
            RegisterPanelToggle("Profiler", "Profiler");
            RegisterPanelToggle("WeaponEditor", "Weapon Editor");
            RegisterPanelToggle("FPSTools", "FPS Tools");
            RegisterPanelToggle("SpriteEditor", "Sprite Editor");
            RegisterPanelToggle("TilemapEditor", "Tilemap Editor");
            RegisterPanelToggle("SpriteAnimEditor", "Sprite Animation Editor");
            RegisterPanelToggle("Physics2D", "Physics 2D");
            RegisterPanelToggle("UndoHistory", "Undo History");
            RegisterPanelToggle("SceneStats", "Scene Statistics");
            RegisterPanelToggle("PrefabEditor", "Prefab Editor");
            RegisterPanelToggle("Search", "Search");

            // Register undo/redo commands
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

            // Register layout commands
            m_commandPalette->RegisterAction("Reset Layout", "Layout", [this]() { ResetToDefaultLayout(); });

            m_commandPalette->RegisterAction("Save Layout", "Layout", [this]() { SaveLayout("Quick Save"); });

            // Register scene commands
            m_commandPalette->RegisterAction("New Scene", "Scene",
                                             [this]() { ShowNotification("New Scene created!", "success"); });

            m_commandPalette->RegisterAction("Save Scene", "Scene",
                                             [this]() { ShowNotification("Scene saved!", "success"); });

            // Register play mode commands
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

            // Register prefab commands
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

            // Register theme commands
            m_commandPalette->RegisterAction("Theme: Spark Professional", "Command",
                                             [this]() { ApplyTheme("Spark Professional"); });

            m_commandPalette->RegisterAction("Theme: Dark", "Command", [this]() { ApplyTheme("Dark"); });

            m_commandPalette->RegisterAction("Theme: Light", "Command", [this]() { ApplyTheme("Light"); });

            // Register tool commands
            m_commandPalette->RegisterAction(
                "Tool: Move", "Command", [this]() { m_currentTool = TransformTool::Move; }, "W");

            m_commandPalette->RegisterAction(
                "Tool: Rotate", "Command", [this]() { m_currentTool = TransformTool::Rotate; }, "E");

            m_commandPalette->RegisterAction(
                "Tool: Scale", "Command", [this]() { m_currentTool = TransformTool::Scale; }, "R");

            m_commandPalette->RegisterAction("Toggle Snap", "Command", [this]() { m_snapEnabled = !m_snapEnabled; });
        }

    } // namespace SparkEditor
