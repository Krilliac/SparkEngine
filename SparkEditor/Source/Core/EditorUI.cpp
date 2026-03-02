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
#endif

namespace SparkEditor {

EditorUI::EditorUI() {
    m_crashHandler = &EditorCrashHandler::GetInstance();
}

EditorUI::~EditorUI() {
    if (m_isInitialized) {
        Shutdown();
    }
}

bool EditorUI::Initialize(const EditorConfig& config) {
    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("Initializing Enhanced EditorUI with full configuration...");
    
    try {
        // Store config
        m_config = std::unique_ptr<EditorConfig>(new EditorConfig(config));
        
        console.LogInfo("Using enhanced initialization for production use");
        
        // Initialize crash handler
        console.LogInfo("Initializing crash handler...");
        if (m_crashHandler && m_crashHandler->Initialize()) {
            console.LogSuccess("Crash handler initialized successfully");
        } else {
            console.LogWarning("Crash handler initialization failed");
        }
        
        // Create panels
        console.LogInfo("Creating editor panels...");
        CreatePanels();
        console.LogSuccess("Panels created successfully");

        // Apply the Spark Professional theme
        console.LogInfo("Applying Spark Professional theme...");
        ApplyTheme("Spark Professional");
        console.LogSuccess("Theme applied");

        m_isInitialized = true;
        console.LogSuccess("Enhanced EditorUI initialized successfully");
        return true;
        
    } catch (const std::exception& e) {
        console.LogError("Exception in EditorUI::Initialize: " + std::string(e.what()));
        return false;
    } catch (...) {
        console.LogError("Unknown exception in EditorUI::Initialize");
        return false;
    }
}

void EditorUI::Update(float deltaTime) {
    if (!m_isInitialized) return;
    
    // Update notifications
    auto it = m_notifications.begin();
    while (it != m_notifications.end()) {
        it->timeLeft -= deltaTime;
        if (it->timeLeft <= 0.0f && it->duration > 0.0f) {
            it = m_notifications.erase(it);
        } else {
            ++it;
        }
    }
    
    // Update stats
    UpdateStats(deltaTime);
}

void EditorUI::Render() {
    if (!m_isInitialized) return;

    // === Full-screen DockSpace ===
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags dockspaceFlags =
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking |
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
    if (m_firstFrame) {
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

    if (m_showDemoWindow) {
        ImGui::ShowDemoWindow(&m_showDemoWindow);
    }
}

void EditorUI::SetupDefaultDockLayout(ImGuiID dockspaceId) {
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

void EditorUI::Shutdown() {
    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("Shutting down EditorUI...");
    
    // Shutdown panels using vector iteration since unordered_map doesn't have rbegin/rend
    std::vector<std::pair<std::string, std::shared_ptr<EditorPanel>>> panelVector(m_panels.begin(), m_panels.end());
    for (auto it = panelVector.rbegin(); it != panelVector.rend(); ++it) {
        try {
            if (it->second) {
                console.LogInfo("Shutting down " + it->first + " panel");
                it->second->Shutdown();
                console.LogSuccess(it->first + " panel shutdown complete");
            }
        } catch (const std::exception& e) {
            console.LogError("Exception shutting down " + it->first + " panel: " + std::string(e.what()));
        }
    }
    m_panels.clear();
    console.LogInfo("All panels shutdown and cleared");
    
    // Note: Don't shutdown crash handler here as it's managed elsewhere
    
    m_isInitialized = false;
    console.LogSuccess("EditorUI shutdown complete");
}

void EditorUI::CreatePanels() {
    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("Creating editor panels...");
    
    // Skip complex panels that may cause deadlocks in debugger environment
    bool isDebuggerPresent = false;
#ifdef _WIN32
    isDebuggerPresent = IsDebuggerPresent();
#endif

    if (isDebuggerPresent) {
        console.LogWarning("DEBUGGER DETECTED - Using minimal panel set to avoid deadlocks");
        
        // Only create the most essential panels
        try {
            console.LogInfo("Creating Scene View panel...");
            auto sceneViewPanel = std::shared_ptr<SceneViewPanel>(new SceneViewPanel());
            m_panels["SceneView"] = sceneViewPanel;
            console.LogSuccess("Created Scene View panel");
        } catch (const std::exception& e) {
            console.LogError("Failed to create Scene View panel: " + std::string(e.what()));
        }
        
        // Initialize essential panels only
        for (auto& [name, panel] : m_panels) {
            try {
                console.LogInfo("Initializing " + name + " panel");
                
                if (panel && panel->Initialize()) {
                    console.LogSuccess("Initialized " + name + " panel");
                } else {
                    console.LogError("Failed to initialize " + name + " panel");
                }
            } catch (const std::exception& e) {
                console.LogError("Exception initializing " + name + " panel: " + std::string(e.what()));
            }
        }
        
        console.LogInfo("Created " + std::to_string(m_panels.size()) + " editor panels (minimal set for debugger)");
        return;
    }
    
    // Full panel creation for release mode
    console.LogInfo("Creating full panel set...");
    
    // Create Scene View Panel (working)
    try {
        console.LogInfo("Creating Scene View panel...");
        auto sceneViewPanel = std::shared_ptr<SceneViewPanel>(new SceneViewPanel());
        m_panels["SceneView"] = sceneViewPanel;
        console.LogSuccess("Created Scene View panel");
    } catch (const std::exception& e) {
        console.LogError("Failed to create Scene View panel: " + std::string(e.what()));
    }
    
    // Create Simple Console Panel
    try {
        console.LogInfo("Creating Simple Console panel...");
        auto consolePanel = std::shared_ptr<SimpleConsolePanel>(new SimpleConsolePanel());
        m_panels["Console"] = consolePanel;
        console.LogSuccess("Created Simple Console panel");
    } catch (const std::exception& e) {
        console.LogError("Failed to create Simple Console panel: " + std::string(e.what()));
    }
    
    // Create Simple Hierarchy Panel
    try {
        console.LogInfo("Creating Simple Hierarchy panel...");
        auto hierarchyPanel = std::shared_ptr<SimpleHierarchyPanel>(new SimpleHierarchyPanel());
        m_panels["Hierarchy"] = hierarchyPanel;
        console.LogSuccess("Created Simple Hierarchy panel");
    } catch (const std::exception& e) {
        console.LogError("Failed to create Simple Hierarchy panel: " + std::string(e.what()));
    }
    
    // Create Inspector Panel
    try {
        console.LogInfo("Creating Inspector panel...");
        auto inspectorPanel = std::shared_ptr<InspectorPanel>(new InspectorPanel());
        m_panels["Inspector"] = inspectorPanel;
        console.LogSuccess("Created Inspector panel");
    } catch (const std::exception& e) {
        console.LogError("Failed to create Inspector panel: " + std::string(e.what()));
    }
    
    // Create Asset Browser Panel
    try {
        console.LogInfo("Creating Asset Browser panel...");
        auto assetBrowserPanel = std::shared_ptr<AssetBrowserPanel>(new AssetBrowserPanel());
        m_panels["AssetBrowser"] = assetBrowserPanel;
        console.LogSuccess("Created Asset Browser panel");
    } catch (const std::exception& e) {
        console.LogError("Failed to create Asset Browser panel: " + std::string(e.what()));
    }
    
    // Create Game View Panel (FPS player camera)
    try {
        console.LogInfo("Creating Game View panel...");
        auto gameViewPanel = std::shared_ptr<GameViewPanel>(new GameViewPanel());
        m_panels["GameView"] = gameViewPanel;
        console.LogSuccess("Created Game View panel");
    } catch (const std::exception& e) {
        console.LogError("Failed to create Game View panel: " + std::string(e.what()));
    }

    // Create Performance Profiler Panel
    try {
        console.LogInfo("Creating Performance Profiler panel...");
        auto profilerPanel = std::shared_ptr<PerformanceProfiler>(new PerformanceProfiler());
        m_panels["Profiler"] = profilerPanel;
        console.LogSuccess("Created Performance Profiler panel");
    } catch (const std::exception& e) {
        console.LogError("Failed to create Profiler panel: " + std::string(e.what()));
    }

    // SKIP SimpleBuildSystem in all modes since it's causing the hang
    console.LogWarning("SKIPPING Simple Build System panel (known to cause hangs)");

    // Initialize all panels
    for (auto& [name, panel] : m_panels) {
        try {
            console.LogInfo("Initializing " + name + " panel");
            
            if (panel && panel->Initialize()) {
                console.LogSuccess("Initialized " + name + " panel");
            } else {
                console.LogError("Failed to initialize " + name + " panel");
            }
        } catch (const std::exception& e) {
            console.LogError("Exception initializing " + name + " panel: " + std::string(e.what()));
        }
    }
    
    // Assign panel icons (FontAwesome)
    if (m_panels.count("SceneView"))   m_panels["SceneView"]->SetIcon(ICON_FA_CAMERA);
    if (m_panels.count("Console"))     m_panels["Console"]->SetIcon(ICON_FA_TERMINAL);
    if (m_panels.count("Hierarchy"))   m_panels["Hierarchy"]->SetIcon(ICON_FA_SITEMAP);
    if (m_panels.count("Inspector"))   m_panels["Inspector"]->SetIcon(ICON_FA_SLIDERS);
    if (m_panels.count("AssetBrowser")) m_panels["AssetBrowser"]->SetIcon(ICON_FA_FOLDER);
    if (m_panels.count("GameView"))    m_panels["GameView"]->SetIcon(ICON_FA_GAMEPAD);
    if (m_panels.count("Profiler"))    m_panels["Profiler"]->SetIcon(ICON_FA_CHART_BAR);

    console.LogSuccess("Created " + std::to_string(m_panels.size()) + " editor panels");
}

void EditorUI::RenderMainMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Scene")) {
                ShowNotification("New Scene created!", "success");
            }
            if (ImGui::MenuItem("Open Scene")) {
                ShowNotification("Open Scene dialog coming soon!", "info");
            }
            if (ImGui::MenuItem("Save Scene")) {
                ShowNotification("Scene saved!", "success");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("New Project")) {
                ShowNotification("New Project feature coming soon!", "info");
            }
            if (ImGui::MenuItem("Open Project")) {
                ShowNotification("Open Project feature coming soon!", "info");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Import Asset")) {
                ShowNotification("Import Asset feature coming soon!", "info");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Build Settings")) {
                ShowNotification("Build Settings coming soon!", "info");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                ShowNotification("Exit feature coming soon!", "info");
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) {
                ShowNotification("Undo operation!", "info");
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y")) {
                ShowNotification("Redo operation!", "info");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", "Ctrl+X")) {
                ShowNotification("Cut operation!", "info");
            }
            if (ImGui::MenuItem("Copy", "Ctrl+C")) {
                ShowNotification("Copy operation!", "info");
            }
            if (ImGui::MenuItem("Paste", "Ctrl+V")) {
                ShowNotification("Paste operation!", "info");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Select All", "Ctrl+A")) {
                ShowNotification("Select All operation!", "info");
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("GameObject")) {
            if (ImGui::MenuItem("Create Empty")) {
                ShowNotification("Created empty GameObject!", "success");
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("3D Object")) {
                if (ImGui::MenuItem("Cube")) {
                    ShowNotification("Created Cube!", "success");
                }
                if (ImGui::MenuItem("Sphere")) {
                    ShowNotification("Created Sphere!", "success");
                }
                if (ImGui::MenuItem("Cylinder")) {
                    ShowNotification("Created Cylinder!", "success");
                }
                if (ImGui::MenuItem("Plane")) {
                    ShowNotification("Created Plane!", "success");
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Light")) {
                if (ImGui::MenuItem("Directional Light")) {
                    ShowNotification("Created Directional Light!", "success");
                }
                if (ImGui::MenuItem("Point Light")) {
                    ShowNotification("Created Point Light!", "success");
                }
                if (ImGui::MenuItem("Spot Light")) {
                    ShowNotification("Created Spot Light!", "success");
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Camera")) {
                if (ImGui::MenuItem("Camera")) {
                    ShowNotification("Created Camera!", "success");
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Window")) {
            if (ImGui::MenuItem("Hierarchy", nullptr, IsPanelVisible("Hierarchy"))) {
                SetPanelVisible("Hierarchy", !IsPanelVisible("Hierarchy"));
            }
            if (ImGui::MenuItem("Inspector", nullptr, IsPanelVisible("Inspector"))) {
                SetPanelVisible("Inspector", !IsPanelVisible("Inspector"));
            }
            if (ImGui::MenuItem("Scene View", nullptr, IsPanelVisible("SceneView"))) {
                SetPanelVisible("SceneView", !IsPanelVisible("SceneView"));
            }
            if (ImGui::MenuItem("Asset Browser", nullptr, IsPanelVisible("AssetBrowser"))) {
                SetPanelVisible("AssetBrowser", !IsPanelVisible("AssetBrowser"));
            }
            if (ImGui::MenuItem("Console", nullptr, IsPanelVisible("Console"))) {
                SetPanelVisible("Console", !IsPanelVisible("Console"));
            }
            if (ImGui::MenuItem("Game View", nullptr, IsPanelVisible("GameView"))) {
                SetPanelVisible("GameView", !IsPanelVisible("GameView"));
            }
            if (ImGui::MenuItem("Profiler", nullptr, IsPanelVisible("Profiler"))) {
                SetPanelVisible("Profiler", !IsPanelVisible("Profiler"));
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Layout")) {
                ResetToDefaultLayout();
                ShowNotification("Layout reset!", "success");
            }
            if (ImGui::MenuItem("Save Layout")) {
                SaveLayout("Custom Layout");
                ShowNotification("Layout saved!", "success");
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("FPS Tools")) {
            if (ImGui::MenuItem(ICON_FA_CROSSHAIRS " Weapon Editor")) {
                ShowNotification("Weapon Editor coming soon!", "info");
            }
            if (ImGui::MenuItem(ICON_FA_FLAG " Spawn Points")) {
                ShowNotification("Spawn Point editor coming soon!", "info");
            }
            if (ImGui::MenuItem(ICON_FA_BULLSEYE " Objectives")) {
                ShowNotification("Objective editor coming soon!", "info");
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_BOMB " Explosive Volumes")) {
                ShowNotification("Explosive volumes editor coming soon!", "info");
            }
            if (ImGui::MenuItem(ICON_FA_SHIELD " Cover Points")) {
                ShowNotification("Cover point editor coming soon!", "info");
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Build")) {
            if (ImGui::MenuItem(ICON_FA_LIGHTBULB " Build Lighting")) {
                ShowNotification("Build Lighting started...", "info");
            }
            if (ImGui::MenuItem(ICON_FA_MAP " Build NavMesh")) {
                ShowNotification("Build NavMesh started...", "info");
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_HAMMER " Build All")) {
                ShowNotification("Build All started...", "info");
            }
            if (ImGui::MenuItem(ICON_FA_ROCKET " Deploy")) {
                ShowNotification("Deploy pipeline coming soon!", "info");
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Show Demo Window", nullptr, m_showDemoWindow)) {
                m_showDemoWindow = !m_showDemoWindow;
            }
            if (ImGui::BeginMenu("Themes")) {
                auto themes = EditorTheme::GetAvailableThemes();
                for (const auto& name : themes) {
                    bool isSelected = (m_currentTheme == name);
                    if (ImGui::MenuItem(name.c_str(), nullptr, isSelected)) {
                        ApplyTheme(name);
                        ShowNotification("Theme: " + name, "success", 2.0f);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("About")) {
                ShowNotification(ICON_FA_BOLT " Spark Engine Editor v1.0 — FPS Game Engine", "info", 5.0f);
            }
            if (ImGui::MenuItem("Documentation")) {
                ShowNotification("Documentation coming soon!", "info");
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

void EditorUI::RenderToolbar() {
    ImGuiWindowFlags toolbarFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));

    if (ImGui::Begin("##Toolbar", nullptr, toolbarFlags)) {
        float btnSize = 28.0f;
        ImVec2 btnDim(btnSize, btnSize);

        // Accent colors
        ImVec4 accentBlue(0.176f, 0.549f, 0.941f, 1.0f);   // #2D8CF0
        ImVec4 accentAmber(0.961f, 0.651f, 0.137f, 1.0f);  // #F5A623
        ImVec4 playGreen(0.298f, 0.686f, 0.314f, 1.0f);    // #4CAF50
        ImVec4 stopRed(0.898f, 0.224f, 0.208f, 1.0f);      // #E53935

        // === Transform Tools ===
        auto ToolButton = [&](const char* icon, TransformTool tool, const char* tooltip) {
            bool active = (m_currentTool == tool);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, accentBlue);
            if (ImGui::Button(icon, btnDim)) m_currentTool = tool;
            if (active) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
            ImGui::SameLine();
        };

        ToolButton(ICON_FA_ARROWS_ALT, TransformTool::Move, "Move (W)");
        ToolButton(ICON_FA_SYNC_ALT, TransformTool::Rotate, "Rotate (E)");
        ToolButton(ICON_FA_EXPAND, TransformTool::Scale, "Scale (R)");

        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // === Space Toggle ===
        bool isLocal = (m_transformSpace == TransformSpace::Local);
        if (ImGui::Button(isLocal ? "Local" : "World", ImVec2(50, btnSize))) {
            m_transformSpace = isLocal ? TransformSpace::World : TransformSpace::Local;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle World/Local space");

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // === Play Controls (centered) ===
        float windowWidth = ImGui::GetWindowContentRegionMax().x;
        float playWidth = btnSize * 3 + 8;
        float cursorX = (windowWidth - playWidth) * 0.5f;
        if (cursorX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(cursorX);

        // Play
        bool isPlaying = (m_playMode == PlayMode::Playing);
        if (isPlaying) ImGui::PushStyleColor(ImGuiCol_Button, playGreen);
        if (ImGui::Button(ICON_FA_PLAY, btnDim)) {
            m_playMode = (m_playMode == PlayMode::Playing) ? PlayMode::Stopped : PlayMode::Playing;
            ShowNotification(isPlaying ? "Stopped" : "Playing...", isPlaying ? "info" : "success", 2.0f);
        }
        if (isPlaying) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play (F5)");
        ImGui::SameLine();

        // Pause
        bool isPaused = (m_playMode == PlayMode::Paused);
        if (isPaused) ImGui::PushStyleColor(ImGuiCol_Button, accentAmber);
        if (ImGui::Button(ICON_FA_PAUSE, btnDim)) {
            if (m_playMode == PlayMode::Playing) m_playMode = PlayMode::Paused;
            else if (m_playMode == PlayMode::Paused) m_playMode = PlayMode::Playing;
        }
        if (isPaused) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pause");
        ImGui::SameLine();

        // Stop
        if (ImGui::Button(ICON_FA_STOP, btnDim)) {
            if (m_playMode != PlayMode::Stopped) {
                m_playMode = PlayMode::Stopped;
                ShowNotification("Stopped", "info", 2.0f);
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop (Shift+F5)");

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // === Snap Controls (right side) ===
        ImGui::Checkbox("Snap", &m_snapEnabled);
        if (m_snapEnabled) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::DragFloat("##SnapVal", &m_snapValue, 0.1f, 0.1f, 100.0f, "%.1f");
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void EditorUI::RenderStatusBar() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    float statusBarHeight = 24.0f;
    ImVec2 statusBarPos(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight);
    ImVec2 statusBarSize(viewport->WorkSize.x, statusBarHeight);

    ImGui::SetNextWindowPos(statusBarPos);
    ImGui::SetNextWindowSize(statusBarSize);
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 3));
    if (ImGui::Begin("##StatusBar", nullptr, flags)) {
        // Left: engine connection status
        ImVec4 statusColor = m_engineConnected
            ? ImVec4(0.3f, 0.8f, 0.3f, 1.0f)
            : ImVec4(0.8f, 0.3f, 0.3f, 1.0f);
        ImGui::TextColored(statusColor, ICON_FA_CIRCLE);
        ImGui::SameLine();
        ImGui::Text("Engine: %s", m_engineConnected ? "Connected" : "Disconnected");

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // Center: tool + selection
        const char* toolNames[] = { "Move", "Rotate", "Scale" };
        ImGui::Text(ICON_FA_ARROWS_ALT " %s | Objects: %d | Selected: %d",
                    toolNames[(int)m_currentTool], m_sceneObjectCount, m_selectedObjectCount);

        // Right: FPS + frame info
        float fps = m_stats.frameTime > 0.001f ? 1000.0f / m_stats.frameTime : 0.0f;
        ImVec4 fpsColor = fps >= 60.0f ? ImVec4(0.3f, 0.8f, 0.3f, 1.0f)
                        : fps >= 30.0f ? ImVec4(0.9f, 0.9f, 0.3f, 1.0f)
                        : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);

        float rightOffset = ImGui::GetWindowWidth() - 360;
        if (rightOffset > ImGui::GetCursorPosX()) {
            ImGui::SameLine(rightOffset);
        }
        ImGui::TextColored(fpsColor, "%.0f FPS", fps);
        ImGui::SameLine();
        ImGui::Text("| %.1fms | Assets: %d | Frame: %llu",
                    m_stats.frameTime, m_assetDatabaseSize, (unsigned long long)m_frameNumber);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void EditorUI::RenderNotifications() {
    const float NOTIFICATION_WIDTH = 320.0f;
    const float NOTIFICATION_HEIGHT = 52.0f;
    const float NOTIFICATION_SPACING = 6.0f;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    float yOffset = viewport->WorkPos.y + 8.0f;

    for (size_t i = 0; i < m_notifications.size(); ++i) {
        const auto& notification = m_notifications[i];

        // Fade out in last 0.5 seconds
        float alpha = 1.0f;
        if (notification.duration > 0.0f && notification.timeLeft < 0.5f) {
            alpha = std::max(0.0f, notification.timeLeft / 0.5f);
        }

        ImVec2 notificationPos(
            viewport->WorkPos.x + viewport->WorkSize.x - NOTIFICATION_WIDTH - 12.0f,
            yOffset + i * (NOTIFICATION_HEIGHT + NOTIFICATION_SPACING));

        ImGui::SetNextWindowPos(notificationPos);
        ImGui::SetNextWindowSize(ImVec2(NOTIFICATION_WIDTH, NOTIFICATION_HEIGHT));
        ImGui::SetNextWindowBgAlpha(0.92f * alpha);

        std::string windowName = "##Notification" + std::to_string(i);
        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;

        // Determine stripe and icon color
        ImVec4 stripeColor(0.176f, 0.549f, 0.941f, alpha); // blue default
        const char* icon = ICON_FA_INFO_CIRCLE;
        if (notification.type == "error") {
            stripeColor = ImVec4(0.898f, 0.224f, 0.208f, alpha);
            icon = ICON_FA_TIMES;
        } else if (notification.type == "warning") {
            stripeColor = ImVec4(0.961f, 0.651f, 0.137f, alpha);
            icon = ICON_FA_EXCLAMATION;
        } else if (notification.type == "success") {
            stripeColor = ImVec4(0.298f, 0.686f, 0.314f, alpha);
            icon = ICON_FA_CHECK;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        if (ImGui::Begin(windowName.c_str(), nullptr, flags)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 wp = ImGui::GetWindowPos();
            ImVec2 ws = ImGui::GetWindowSize();

            // Colored left stripe
            dl->AddRectFilled(wp, ImVec2(wp.x + 4, wp.y + ws.y),
                ImGui::ColorConvertFloat4ToU32(stripeColor), 4.0f, ImDrawFlags_RoundCornersLeft);

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

void EditorUI::RenderPanels() {
    static float lastUpdateTime = 0.0f;
    static auto lastClock = std::chrono::steady_clock::now();
    
    auto now = std::chrono::steady_clock::now();
    float deltaTime = std::chrono::duration<float>(now - lastClock).count();
    lastClock = now;
    
    for (auto& [name, panel] : m_panels) {
        if (panel->IsVisible()) {
            panel->Update(deltaTime); // Fixed: Pass proper delta time
            panel->Render();
        }
    }
}

void EditorUI::RenderModalDialogs() {
    if (m_currentDialog.isOpen) {
        ImGui::OpenPopup(m_currentDialog.title.c_str());
        
        if (ImGui::BeginPopupModal(m_currentDialog.title.c_str(), &m_currentDialog.isOpen)) {
            if (m_currentDialog.content) {
                m_currentDialog.content();
            }
            
            ImGui::Separator();
            
            for (const auto& [buttonName, callback] : m_currentDialog.buttons) {
                if (ImGui::Button(buttonName.c_str())) {
                    if (callback) callback();
                    m_currentDialog.isOpen = false;
                }
                ImGui::SameLine();
            }
            
            ImGui::EndPopup();
        }
    }
}

void EditorUI::UpdateStats(float deltaTime) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastStatsUpdate);
    
    if (elapsed.count() >= 500) { // Update every 500ms
        m_stats.frameTime = deltaTime * 1000.0f; // Convert to ms
        m_stats.visiblePanels = 0;
        m_stats.totalPanels = static_cast<int>(m_panels.size());
        
        for (const auto& [name, panel] : m_panels) {
            if (panel->IsVisible()) {
                m_stats.visiblePanels++;
            }
        }
        
        m_stats.lastUpdate = now;
        m_lastStatsUpdate = now;
    }
}

// Simple implementations for interface methods
bool EditorUI::IsPanelVisible(const std::string& panelName) const {
    auto it = m_panels.find(panelName);
    return it != m_panels.end() ? it->second->IsVisible() : false;
}

void EditorUI::SetPanelVisible(const std::string& panelName, bool visible) {
    auto it = m_panels.find(panelName);
    if (it != m_panels.end()) {
        it->second->SetVisible(visible);
    }
}

bool EditorUI::SaveLayout(const std::string& layoutName, const std::string& description) {
    // Implementation for saving ImGui docking layout
    try {
        std::string layoutsDir = "Layouts";
        if (!std::filesystem::exists(layoutsDir)) {
            std::filesystem::create_directories(layoutsDir);
        }
        
        std::string filePath = layoutsDir + "/" + layoutName + ".ini";
        
        // Save ImGui settings to specific file
        ImGui::SaveIniSettingsToDisk(filePath.c_str());
        
        // Save layout metadata
        std::ofstream metaFile(filePath + ".meta");
        if (metaFile.is_open()) {
            metaFile << "name=" << layoutName << "\n";
            metaFile << "description=" << description << "\n";
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            metaFile << "created=" << time_t << "\n";
        }
        
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool EditorUI::LoadLayout(const std::string& layoutName) {
    // Implementation for loading ImGui docking layout
    try {
        std::string filePath = "Layouts/" + layoutName + ".ini";
        
        if (!std::filesystem::exists(filePath)) {
            return false;
        }
        
        // Load ImGui settings from specific file
        ImGui::LoadIniSettingsFromDisk(filePath.c_str());
        
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void EditorUI::ResetToDefaultLayout() {
    // Implementation for resetting to default layout
    // Instead of ClearIniSettings, we'll load default settings
    
    // Reset all panels to default visibility
    for (auto& [name, panel] : m_panels) {
        // Set sensible defaults based on panel type
        if (name == "Scene View" || name == "Console" || name == "Hierarchy") {
            panel->SetVisible(true);
        } else {
            panel->SetVisible(false);
        }
    }
    
    // Reset ImGui style to default
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle(); // Reset to default constructor values
}

void EditorUI::ApplyTheme(const std::string& themeName) {
    m_currentTheme = themeName;

    if (!EditorTheme::ApplyTheme(themeName)) {
        // Fallback to basic ImGui dark style
        ImGui::StyleColorsDark();
    }
}

void EditorUI::ShowNotification(const std::string& message, const std::string& type, float duration) {
    Notification notification;
    notification.message = message;
    notification.type = type;
    notification.duration = duration;
    notification.timeLeft = duration;
    notification.timestamp = std::chrono::steady_clock::now();
    
    m_notifications.push_back(notification);
}

std::string EditorUI::ExecuteCommand(const std::string& command) {
    // Simple command parsing
    std::vector<std::string> parts;
    std::string current;
    for (char c : command) {
        if (c == ' ') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    
    if (parts.empty()) {
        return "Empty command";
    }
    
    auto it = m_commands.find(parts[0]);
    if (it != m_commands.end()) {
        std::vector<std::string> args(parts.begin() + 1, parts.end());
        return it->second(args);
    }
    
    return "Unknown command: " + parts[0];
}

void EditorUI::RegisterCommand(const std::string& name, 
                              std::function<std::string(const std::vector<std::string>&)> handler,
                              const std::string& description) {
    m_commands[name] = handler;
}

void EditorUI::SetFrameNumber(uint64_t frameNumber) {
    m_frameNumber = frameNumber;
}

UIStats EditorUI::GetStats() const {
    return m_stats;
}

void EditorUI::SetEngineConnected(bool connected) {
    m_engineConnected = connected;
}

void EditorUI::UpdateAssetDatabaseInfo(int assetCount, size_t memoryUsage) {
    m_assetDatabaseSize = assetCount;
    m_assetMemoryUsage = memoryUsage;
}

void EditorUI::UpdateSceneInfo(int objectCount, int selectedCount) {
    m_sceneObjectCount = objectCount;
    m_selectedObjectCount = selectedCount;
}

bool EditorUI::HasRecoveryData() {
    return m_recoveryDataAvailable;
}

bool EditorUI::ShowRecoveryDialog() {
    // TODO: Implement recovery dialog
    return false;
}

bool EditorUI::ImportLayout(const std::string& filePath) {
    // TODO: Implement layout import
    return false;
}

bool EditorUI::ExportLayout(const std::string& filePath) {
    // TODO: Implement layout export
    return false;
}

void EditorUI::ShowModalDialog(const std::string& title, 
                              std::function<void()> content,
                              const std::unordered_map<std::string, std::function<void()>>& buttons) {
    m_currentDialog.title = title;
    m_currentDialog.content = content;
    m_currentDialog.buttons = buttons;
    m_currentDialog.isOpen = true;
}

} // namespace SparkEditor