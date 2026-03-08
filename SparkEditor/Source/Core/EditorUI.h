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
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <functional>
#include <chrono>

#include "EditorLogger.h"
#include "EditorLayoutManager.h"
#include "EditorCrashHandler.h"
#include "ProjectManager.h"

namespace SparkEditor
{

    // Forward declarations
    class EditorPanel;
    class ProjectBrowserPanel;
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

        // Panel management
        std::unordered_map<std::string, std::shared_ptr<EditorPanel>> m_panels;

        // Configuration - store as pointer to avoid incomplete type issues
        std::unique_ptr<EditorConfig> m_config;

        // UI state
        bool m_isInitialized = false;
        std::string m_currentTheme = "Spark Professional";
        bool m_showDemoWindow = false;
        bool m_firstFrame = true;
        uint64_t m_frameNumber = 0;

        // Statistics tracking
        mutable UIStats m_stats;
        std::chrono::steady_clock::time_point m_lastStatsUpdate;

        // Status tracking
        bool m_engineConnected = false;
        int m_sceneObjectCount = 0;
        int m_assetDatabaseSize = 0;
        size_t m_assetMemoryUsage = 0;

        // Notifications
        struct Notification
        {
            std::string message;
            std::string type;
            float duration;
            float timeLeft;
            std::chrono::steady_clock::time_point timestamp;
        };
        std::vector<Notification> m_notifications;

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

        // Toolbar state
        enum class PlayMode
        {
            Stopped,
            Playing,
            Paused
        };
        PlayMode m_playMode = PlayMode::Stopped;

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

        // Helper methods
        void RenderMainMenuBar();
        void RenderToolbar();
        void RenderStatusBar();
        void RenderNotifications();
        void RenderPanels();
        void RenderModalDialogs();
        void SetupDefaultDockLayout(ImGuiID dockspaceId);
        void UpdateStats(float deltaTime);
        void CreatePanels();
    };

} // namespace SparkEditor