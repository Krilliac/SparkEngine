/**
 * @file EditorUI.h
 * @brief Core UI management system for the Spark Engine Editor with advanced features
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file defines the main UI controller that manages all editor panels,
 * layout, themes, and user interface interactions with enhanced logging,
 * crash handling, and comprehensive layout management.
 */

#pragma once

#include <imgui.h>
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <functional>
#include <chrono>

#include "EditorLogger.h"
#include "EditorLayoutManager.h"
#include "EditorNotificationManager.h"
#include "EditorCrashHandler.h"
#include "ProjectManager.h"
#include "../UndoRedo/UndoRedoManager.h"
#include "../CommandHistory.h"
#include "../Prefabs/PrefabManager.h"
#include "../Search/CommandPalette.h"
#include "Engine/Editor/PlayModeManager.h"
#include "../Gizmos/GizmoSystem.h"
#include "../Communication/CollaborativeEditSession.h"
#include "../Communication/LiveEditBridge.h"
#include "Engine/ECS/Components.h"

#ifdef _WIN32
#include "Graphics/GraphicsEngine.h"
struct ID3D11Device;
struct ID3D11DeviceContext;
#endif

namespace SparkEditor
{

    // Forward declarations
    class EditorPanel;
    class EditorPluginManager;
    class ProjectBrowserPanel;
    class HierarchyPanel;
    struct EditorConfig; // Forward declare instead of defining

    /**
 * @brief UI update statistics
 */
    struct UIStats
    {
        float frameTime = 0.0f;
        float averageFrameTime = 0.0f;
        int drawCalls = 0;
        size_t memoryUsage = 0;
        int visiblePanels = 0;
        int totalPanels = 0;
        float layoutSwitchTime = 0.0f;
        std::chrono::steady_clock::time_point lastUpdate;
    };

    /**
 * @brief Simple UI management system for the Spark Engine Editor
 */
    class EditorUI
    {
      public:
        EditorUI();
        ~EditorUI();

        bool Initialize(const EditorConfig& config);
        void Update(float deltaTime);
        void Render();
        void Shutdown();

        // Simple panel management
        bool IsPanelVisible(const std::string& panelName) const;
        void SetPanelVisible(const std::string& panelName, bool visible);

        // Simple accessors
        EditorLayoutManager* GetLayoutManager() const { return m_layoutManager.get(); }
        EditorLogger* GetLogger() const { return m_logger.get(); }
        EditorCrashHandler* GetCrashHandler() const { return m_crashHandler; }
        ProjectManager* GetProjectManager() { return m_projectManager.get(); }
        /// @brief The single process-wide undo/redo history — the same
        /// UndoRedoManager singleton that Spark::Editor::CommandHistory wraps
        /// and that SwapWorld() clears before freeing the old World. All undo
        /// surfaces (Ctrl+Z, Edit menu, command palette, UndoHistory panel)
        /// MUST operate on this instance; a second private instance would be
        /// invisible to SwapWorld()'s history-clear and re-open the
        /// use-after-free window on scene replacement.
        UndoRedoManager* GetUndoRedoManager() { return &UndoRedoManager::GetInstance(); }
        PrefabManager* GetPrefabManager() { return m_prefabManager.get(); }
        CommandPalette* GetCommandPalette() { return m_commandPalette.get(); }
        GizmoSystem* GetGizmoSystem() { return m_gizmoSystem.get(); }
        CollaborativeEditSession* GetCollabSession() { return m_collabSession.get(); }
        LiveEditBridge* GetLiveEditBridge() { return m_liveEditBridge.get(); }

        /// @brief The single live ECS World being edited (the document). Owned by
        /// EditorUI; panels (SceneView, Hierarchy, Inspector) hold a non-owning
        /// pointer to it via their own SetWorld().
        World* GetWorld() { return m_world.get(); }

        /// @brief The currently selected ECS entity (Unit C2) — the
        /// document-level selection for World-backed panels (Hierarchy
        /// publishes it, Inspector (C3) consumes it). Distinct from the
        /// legacy SceneFile SelectionManager used by the dormant SceneFile
        /// hierarchy path. entt::null when nothing is selected.
        ::EntityID GetSelectedEntity() const { return m_selectedEntity; }
        void SetSelectedEntity(::EntityID e) { m_selectedEntity = e; }

        /// @brief Set non-owning pointer to the plugin manager (owned by EditorApplication)
        void SetPluginManager(EditorPluginManager* pluginManager) { m_pluginManager = pluginManager; }

        // Project operations (triggered from menu bar)
        void ShowNewProjectDialog();
        void ShowOpenProjectDialog();
        void ShowProjectBrowser();

        // Simple layout operations
        bool SaveLayout(const std::string& layoutName, const std::string& description = "");
        bool LoadLayout(const std::string& layoutName);
        void ResetToDefaultLayout();

        // Simple theme operations
        void ApplyTheme(const std::string& themeName);
        const std::string& GetCurrentTheme() const { return m_currentTheme; }

        // Simple notifications
        void ShowNotification(const std::string& message, const std::string& type = "info", float duration = 3.0f);

        // Simple command system
        std::string ExecuteCommand(const std::string& command);
        void RegisterCommand(const std::string& name,
                             std::function<std::string(const std::vector<std::string>&)> handler,
                             const std::string& description = "");

        // Simple engine status
        void SetFrameNumber(uint64_t frameNumber);
        UIStats GetStats() const;
        void SetEngineConnected(bool connected);
        bool IsEngineConnected() const { return m_engineConnected; }

        // Simple asset and scene info
        void UpdateAssetDatabaseInfo(int assetCount, size_t memoryUsage);
        void UpdateSceneInfo(int objectCount, int selectedCount);

        // Simple recovery
        bool HasRecoveryData();
        bool ShowRecoveryDialog();

        // Exit request (set by File > Exit)
        bool IsExitRequested() const { return m_exitRequested; }

#ifdef _WIN32
        // Pass graphics device to panels that need it (SceneView)
        void SetGraphicsDevice(ID3D11Device* device, ID3D11DeviceContext* context);

        /// @brief Get the editor's GraphicsEngine, attached to the editor's own
        /// D3D11 device via InitializeFromDevice() (no swapchain). Null until
        /// SetGraphicsDevice() has been called. Lets panels (SceneView) drive
        /// Spark::RenderWorldBasic() through the shared basic-shader path.
        GraphicsEngine* GetGraphics() { return m_graphics.get(); }
#endif

        // Scene management helpers
        bool SaveCurrentScene(const std::string& path);

        /// @brief Load a reflected scene JSON (as written by SaveCurrentScene)
        /// into a fresh ::World, replace m_world with it, and re-wire the
        /// caching panels (SceneView, Hierarchy) so they don't dangle a
        /// pointer to the old World. Returns false (leaving the current
        /// World untouched) if the load fails.
        bool OpenScene(const std::string& path);

        const std::string& GetCurrentSceneName() const { return m_currentSceneName; }
        bool IsSceneModified() const
        {
            // m_sceneModified is only flipped true on one menu path (GameObject
            // creation in EditorMenuBar.cpp) and is therefore unreliable on its
            // own. Every edit path that actually mutates the document --
            // hierarchy create/delete/duplicate/rename/reparent, Inspector
            // property edits, gizmo drags -- routes through
            // Spark::Editor::CommandHistory::Execute(), which maintains a
            // precise monotonic-sequence dirty flag (IsModified()) independent
            // of which UI surface triggered the command. OR the two together
            // so no edit path can under-report unsaved changes.
            return m_sceneModified || Spark::Editor::CommandHistory::GetInstance().IsModified();
        }

        // Simple file operations
        bool ImportLayout(const std::string& filePath);
        bool ExportLayout(const std::string& filePath);

        // Simple dialog
        void ShowModalDialog(const std::string& title, std::function<void()> content,
                             const std::unordered_map<std::string, std::function<void()>>& buttons);

      private:
        // Core systems
        std::unique_ptr<EditorLogger> m_logger;
        std::unique_ptr<EditorLayoutManager> m_layoutManager;
        EditorCrashHandler* m_crashHandler = nullptr;
        std::unique_ptr<ProjectManager> m_projectManager;
        std::shared_ptr<ProjectBrowserPanel> m_projectBrowserPanel;
        std::unique_ptr<PrefabManager> m_prefabManager;
        std::unique_ptr<CommandPalette> m_commandPalette;

        // Plugin manager (non-owning, owned by EditorApplication)
        EditorPluginManager* m_pluginManager = nullptr;

        // Panel management
        std::unordered_map<std::string, std::shared_ptr<EditorPanel>> m_panels;

        // Configuration - store as pointer to avoid incomplete type issues
        std::unique_ptr<EditorConfig> m_config;

        // UI state
        bool m_isInitialized = false;
        std::string m_currentTheme = "Spark Ember";
        bool m_showDemoWindow = false;
        bool m_firstFrame = true;
        bool m_showWelcomeScreen = true;
        uint64_t m_frameNumber = 0;

        // Statistics tracking
        mutable UIStats m_stats;
        std::chrono::steady_clock::time_point m_lastStatsUpdate;

        // Status tracking
        bool m_engineConnected = false;
        int m_sceneObjectCount = 0;
        int m_assetDatabaseSize = 0;
        size_t m_assetMemoryUsage = 0;

        // Notifications — owned EditorNotificationManager handles state + rendering.
        std::unique_ptr<EditorNotificationManager> m_notificationManager;

        // Dialog state
        struct ModalDialog
        {
            std::string title;
            std::function<void()> content;
            std::unordered_map<std::string, std::function<void()>> buttons;
            bool isOpen = false;
        };
        ModalDialog m_currentDialog;

        // Commands
        std::unordered_map<std::string, std::function<std::string(const std::vector<std::string>&)>> m_commands;

        // Performance metrics
        std::vector<float> m_frameTimeHistory;
        static constexpr size_t MAX_FRAME_HISTORY = 60;

        // Recovery
        bool m_recoveryDataAvailable = false;

        // Additional member variable for selected objects count
        int m_selectedObjectCount = 0;

        // Toolbar state — delegates to PlayModeManager for scene snapshot/restore
        enum class PlayMode
        {
            Stopped,
            Playing,
            Simulating,
            Paused
        };
        PlayMode m_playMode = PlayMode::Stopped;

        /// @brief Play-in-editor manager (scene snapshot, time control, subsystem toggles)
        Spark::Editor::PlayModeManager m_playModeManager;

        enum class TransformTool
        {
            Move,
            Rotate,
            Scale
        };
        TransformTool m_currentTool = TransformTool::Move;

        enum class TransformSpace
        {
            World,
            Local
        };
        TransformSpace m_transformSpace = TransformSpace::World;

        bool m_snapEnabled = false;
        float m_snapValue = 1.0f;

        // Gizmo system — 3D object manipulation overlays
        std::unique_ptr<GizmoSystem> m_gizmoSystem;

        // The single live ECS World being edited (the document). Panels that
        // display/manipulate scene content (SceneView, Hierarchy, Inspector)
        // are wired to this via non-owning World* accessors.
        std::unique_ptr<World> m_world;

        // The currently selected ECS entity (Unit C2). entt::null when
        // nothing is selected. Published by HierarchyPanel, consumed by
        // InspectorPanel (C3).
        ::EntityID m_selectedEntity = entt::null;

#ifdef _WIN32
        // GraphicsEngine attached to the editor's own D3D11 device (via
        // InitializeFromDevice(), no swapchain) — lets SceneViewPanel drive
        // Spark::RenderWorldBasic() through the shared basic-shader path.
        std::unique_ptr<GraphicsEngine> m_graphics;
#endif

        // Collaborative editing — multi-user session management
        std::unique_ptr<CollaborativeEditSession> m_collabSession;

        // Live edit bridge — forwards edits to running AreaServer
        std::unique_ptr<LiveEditBridge> m_liveEditBridge;

        // Exit state
        bool m_exitRequested = false;

        // Scene state
        std::string m_currentScenePath;
        std::string m_currentSceneName = "Untitled";
        bool m_sceneModified = false;

        // Helper methods
        /// @brief Atomically replace the edited document World. Clears the
        /// undo/redo command history FIRST (so no queued command can reference
        /// the about-to-be-freed old World — prevents use-after-free on a later
        /// Undo/Redo), then moves newWorld into m_world, then calls
        /// RewirePanelsToWorld(). Both OpenScene() and the initial world
        /// creation in SetGraphicsDevice() route through it.
        void SwapWorld(std::unique_ptr<::World> newWorld);

        /// @brief Re-point the panels that cache a raw ::World* (SceneView,
        /// Hierarchy) at the current m_world and clear selection. Must be
        /// called any time m_world is (re)assigned — both the initial seed
        /// wiring in SetGraphicsDevice() and OpenScene() share this path so
        /// the caching panels never dangle a pointer to a freed World.
        /// InspectorPanel needs no re-wire — it reads GetWorld() live.
        void RewirePanelsToWorld();
        void RenderMainMenuBar();
        void RenderFileMenu();
        void RenderFileSceneItems();
        void RenderFileProjectItems();
        void RenderEditMenu();
        void RenderGameObjectMenu();
        void RenderGameObject3DSubMenu(const std::function<void(const std::string&)>& createObject);
        void RenderGameObject2DSubMenu();
        void RenderGameObjectVolumeSubMenu(const std::function<void(const std::string&)>& createObject);
        void RenderGameObjectSpecializedSubMenus(const std::function<void(const std::string&)>& createObject);
        // Window menu panels are now rendered dynamically by PanelCategory
        // in RenderWindowMenu() — no per-category submenu functions needed.
        void RenderWindowMenu();
        void RenderFPSToolsMenu();
        void RenderBuildMenu();
        void RenderHelpMenu();
        void RenderToolbar();
        void RenderToolbarTransformTools(float btnSize, ImDrawList* dl, const ImVec4& accentTeal, const ImVec4& pillBg);
        void RenderToolbarPlayControls(float btnSize, ImDrawList* dl, const ImVec4& playGreen,
                                       const ImVec4& accentAmber, const ImVec4& stopRed, const ImVec4& pillBg);
        void RenderToolbarSnapControls(float btnSize, const ImVec4& pillBg);
        void RenderStatusBar();
        void RenderPanels();
        void RenderModalDialogs();
        void RenderWelcomeScreen();
        void SetupDefaultDockLayout(ImGuiID dockspaceId);
        void UpdateStats(float deltaTime);
        void CreatePanels();
        void CreateCorePanels(
            const std::function<void(const std::string&, std::shared_ptr<EditorPanel>)>& registerPanel);
        void CreateToolAndContentPanels(
            const std::function<void(const std::string&, std::shared_ptr<EditorPanel>)>& registerPanel);
        void InitializePanelIcons();
        void InitializePanelCategories();
        void SetDefaultPanelVisibility();
        void InitializeCommandPalette();
        void HandleKeyboardShortcuts();

        // Initialize() helpers — split for readability
        void InitializeManagers(const EditorConfig& config);
        void WireCallbacks();

        // InitializeCommandPalette() helpers — grouped by domain
        void RegisterPanelToggleCommands();
        void RegisterEditCommands();
        void RegisterSceneCommands();
        void RegisterToolCommands();

        // Update() helpers — separate input from tick logic
        void ProcessSceneShortcuts();
        void ProcessGlobalHotkeys();
    };

} // namespace SparkEditor
