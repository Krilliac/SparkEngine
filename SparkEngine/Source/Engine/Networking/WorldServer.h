/**
 * @file WorldServer.h
 * @brief Central coordinator for area-based multiplayer world architecture
 * @author Spark Engine Team
 * @date 2026
 *
 * Inspired by HeroEngine's World Server, this class orchestrates multiple
 * AreaServer instances to form a seamless, scalable multiplayer world.
 * The WorldServer is responsible for:
 *
 * - Spawning and managing AreaServer processes/instances
 * - Routing player connections to the appropriate AreaServer
 * - Coordinating entity migration between AreaServers
 * - Dynamic load balancing across physical machines
 * - Global world state (time of day, weather, events)
 * - Player session management (login, area transfers, disconnect)
 *
 * ## Architecture
 * ```
 *   [Client] ──connect──> [WorldServer] ──route──> [AreaServer A]
 *                                                   [AreaServer B]
 *                                                   [AreaServer C]
 * ```
 *
 * All networking code is guarded by ENABLE_NETWORKING.
 */

#pragma once

#include "../../Core/Platform.h"
#include "AreaServer.h"
#include "NetworkManager.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef ENABLE_NETWORKING

namespace Spark::Net
{

    // ============================================================================
    // World Server Configuration
    // ============================================================================

    /**
     * @brief Configuration for the World Server
     */
    struct WorldServerConfig
    {
        std::string worldName = "SparkWorld";
        uint16_t port = 27020;             ///< Port for client connections
        uint16_t interServerPort = 27021;  ///< Port for AreaServer communication
        int maxTotalClients = 1000;        ///< Max clients across all areas
        float tickRate = 10.0f;            ///< WorldServer tick rate (lower than area servers)
        bool enableLoadBalancing = true;   ///< Dynamic area reassignment
        float loadBalanceInterval = 30.0f; ///< Seconds between load balance checks
        std::string logFilePath = "world_server.log";
    };

    // ============================================================================
    // Player Session
    // ============================================================================

    /**
     * @brief Tracks a player's session across area transitions
     */
    struct PlayerSession
    {
        ClientID clientId = INVALID_CLIENT;  ///< Unique client identifier.
        std::string playerName;              ///< Display name for logging and admin tools.
        AreaID currentArea = INVALID_AREA;   ///< Area the player is currently simulated in.
        AreaID pendingArea = INVALID_AREA;   ///< Area being transferred to (INVALID if not migrating).
        XMFLOAT3 lastKnownPosition{0, 0, 0}; ///< Most recent position (for proximity checks).
        float sessionDuration = 0.0f;        ///< Total time connected this session (seconds).
        bool isTransferring = false;         ///< True while migrating between area servers.
    };

    // ============================================================================
    // Area Registration
    // ============================================================================

    /**
     * @brief Information about a registered area server
     */
    struct AreaRegistration
    {
        AreaID areaId = INVALID_AREA;                        ///< Unique area identifier.
        std::string areaName;                                ///< Human-readable area name (for logging and admin).
        std::string hostAddress;                             ///< Host machine address (IP or hostname).
        uint16_t port = 0;                                   ///< Client-facing UDP port.
        uint16_t interServerPort = 0;                        ///< Inter-server communication port.
        int currentClients = 0;                              ///< Number of players currently in this area.
        int maxClients = 64;                                 ///< Maximum concurrent players for this area.
        float cpuLoad = 0.0f;                                ///< CPU usage [0, 1] for load-balancing decisions.
        size_t memoryUsageMB = 0;                            ///< Reported memory usage (MB).
        bool isOnline = false;                               ///< True while the area server is accepting connections.
        std::chrono::steady_clock::time_point lastHeartbeat; ///< Time of most recent heartbeat (for timeout detection).
        XMFLOAT3 areaPosition{0.0f, 0.0f, 0.0f};             ///< World-space origin of the area region
        XMFLOAT3 areaSize{1000.0f, 1000.0f, 1000.0f};        ///< Extent of the area region
    };

    // ============================================================================
    // World Server Statistics
    // ============================================================================

    /// @brief Aggregate metrics across all area servers managed by the world server.
    struct WorldServerStats
    {
        float uptimeSeconds = 0.0f;          ///< Seconds since the world server started.
        uint32_t totalPlayers = 0;           ///< Current total players across all areas.
        uint32_t peakPlayers = 0;            ///< High-water mark for simultaneous players.
        uint32_t activeAreas = 0;            ///< Number of area servers currently online.
        uint32_t totalAreaTransfers = 0;     ///< Successful player migrations between areas.
        uint32_t failedTransfers = 0;        ///< Failed migration attempts.
        float averageAreaLoad = 0.0f;        ///< Mean CPU load across all area servers [0, 1].
        uint32_t loadBalanceEvents = 0;      ///< Number of load balance passes performed
        uint32_t totalEntityMigrations = 0;  ///< Number of entity migrations completed
        uint32_t totalMessagesProcessed = 0; ///< Total world messages processed
    };

    /// Load thresholds for area balancing
    constexpr float LOAD_BALANCE_OVERLOAD_THRESHOLD = 0.8f;
    constexpr float LOAD_BALANCE_UNDERLOAD_THRESHOLD = 0.3f;

    // ============================================================================
    // World Server
    // ============================================================================

    /**
     * @brief Central coordinator for a multi-area multiplayer world
     *
     * The WorldServer manages the lifecycle of AreaServers and routes
     * players between them as they move through the game world. It maintains
     * global world state and performs load balancing.
     */
    class WorldServer
    {
      public:
        WorldServer();
        ~WorldServer();

        // Non-copyable
        WorldServer(const WorldServer&) = delete;
        WorldServer& operator=(const WorldServer&) = delete;

        // -- Lifecycle --

        /**
         * @brief Start the world server
         * @return true on success
         */
        bool Start(const WorldServerConfig& config);

        /**
         * @brief Stop the world server and all managed area servers
         */
        void Stop();

        /**
         * @brief Check if the world server is running
         */
        bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

        /**
         * @brief Run a single world server tick (for external driving)
         */
        void Tick(float deltaTime);

        /** Update one registered area's externally verified availability. */
        bool SetAreaOnline(AreaID areaId, bool online);

        // -- Area Management --

        /**
         * @brief Register an area server with the world
         * @param config Area server configuration
         * @return Assigned AreaID, or INVALID_AREA on failure
         */
        AreaID RegisterAreaServer(const AreaServerConfig& config);

        /**
         * @brief Unregister an area server
         * @param areaId Area to unregister
         */
        void UnregisterAreaServer(AreaID areaId);

        /**
         * @brief Get an owned snapshot of a registered area
         *
         * The returned value remains valid after the area is updated or
         * unregistered. No reference or pointer into the internal map escapes.
         */
        std::optional<AreaRegistration> GetAreaInfoSnapshot(AreaID areaId) const;

        /**
         * @brief Get all registered areas
         */
        std::vector<AreaRegistration> GetAllAreas() const;

        /**
         * @brief Get the recommended area for a world position
         * @param position World position
         * @return AreaID of the area containing that position
         */
        AreaID GetAreaForPosition(const XMFLOAT3& position) const;

        // -- Player Management --

        /**
         * @brief Handle a new player connection
         * @param clientId Client ID
         * @param playerName Player name
         * @param spawnPosition Requested spawn position
         * @return AreaID the player is assigned to
         */
        AreaID HandlePlayerConnect(ClientID clientId, const std::string& playerName, const XMFLOAT3& spawnPosition);

        /**
         * @brief Handle player disconnection
         * @param clientId Client ID
         */
        void HandlePlayerDisconnect(ClientID clientId);

        /**
         * @brief Update a player's last-known world position.
         *
         * Also refreshes the ConnectionScopeFilter visibility sphere (via
         * NetworkManager::SetClientScope) so entity replication filters the
         * connection's interest set by this position. Call each time the
         * area server reports new player coordinates.
         *
         * @param clientId        Client ID.
         * @param position        New world-space position.
         * @param interestRadius  Visibility radius for replication scoping (world units).
         * @return true if the player session exists and was updated.
         */
        bool UpdatePlayerPosition(ClientID clientId, const XMFLOAT3& position, float interestRadius = 500.0f);

        /**
         * @brief Transfer a player from one area to another
         * @param clientId Client ID
         * @param targetArea Target area ID
         * @return true if transfer was initiated
         */
        bool TransferPlayer(ClientID clientId, AreaID targetArea);

        /**
         * @brief Get an owned snapshot of a player's current session
         *
         * The returned value remains valid after the session is updated or
         * removed. No reference or pointer into the internal map escapes.
         */
        std::optional<PlayerSession> GetPlayerSessionSnapshot(ClientID clientId) const;

        /**
         * @brief Get total player count across all areas
         */
        uint32_t GetTotalPlayerCount() const;

        // -- Load Balancing --

        /**
         * @brief Perform a load balancing pass
         *
         * Evaluates area server loads and may reassign areas to different
         * physical machines if imbalanced.
         */
        void PerformLoadBalancing();

        // -- Global State --

        /**
         * @brief Broadcast a message to all area servers
         */
        void BroadcastToAllAreas(const NetworkMessage& msg);

        /**
         * @brief Broadcast a message to all connected players
         */
        void BroadcastToAllPlayers(const NetworkMessage& msg);

        // -- Queries --

        const WorldServerConfig& GetConfig() const { return m_config; }

        /**
         * @brief Get a snapshot copy of the world server statistics.
         *
         * Returns by value under m_statsMutex rather than handing out a reference
         * to m_stats, which the tick thread mutates (Tick / ProcessEntityMigrations)
         * — a live reference would expose torn reads to console/other threads.
         */
        WorldServerStats GetStats() const
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            return m_stats;
        }

        /// Console status string
        std::string Console_GetStatus() const;

      private:
        void TickLoop();
        void ProcessWorldMessages(float deltaTime);
        void ProcessAreaHeartbeats();
        void UpdatePlayerSessions(float deltaTime);
        void ProcessEntityMigrations();
        void Log(const std::string& message);

        AreaID AllocateAreaID();

        WorldServerConfig m_config;
        WorldServerStats m_stats;
        /// Guards every read and write of m_stats. When a map count and its mirrored
        /// statistic are updated atomically, acquire the owning map mutex first and
        /// m_statsMutex second; no code acquires those mutexes in reverse order.
        mutable std::mutex m_statsMutex;
        std::atomic<bool> m_running{false};
        std::thread m_tickThread;
        std::chrono::steady_clock::time_point m_startTime;

        // Areas
        std::unordered_map<AreaID, AreaRegistration> m_areas;
        AreaID m_nextAreaId = 1;
        mutable std::mutex m_areaMutex;

        // Players
        std::unordered_map<ClientID, PlayerSession> m_playerSessions;
        mutable std::mutex m_playerMutex;

        // Load balancing
        float m_loadBalanceTimer = 0.0f;

        // Message queue
        std::queue<NetworkMessage> m_worldMessageQueue;
        mutable std::mutex m_messageMutex;
        float m_messageLogTimer = 0.0f; ///< Timer for periodic message processing stats

        // Logging
        mutable std::mutex m_logMutex;
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
