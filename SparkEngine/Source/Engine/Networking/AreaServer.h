/**
 * @file AreaServer.h
 * @brief Area-based server process for scalable multiplayer worlds
 * @author Spark Engine Team
 * @date 2026
 *
 * Inspired by HeroEngine's Area Server architecture, each AreaServer manages
 * a single world area — running its own ECS, physics, AI, and scripting
 * simulation. Multiple AreaServer instances are coordinated by a WorldServer,
 * enabling the engine to support large persistent worlds and MMO-scale
 * multiplayer beyond the single-process DedicatedServer model.
 *
 * Key features:
 * - **Self-contained simulation**: Each area server runs a full game loop
 *   (physics, AI, ECS, scripting) for its area's entities.
 * - **Entity migration**: Entities crossing area boundaries are serialized
 *   and transferred to the adjacent AreaServer.
 * - **Dynamic load balancing**: The WorldServer can reassign areas to
 *   different physical machines based on CPU/memory load.
 * - **Cross-area messaging**: AreaServers can send messages to each other
 *   for interactions spanning area boundaries (e.g., projectiles, voice).
 *
 * ## Relationship to existing systems
 * - Extends `DedicatedServer` concepts but is scoped to a single area
 * - Uses `NetworkManager` for communication with clients and other servers
 * - Integrates with `SeamlessAreaManager` for area definitions
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
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef ENABLE_NETWORKING

namespace Spark::Net
{

    class IAreaSimulation;

    // ============================================================================
    // Area Server Configuration
    // ============================================================================

    using AreaID = uint32_t;
    constexpr AreaID INVALID_AREA = 0;

    /**
     * @brief Configuration for an individual Area Server instance
     */
    struct AreaServerConfig
    {
        AreaID areaId = INVALID_AREA;
        std::string areaName;         ///< Human-readable area name
        std::string scenePath;        ///< Path to the area's scene file
        uint16_t port = 0;            ///< Port for client connections (0 = auto)
        uint16_t interServerPort = 0; ///< Port for inter-server communication
        float tickRate = 60.0f;       ///< Simulation ticks per second
        int maxClients = 64;          ///< Maximum clients in this area
        bool enableAI = true;         ///< Run AI simulation
        bool enablePhysics = true;    ///< Run physics simulation
        bool enableScripting = true;  ///< Run AngelScript simulation
    };

    // ============================================================================
    // Entity Migration
    // ============================================================================

    /**
     * @brief Serialized entity data for cross-area migration
     */
    struct MigratingEntity
    {
        uint32_t networkID = 0;               ///< Network ID to preserve across the migration.
        ClientID ownerID = INVALID_CLIENT;    ///< Client that owns/controls this entity.
        std::string entityType;               ///< Type name for re-spawning on the destination server.
        std::vector<uint8_t> serializedState; ///< Serialized ECS components (full snapshot).
        XMFLOAT3 position{0, 0, 0};           ///< World-space position at the moment of migration.
        XMFLOAT3 velocity{0, 0, 0};           ///< Velocity for seamless motion continuity.
        float timestamp = 0.0f;               ///< Server time when migration was initiated.
    };

    // ============================================================================
    // Area Server Statistics
    // ============================================================================

    /// @brief Runtime performance metrics for a single area server instance.
    struct AreaServerStats
    {
        float uptimeSeconds = 0.0f;       ///< Seconds since this area server started.
        uint64_t totalTicks = 0;          ///< Total simulation ticks completed.
        float averageTickMs = 0.0f;       ///< Rolling average tick duration (ms).
        float peakTickMs = 0.0f;          ///< Worst-case tick duration seen (ms).
        uint32_t entityCount = 0;         ///< Current number of entities in this area.
        uint32_t clientCount = 0;         ///< Current number of connected clients.
        size_t memoryUsageMB = 0;         ///< Estimated memory usage (MB).
        float cpuUsagePercent = 0.0f;     ///< CPU load [0, 100] for load-balancing decisions.
        uint32_t entitiesMigratedIn = 0;  ///< Entities received from other area servers.
        uint32_t entitiesMigratedOut = 0; ///< Entities sent to other area servers.
    };

    // ============================================================================
    // Area Server
    // ============================================================================

    /**
     * @brief Manages a single world area's simulation and client connections
     *
     * An AreaServer is a lightweight server process that runs a complete game
     * simulation for one area of the world. It handles:
     * - Client connections for players in the area
     * - Entity simulation (physics, AI, scripting)
     * - Entity migration to/from adjacent area servers
     * - Cross-area messaging for border interactions
     */
    class AreaServer
    {
      public:
        AreaServer();
        ~AreaServer();

        // Non-copyable
        AreaServer(const AreaServer&) = delete;
        AreaServer& operator=(const AreaServer&) = delete;

        // -- Lifecycle --

        /**
         * @brief Start the area server with the given configuration
         * @return true on success
         */
        bool Start(const AreaServerConfig& config);

        /**
         * @brief Stop the area server gracefully
         */
        void Stop();

        /**
         * @brief Run a single simulation tick (for external driving)
         */
        void Tick(float deltaTime);

        /**
         * @brief Check if the area server is running
         */
        bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

        // -- Simulation Plug-in --

        /**
         * @brief Attach a game-supplied authoritative simulation to this area.
         *
         * The built-in UpdateSimulation() only performs placeholder velocity
         * integration; game modules supply real per-area simulation by
         * implementing IAreaSimulation and attaching it here. The simulation
         * is invoked once per tick, after the built-in velocity integration.
         *
         * @param sim Non-owning pointer; pass nullptr to detach. The caller
         *        must keep the simulation alive until it is detached or the
         *        server is stopped.
         *
         * Threading contract: IAreaSimulation::OnAreaTick() is called on the
         * AreaServer tick thread — the thread spawned by Start(), or whichever
         * thread drives Tick() externally. SetSimulation() itself may be
         * called from any thread (the pointer is published atomically), but
         * after detaching, an OnAreaTick() already in flight on the tick
         * thread may still complete — do not destroy the simulation object
         * until the server is stopped or a subsequent tick has been observed.
         */
        void SetSimulation(IAreaSimulation* sim) { m_simulation.store(sim, std::memory_order_release); }

        // -- Entity Migration --

        /**
         * @brief Accept a migrating entity from another area server
         * @param entity Serialized entity data
         * @return true if the entity was accepted
         */
        bool AcceptMigratingEntity(const MigratingEntity& entity);

        /**
         * @brief Migrate an entity out to another area server
         * @param networkID Entity network ID
         * @param targetAreaId Target area server ID
         * @return true if migration was initiated
         */
        bool MigrateEntityOut(uint32_t networkID, AreaID targetAreaId);

        /**
         * @brief Get entities pending migration
         */
        const std::vector<MigratingEntity>& GetPendingMigrations() const { return m_pendingMigrations; }

        /**
         * @brief Clear pending migrations after they've been processed
         */
        void ClearPendingMigrations() { m_pendingMigrations.clear(); }

        // -- Cross-Area Communication --

        /**
         * @brief Send a message to another area server
         * @param targetAreaId Target area ID
         * @param msg Message to send
         */
        void SendCrossAreaMessage(AreaID targetAreaId, const NetworkMessage& msg);

        /**
         * @brief Register handler for cross-area messages
         */
        using CrossAreaHandler = std::function<void(AreaID sourceArea, const NetworkMessage&)>;
        void RegisterCrossAreaHandler(MessageType type, CrossAreaHandler handler);

        // -- Queries --

        /// Register a client as present in this area
        void AddClient(ClientID clientId);

        /// Remove a client from this area
        void RemoveClient(ClientID clientId);

        /// Get count of connected clients
        uint32_t GetClientCount() const;

        const AreaServerConfig& GetConfig() const { return m_config; }
        AreaServerStats GetStats() const
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            return m_stats;
        }
        AreaID GetAreaID() const { return m_config.areaId; }
        const std::string& GetAreaName() const { return m_config.areaName; }

        /// Console status string
        std::string Console_GetStatus() const;

      private:
        void TickLoop();
        void ProcessClientMessages(float deltaTime);
        void UpdateSimulation(float deltaTime);
        void CheckEntityBoundaries();

        AreaServerConfig m_config;
        AreaServerStats m_stats;

        /// Game-supplied simulation hook (non-owning; see SetSimulation()).
        std::atomic<IAreaSimulation*> m_simulation{nullptr};
        mutable std::mutex m_statsMutex; ///< Guards m_stats (written by tick thread, read elsewhere)
        std::atomic<bool> m_running{false};
        std::thread m_tickThread;
        std::chrono::steady_clock::time_point m_startTime;

        // Entity migration
        std::vector<MigratingEntity> m_pendingMigrations;
        mutable std::mutex m_migrationMutex;

        // Cross-area messaging
        std::unordered_map<uint16_t, CrossAreaHandler> m_crossAreaHandlers;
        std::queue<std::pair<AreaID, NetworkMessage>> m_crossAreaMessageQueue;
        mutable std::mutex m_crossAreaMutex;

        // Entity tracking
        std::unordered_map<uint32_t, MigratingEntity> m_trackedEntities;

        // Client tracking for heartbeat timeouts
        struct ClientRecord
        {
            ClientID clientId = INVALID_CLIENT;
            float lastHeartbeatTime = 0.0f;
        };
        std::unordered_map<ClientID, ClientRecord> m_connectedClients;
        mutable std::mutex m_clientMutex;

        // Area bounds (axis-aligned bounding box)
        XMFLOAT3 m_boundsMin{-500.0f, -500.0f, -500.0f};
        XMFLOAT3 m_boundsMax{500.0f, 500.0f, 500.0f};

        // Rate-limited logging
        float m_lastStatusLogTime = 0.0f;
        static constexpr float STATUS_LOG_INTERVAL = 10.0f;

        // Heartbeat timeout
        static constexpr float HEARTBEAT_TIMEOUT = 30.0f;
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
