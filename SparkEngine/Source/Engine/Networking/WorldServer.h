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
        ClientID clientId = INVALID_CLIENT;
        std::string playerName;
        AreaID currentArea = INVALID_AREA;
        AreaID pendingArea = INVALID_AREA; ///< Area being transferred to
        XMFLOAT3 lastKnownPosition{0, 0, 0};
        float sessionDuration = 0.0f;
        bool isTransferring = false; ///< Currently being migrated between areas
    };

    // ============================================================================
    // Area Registration
    // ============================================================================

    /**
     * @brief Information about a registered area server
     */
    struct AreaRegistration
    {
        AreaID areaId = INVALID_AREA;
        std::string areaName;
        std::string hostAddress;      ///< Host machine address
        uint16_t port = 0;            ///< Client-facing port
        uint16_t interServerPort = 0; ///< Inter-server port
        int currentClients = 0;
        int maxClients = 64;
        float cpuLoad = 0.0f; ///< CPU usage (0.0 - 1.0)
        size_t memoryUsageMB = 0;
        bool isOnline = false;
        std::chrono::steady_clock::time_point lastHeartbeat;
    };

    // ============================================================================
    // World Server Statistics
    // ============================================================================

    struct WorldServerStats
    {
        float uptimeSeconds = 0.0f;
        uint32_t totalPlayers = 0;
        uint32_t peakPlayers = 0;
        uint32_t activeAreas = 0;
        uint32_t totalAreaTransfers = 0;
        uint32_t failedTransfers = 0;
        float averageAreaLoad = 0.0f;
    };

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
         * @brief Get information about a registered area
         */
        const AreaRegistration* GetAreaInfo(AreaID areaId) const;

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
         * @brief Transfer a player from one area to another
         * @param clientId Client ID
         * @param targetArea Target area ID
         * @return true if transfer was initiated
         */
        bool TransferPlayer(ClientID clientId, AreaID targetArea);

        /**
         * @brief Get a player's current session
         */
        const PlayerSession* GetPlayerSession(ClientID clientId) const;

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
        const WorldServerStats& GetStats() const { return m_stats; }

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

        // Logging
        mutable std::mutex m_logMutex;
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
