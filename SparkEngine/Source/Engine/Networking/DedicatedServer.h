/**
 * @file DedicatedServer.h
 * @brief Dedicated game server with full lifecycle management
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a standalone dedicated server implementation for hosting
 * multiplayer FPS matches without a rendering client. Supports:
 * - Headless server startup and tick loop
 * - Server configuration (max players, tick rate, maps, game modes)
 * - Map rotation with automatic level transitions
 * - Remote console (RCON) for administration
 * - LAN broadcast discovery so clients on the local network can find the server
 * - Graceful shutdown with client notification
 * - Server-side game state authority
 *
 * All networking code is guarded by ENABLE_NETWORKING.
 */

#pragma once

#include "../../Core/Platform.h"
#include "NetworkManager.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef ENABLE_NETWORKING

// Windows headers may redefine SendMessage; keep our namespace clean.
#ifdef SendMessage
#undef SendMessage
#endif

namespace Spark::Net
{

    // ============================================================================
    // Server Configuration
    // ============================================================================

    /// @brief Game mode identifiers matching FPSToolsPanel definitions
    enum class GameModeType : uint8_t
    {
        Deathmatch = 0,
        TeamDeathmatch,
        CaptureTheFlag,
        Domination,
        SearchAndDestroy,
        FreeForAll,
        Custom
    };

    /// @brief Full server configuration used to launch a dedicated server
    struct ServerConfig
    {
        // Identity
        std::string serverName = "Spark Dedicated Server";
        std::string motd; ///< Message of the day shown on connect

        // Network
        uint16_t port = DEFAULT_PORT;
        int maxClients = 32;
        float tickRate = 60.0f;             ///< Server simulation ticks per second
        float clientTimeoutSeconds = 30.0f; ///< Kick after this many seconds of silence
        float heartbeatIntervalSeconds = 1.0f;
        bool lanOnly = false; ///< Restrict to LAN connections

        // Game
        GameModeType gameMode = GameModeType::Deathmatch;
        std::string customGameModeName; ///< For GameModeType::Custom
        int scoreLimit = 50;
        float timeLimitMinutes = 15.0f;
        int roundCount = 1;
        bool friendlyFire = false;
        bool autoBalanceTeams = true;

        // Map rotation
        std::vector<std::string> mapRotation;
        bool randomizeMapOrder = false;

        // Administration
        std::string rconPassword; ///< Empty = RCON disabled
        uint16_t rconPort = 0;    ///< 0 = same as game port + 1
        bool enableLogging = true;
        std::string logFilePath = "server.log";

        // LAN discovery
        bool enableLanBroadcast = true;
        uint16_t lanBroadcastPort = 27016; ///< UDP port for LAN discovery

        // Performance
        bool enableAntiCheat = false;
        float replicationRate = 20.0f; ///< Entity replication Hz
        int snapshotHistorySize = 64;  ///< Snapshots kept for lag compensation
    };

    // ============================================================================
    // Server Events
    // ============================================================================

    /// @brief Callbacks fired by the dedicated server during its lifecycle
    struct ServerCallbacks
    {
        std::function<void()> onServerStarted;
        std::function<void()> onServerStopped;
        std::function<void(ClientID, const std::string&)> onClientConnected;
        std::function<void(ClientID, const std::string&)> onClientDisconnected;
        std::function<void(const std::string&)> onMapChanged;
        std::function<void(const std::string&)> onChatMessage;
        std::function<void(const std::string&, const std::string&)> onRconCommand; ///< (command, response)
        std::function<void(const std::string&)> onLogMessage;
    };

    // ============================================================================
    // Server Statistics
    // ============================================================================

    struct ServerStats
    {
        float uptimeSeconds = 0.0f;
        uint64_t totalTicksProcessed = 0;
        float averageTickMs = 0.0f;
        float peakTickMs = 0.0f;
        uint32_t currentPlayers = 0;
        uint32_t peakPlayers = 0;
        uint64_t totalBytesIn = 0;
        uint64_t totalBytesOut = 0;
        uint32_t totalConnectionsServed = 0;
        float currentTickRate = 0.0f;

        // Match
        std::string currentMap;
        int currentMapIndex = 0;
        float matchTimeRemaining = 0.0f;
        int currentRound = 1;
    };

    // ============================================================================
    // RCON Command
    // ============================================================================

    /// @brief A registered remote console command
    struct RconCommand
    {
        std::string name;
        std::string description;
        std::function<std::string(const std::vector<std::string>&)> handler;
    };

    // ============================================================================
    // LAN Discovery
    // ============================================================================

    /// @brief Information broadcast on LAN for server discovery
    struct ServerBroadcastInfo
    {
        std::string serverName;
        std::string mapName;
        GameModeType gameMode = GameModeType::Deathmatch;
        uint16_t port = DEFAULT_PORT;
        int currentPlayers = 0;
        int maxPlayers = 32;
        float ping = 0.0f; ///< Filled in by the discovering client
    };

    // ============================================================================
    // Dedicated Server
    // ============================================================================

    /// @brief Manages a headless dedicated game server
    ///
    /// The DedicatedServer wraps NetworkManager in server mode and adds:
    /// - Fixed-timestep tick loop (runs on a dedicated thread or externally driven)
    /// - Map rotation with automatic transitions
    /// - RCON command registration and dispatch
    /// - LAN broadcast for server discovery
    /// - Server-authoritative match state (score, timer, rounds)
    ///
    /// Thread safety: The server tick loop runs on its own thread when
    /// Start() is used. External code may call RCON, kick, and query
    /// methods from any thread — they are protected by internal mutexes.
    class DedicatedServer
    {
      public:
        DedicatedServer();
        ~DedicatedServer();

        // Non-copyable
        DedicatedServer(const DedicatedServer&) = delete;
        DedicatedServer& operator=(const DedicatedServer&) = delete;

        // -- Lifecycle --

        /// @brief Start the server with the given configuration.
        /// Launches the tick loop on a background thread.
        /// @return true on success.
        bool Start(const ServerConfig& config);

        /// @brief Stop the server gracefully: notifies clients, flushes
        /// outgoing messages, then shuts down networking.
        void Stop();

        /// @brief Check whether the server is currently running.
        bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

        /// @brief Run a single server tick. Used when driving the loop
        /// externally (e.g. from the editor PIE) instead of Start().
        /// @param deltaTime Seconds since last tick.
        void Tick(float deltaTime);

        /// @brief Initialize the server without starting the background
        /// tick loop. Pair with Tick() for external driving.
        bool InitializeOnly(const ServerConfig& config);

        // -- Map Management --

        /// @brief Load the next map in the rotation.
        void ChangeMap(const std::string& mapName);

        /// @brief Advance to the next map in the rotation list.
        void RotateToNextMap();

        /// @brief Get the name of the currently loaded map.
        const std::string& GetCurrentMap() const { return m_currentMap; }

        // -- Match State --

        /// @brief Start a new match on the current map.
        void StartMatch();

        /// @brief End the current match (triggers score tally, map rotation).
        void EndMatch();

        /// @brief Check if a match is currently in progress.
        bool IsMatchInProgress() const { return m_matchInProgress; }

        /// @brief Get remaining match time in seconds.
        float GetMatchTimeRemaining() const { return m_matchTimeRemaining; }

        // -- Player Management --

        /// @brief Kick a player with an optional reason.
        void KickPlayer(ClientID id, const std::string& reason = "");

        /// @brief Ban a player by client ID (persists until server restart).
        void BanPlayer(ClientID id, const std::string& reason = "");

        /// @brief Get information about all connected clients.
        std::vector<ClientInfo> GetConnectedClients() const;

        /// @brief Get the number of currently connected players.
        uint32_t GetPlayerCount() const;

        // -- RCON --

        /// @brief Register a remote console command.
        void RegisterRconCommand(const std::string& name, const std::string& description,
                                 std::function<std::string(const std::vector<std::string>&)> handler);

        /// @brief Execute an RCON command string, e.g. "kick 3 cheating".
        /// @return The command response text.
        std::string ExecuteRcon(const std::string& commandLine);

        /// @brief Get all registered RCON commands.
        const std::vector<RconCommand>& GetRconCommands() const { return m_rconCommands; }

        // -- LAN Discovery --

        /// @brief Start broadcasting presence on LAN.
        void StartLanBroadcast();

        /// @brief Stop LAN broadcasting.
        void StopLanBroadcast();

        /// @brief Discover LAN servers (client-side static utility).
        /// @param broadcastPort Port to listen for server broadcasts.
        /// @param timeoutMs How long to listen.
        /// @return List of discovered servers.
        static std::vector<ServerBroadcastInfo> DiscoverLanServers(uint16_t broadcastPort = 27016,
                                                                   int timeoutMs = 2000);

        // -- Configuration & Stats --

        const ServerConfig& GetConfig() const { return m_config; }
        const ServerStats& GetStats() const { return m_stats; }
        void SetCallbacks(const ServerCallbacks& callbacks) { m_callbacks = callbacks; }

        /// @brief Console integration: full server status string.
        std::string Console_GetStatus() const;

      private:
        // -- Internal --

        /// @brief The main server tick loop (runs on m_tickThread).
        void TickLoop();

        /// @brief Process server-specific incoming messages.
        void ProcessServerMessages(float deltaTime);

        /// @brief Update match timer, round transitions, score limits.
        void UpdateMatchState(float deltaTime);

        /// @brief Send a LAN broadcast packet (called periodically).
        void SendLanBroadcast();

        /// @brief Register built-in RCON commands (help, status, kick, ban, map, say).
        void RegisterBuiltInRconCommands();

        /// @brief Log a message to file and callback.
        void Log(const std::string& message);

        /// @brief Parse an RCON command string into name + arguments.
        static void ParseRconCommandLine(const std::string& commandLine, std::string& outName,
                                         std::vector<std::string>& outArgs);

        // State
        ServerConfig m_config;
        ServerCallbacks m_callbacks;
        ServerStats m_stats;
        std::atomic<bool> m_running{false};
        bool m_matchInProgress = false;
        float m_matchTimeRemaining = 0.0f;
        int m_currentRound = 1;
        std::string m_currentMap;
        int m_currentMapIndex = 0;

        // Tick loop
        std::thread m_tickThread;
        std::chrono::steady_clock::time_point m_startTime;

        // RCON
        std::vector<RconCommand> m_rconCommands;
        mutable std::mutex m_rconMutex;

        // LAN broadcast
        std::atomic<bool> m_lanBroadcastActive{false};
        std::thread m_lanBroadcastThread;
        float m_lanBroadcastInterval = 3.0f; ///< Seconds between broadcasts

        // Bans (in-memory for the session)
        std::vector<ClientID> m_bannedClients;
        mutable std::mutex m_banMutex;

        // Logging
        mutable std::mutex m_logMutex;
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
