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
 * - Trusted in-process administration command dispatch (legacy RCON API names)
 * - LAN broadcast discovery so clients on the local network can find the server
 * - Graceful shutdown with client notification
 * - Server-side game state authority
 *
 * All networking code is guarded by ENABLE_NETWORKING.
 */

#pragma once

#include "../../Core/Platform.h"
#include "INetworkRuntime.h"
#include "NetworkManagerRuntimeAdapter.h"

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
        // Reserved for source/config compatibility. No remote RCON transport
        // currently consumes these fields; use ExecuteRcon only from trusted
        // host code until an authenticated administration channel is added.
        std::string rconPassword;
        uint16_t rconPort = 0;
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
        std::function<void()> onServerStarted; ///< Fired after the server binds and begins listening.
        std::function<void()> onServerStopped; ///< Fired after the server shuts down cleanly.
        std::function<void(ClientID, const std::string&)>
            onClientConnected; ///< Fired when a client completes handshake (id, name).
        std::function<void(ClientID, const std::string&)>
            onClientDisconnected; ///< Fired when a client disconnects or times out (id, reason).
        std::function<void(const std::string&)> onMapChanged;  ///< Fired after a map change completes (new map name).
        std::function<void(const std::string&)> onChatMessage; ///< Fired on incoming chat (formatted message string).
        std::function<void(const std::string&, const std::string&)>
            onRconCommand; ///< Fired on trusted local admin command dispatch (command, response).
        std::function<void(const std::string&)> onLogMessage; ///< Fired on server log output (message).
    };

    // ============================================================================
    // Server Statistics
    // ============================================================================

    /// @brief Aggregate server performance and match-state metrics.
    struct ServerStats
    {
        float uptimeSeconds = 0.0f;          ///< Seconds since the server started.
        uint64_t totalTicksProcessed = 0;    ///< Total simulation ticks completed.
        float averageTickMs = 0.0f;          ///< Rolling average tick duration (ms).
        float peakTickMs = 0.0f;             ///< Worst-case tick duration seen (ms).
        uint32_t currentPlayers = 0;         ///< Number of currently connected players.
        uint32_t peakPlayers = 0;            ///< High-water mark for simultaneous players.
        uint64_t totalBytesIn = 0;           ///< Cumulative inbound traffic (bytes).
        uint64_t totalBytesOut = 0;          ///< Cumulative outbound traffic (bytes).
        uint32_t totalConnectionsServed = 0; ///< Lifetime connection count (including disconnected).
        float currentTickRate = 0.0f;        ///< Actual ticks per second (may differ from target).

        // Match state
        std::string currentMap;          ///< Name of the active map (e.g. "de_dust2").
        int currentMapIndex = 0;         ///< Index into the map rotation list.
        float matchTimeRemaining = 0.0f; ///< Seconds remaining in the current match.
        int currentRound = 1;            ///< Current round number (1-based).
    };

    // ============================================================================
    // Administration Command (legacy RCON API name)
    // ============================================================================

    /// @brief A registered administration command (legacy RCON API name).
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
    /// - Trusted in-process administration command registration and dispatch
    /// - LAN broadcast for server discovery
    /// - Server-authoritative match state (score, timer, rounds)
    ///
    /// Thread safety: The server tick loop runs on its own thread when
    /// Start() is used. Trusted host code may dispatch admin commands, kick, and query
    /// methods from any thread — they are protected by internal mutexes.
    class DedicatedServer
    {
      public:
        DedicatedServer();
        explicit DedicatedServer(INetworkRuntime& networkRuntime);
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

        // -- Trusted local administration (legacy RCON API names) --

        /// @brief Register a trusted in-process administration command.
        void RegisterRconCommand(const std::string& name, const std::string& description,
                                 std::function<std::string(const std::vector<std::string>&)> handler);

        /// @brief Execute a trusted in-process RCON command string, e.g. "kick 3 cheating".
        /// Network chat is intentionally not an RCON transport; a future remote
        /// administration channel must authenticate before calling this API.
        /// @return The command response text.
        std::string ExecuteRcon(const std::string& commandLine);

        /// @brief Get all registered administration commands.
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

        /// @brief Register built-in local admin commands (help, status, kick, ban, map, say).
        void RegisterBuiltInRconCommands();
        void RegisterNetworkHandlers();
        void ClearNetworkHandlers();

        /// @brief Log a message to file and callback.
        void Log(const std::string& message);

        /// @brief Parse an administration command string into name + arguments.
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

        // Trusted local administration (legacy RCON API names)
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
        INetworkRuntime* m_networkRuntime = nullptr;
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
