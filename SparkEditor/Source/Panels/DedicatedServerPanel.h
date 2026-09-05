/**
 * @file DedicatedServerPanel.h
 * @brief Editor panel for dedicated server management, cooking, and PIE server spawning
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a UI for:
 * - Configuring dedicated server settings (port, max players, tick rate)
 * - Maintaining the map pick-list used for the launch map (SparkServer runs one map)
 * - Cooking and packaging standalone dedicated server executables
 * - Launching a local dedicated server process from the editor for testing
 * - Listing the locally launched server (there is no LAN discovery transport, and
 *   joining is disabled until an editor client transport exists)
 * - Server monitoring from the health report SparkServer publishes (uptime, player
 *   count); RCON and per-player kick/ban are not available.
 */

#pragma once

#include "../Core/EditorPanel.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Spark::Editor
{
    class PlayModeManager;
} // namespace Spark::Editor

namespace SparkEditor
{
    class BuildPipeline;
    class DedicatedServerProcessController;

    /// @brief Editor panel for dedicated server configuration, cooking, and PIE launching
    class DedicatedServerPanel : public EditorPanel
    {
      public:
        // ============================================================
        // Server cook target configuration
        // ============================================================

        enum class ServerPlatform
        {
            WindowsX64,
            LinuxX64,
            LinuxARM64,
            MacOSX64,
            MacOSARM64
        };

        enum class ServerBuildProfile
        {
            Debug,
            Development,
            Shipping
        };

        struct ServerCookSettings
        {
            ServerPlatform platform = ServerPlatform::WindowsX64;
            ServerBuildProfile profile = ServerBuildProfile::Development;
            std::string outputDirectory = "Build/Server";
            std::string executableName = "SparkDedicatedServer";

            // Cook options
            bool stripEditor = true;
            bool stripGraphics = true; ///< True = headless (no GPU)
            bool stripAudio = true;
            bool includeDebugSymbols = false;
            bool compressAssets = false;
            int compressionLevel = 6; ///< 1-9
            bool enableLogging = true;
            bool enableRcon = false;
        };

        // ============================================================
        // PIE server launch configuration
        // ============================================================

        struct PIEServerConfig
        {
            std::string serverName = "PIE Local Server";
            uint16_t port = 27015;
            int maxPlayers = 8;
            float tickRate = 60.0f;
            int gameMode = 0; ///< Index into game mode list
            std::string mapName = "default";
            bool openConsoleWindow = false; ///< Reserved until process-window control is implemented.
            bool autoConnect = false;       ///< Reserved until editor client transport integration is implemented.
            std::string bindAddress = "loopback";
            bool enableRcon = false;
            std::string rconPassword = "dev";
        };

        // ============================================================
        // LAN server discovery entry
        // ============================================================

        struct DiscoveredServer
        {
            std::string name;
            std::string map;
            std::string address;
            uint16_t port = 0;
            int currentPlayers = 0;
            int maxPlayers = 0;
            int gameMode = 0;
            float ping = 0.0f;
            std::chrono::steady_clock::time_point discoveredAt;
        };

        // ============================================================
        // Construction
        // ============================================================

        DedicatedServerPanel();
        ~DedicatedServerPanel() override;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        std::string GetTypeName() const override { return "DedicatedServerPanel"; }

        /// @brief Connect to PlayModeManager for PIE integration.
        void SetPlayModeManager(Spark::Editor::PlayModeManager* mgr) { m_playModeManager = mgr; }

        // ============================================================
        // External accessors
        // ============================================================

        const PIEServerConfig& GetPIEConfig() const { return m_pieConfig; }
        const ServerCookSettings& GetCookSettings() const { return m_cookSettings; }
        bool IsPIEServerRunning() const { return m_pieServerRunning; }

      private:
        // -- Tab rendering --
        void RenderServerConfigTab();
        void RenderMapRotationTab();
        void RenderCookPackageTab();
        void RenderPIEServerTab();
        void RenderServerBrowserTab();
        void RenderMonitorTab();

        // -- Cook/Package helpers --
        void StartCookServer();
        void RenderCookProgress();
        void RenderCookLog();
        static const char* GetPlatformName(ServerPlatform platform);
        static const char* GetProfileName(ServerBuildProfile profile);

        // -- PIE server helpers --
        void LaunchPIEServer();
        void StopPIEServer();
        void RenderPIEServerConsole();

        // -- Server browser helpers --
        void RefreshServerBrowser();
        void ConnectToServer(const DiscoveredServer& server);

        /**
         * @brief Player count read from the server's health JSON.
         * @return The reported count, or -1 when no health report has been read yet.
         */
        int GetReportedPlayerCount() const;

        // -- Monitor helpers --
        void RenderPlayerList();
        void RenderRconConsole();
        void RenderServerStats();

        // -- State --
        Spark::Editor::PlayModeManager* m_playModeManager = nullptr;

        // Server config (persistent across sessions)
        PIEServerConfig m_pieConfig;
        ServerCookSettings m_cookSettings;

        // Map rotation list
        std::vector<std::string> m_mapRotation;
        int m_selectedMapIndex = -1;
        char m_newMapNameBuf[256] = {};

        // Cook state
        bool m_isCooking = false;
        float m_cookProgress = 0.0f;
        std::string m_cookStatus = "Idle";
        struct CookLogEntry
        {
            std::string message;
            std::string level; // "info", "warning", "error", "success"
        };
        std::vector<CookLogEntry> m_cookLog;
        std::unique_ptr<BuildPipeline> m_cookPipeline;

        // PIE server state
        bool m_pieServerRunning = false;
        float m_pieServerUptime = 0.0f;
        std::vector<std::string> m_pieServerLog;
        std::unique_ptr<DedicatedServerProcessController> m_serverProcess;
        char m_pieRconInputBuf[512] = {};

        /// Result of the last connect attempt, shown under the Direct Connect field so a refused
        /// connect is visible rather than silent.
        std::string m_connectStatus;

        // Server browser
        std::vector<DiscoveredServer> m_discoveredServers;

        // Monitor / RCON
        char m_rconInputBuf[512] = {};
        std::vector<std::string> m_rconHistory;

        // UI state
        int m_activeTab = 0;
        static constexpr int TAB_CONFIG = 0;
        static constexpr int TAB_MAPS = 1;
        static constexpr int TAB_COOK = 2;
        static constexpr int TAB_PIE = 3;
        static constexpr int TAB_BROWSER = 4;
        static constexpr int TAB_MONITOR = 5;

        // Game mode names (matching FPSToolsPanel)
        static constexpr const char* GAME_MODE_NAMES[] = {"Deathmatch", "Team Deathmatch",  "Capture The Flag",
                                                          "Domination", "Search & Destroy", "Free For All",
                                                          "Custom"};
        static constexpr int NUM_GAME_MODES = 7;
    };

} // namespace SparkEditor
