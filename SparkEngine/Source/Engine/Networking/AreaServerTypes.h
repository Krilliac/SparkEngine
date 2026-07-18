/**
 * @file AreaServerTypes.h
 * @brief Configuration, migration, and statistics types for AreaServer
 * @author Spark Engine Team
 * @date 2026
 *
 * Shared types consumed by AreaServer: area identifiers, per-instance
 * configuration, serialized entity-migration payloads, and runtime
 * performance statistics used for load-balancing decisions.
 *
 * All networking code is guarded by ENABLE_NETWORKING.
 *
 * @see AreaServer.h
 */

#pragma once

#include "../../Core/Platform.h"
#include "NetworkManager.h"

#include <cstdint>
#include <string>
#include <vector>

#ifdef ENABLE_NETWORKING

namespace Spark::Net
{

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

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
