/**
 * @file AreaServer.cpp
 * @brief Area-based server process implementation
 */

#include "AreaServer.h"

#ifdef ENABLE_NETWORKING

#include "IAreaSimulation.h"

#include "../../Core/FaultIsolation.h"
#include "../../Utils/ContainerUtils.h"
#include "../../Utils/LogMacros.h"
#include "../../Utils/Validate.h"

#include <cmath>
#include <sstream>

namespace Spark::Net
{

    // ============================================================================
    // Construction / Destruction
    // ============================================================================

    AreaServer::AreaServer() = default;

    AreaServer::~AreaServer()
    {
        Stop();
    }

    // ============================================================================
    // Lifecycle
    // ============================================================================

    bool AreaServer::Start(const AreaServerConfig& config)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        if (m_running.load(std::memory_order_acquire))
        {
            SPARK_LOG_WARN("AreaServer", "Already running.");
            return false;
        }

        m_config = config;
        m_startTime = std::chrono::steady_clock::now();
        m_stats = {};
        m_running.store(true, std::memory_order_release);

        m_tickThread = std::thread(&AreaServer::TickLoop, this);

        SPARK_LOG_INFO("AreaServer", "Started area '%s' (ID=%u) on port %u.", config.areaName.c_str(), config.areaId,
                       config.port);
        return true;
    }

    void AreaServer::Stop()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        if (!m_running.load(std::memory_order_acquire))
            return;

        SPARK_LOG_INFO("AreaServer", "Stopping area '%s'...", m_config.areaName.c_str());
        m_running.store(false, std::memory_order_release);

        if (m_tickThread.joinable())
        {
            m_tickThread.join();
        }

        SPARK_LOG_INFO("AreaServer", "Area '%s' stopped.", m_config.areaName.c_str());
    }

    void AreaServer::Tick(float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        SPARK_GUARDED_UPDATE("Area:ClientMessages", "Network", { ProcessClientMessages(deltaTime); });
        SPARK_GUARDED_UPDATE("Area:Simulation", "Network", { UpdateSimulation(deltaTime); });
        SPARK_GUARDED_UPDATE("Area:Boundaries", "Network", { CheckEntityBoundaries(); });

        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.totalTicks++;
            auto now = std::chrono::steady_clock::now();
            m_stats.uptimeSeconds = std::chrono::duration<float>(now - m_startTime).count();
        }
    }

    // ============================================================================
    // Entity Migration
    // ============================================================================

    bool AreaServer::AcceptMigratingEntity(const MigratingEntity& entity)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        // Validate networkID
        if (entity.networkID == 0)
        {
            SPARK_LOG_ERROR("AreaServer", "Area '%s' rejected migrating entity: networkID is 0.",
                            m_config.areaName.c_str());
            return false;
        }

        // Validate serialized state is not empty
        if (entity.serializedState.empty())
        {
            SPARK_LOG_ERROR("AreaServer", "Area '%s' rejected migrating entity %u: serializedState is empty.",
                            m_config.areaName.c_str(), entity.networkID);
            return false;
        }

        // Validate position is finite (no NaN or Inf)
        if (!std::isfinite(entity.position.x) || !std::isfinite(entity.position.y) || !std::isfinite(entity.position.z))
        {
            SPARK_LOG_ERROR("AreaServer",
                            "Area '%s' rejected migrating entity %u: position contains non-finite values.",
                            m_config.areaName.c_str(), entity.networkID);
            return false;
        }

        std::lock_guard<std::mutex> lock(m_migrationMutex);

        // Check for duplicate entity
        if (Spark::ContainerUtils::Contains(m_trackedEntities, entity.networkID))
        {
            SPARK_LOG_WARN("AreaServer", "Area '%s' already tracking entity %u, overwriting.",
                           m_config.areaName.c_str(), entity.networkID);
        }

        // Deserialize the entity state from the serialized bytes
        NetBuffer buffer;
        buffer.WriteBytes(entity.serializedState.data(), entity.serializedState.size());

        XMFLOAT3 deserializedPos = buffer.ReadVector3();
        XMFLOAT3 deserializedVel = buffer.ReadVector3();
        std::string entityType = buffer.ReadString();

        if (buffer.HasError())
        {
            SPARK_LOG_ERROR("AreaServer",
                            "Area '%s' failed to deserialize state for migrating entity %u: buffer read error.",
                            m_config.areaName.c_str(), entity.networkID);
            return false;
        }

        // Track the accepted entity
        m_trackedEntities[entity.networkID] = entity;

        {
            std::lock_guard<std::mutex> slock(m_statsMutex);
            m_stats.entitiesMigratedIn++;
            m_stats.entityCount = static_cast<uint32_t>(m_trackedEntities.size());
        }

        SPARK_LOG_INFO("AreaServer",
                       "Area '%s' accepted migrating entity %u (type='%s') from owner %u at position (%.2f, %.2f, "
                       "%.2f), velocity (%.2f, %.2f, %.2f), state size=%zu bytes.",
                       m_config.areaName.c_str(), entity.networkID, entity.entityType.c_str(), entity.ownerID,
                       deserializedPos.x, deserializedPos.y, deserializedPos.z, deserializedVel.x, deserializedVel.y,
                       deserializedVel.z, entity.serializedState.size());

        return true;
    }

    MigratingEntity AreaServer::BuildMigration(uint32_t networkID, const MigratingEntity& tracked) const
    {
        MigratingEntity migration;
        migration.networkID = networkID;
        migration.ownerID = tracked.ownerID;
        migration.entityType = tracked.entityType;
        migration.position = tracked.position;
        migration.velocity = tracked.velocity;
        {
            std::lock_guard<std::mutex> slock(m_statsMutex);
            migration.timestamp = m_stats.uptimeSeconds;
        }

        // Serialize entity position/velocity/type data into serializedState
        NetBuffer buffer;
        buffer.WriteVector3(migration.position);
        buffer.WriteVector3(migration.velocity);
        buffer.WriteString(migration.entityType);
        migration.serializedState = buffer.GetData();
        return migration;
    }

    bool AreaServer::MigrateEntityOut(uint32_t networkID, AreaID targetAreaId)
    {
        // Validate parameters
        if (networkID == 0)
        {
            SPARK_LOG_ERROR("AreaServer", "Area '%s' cannot migrate entity: networkID is 0.",
                            m_config.areaName.c_str());
            return false;
        }

        if (targetAreaId == INVALID_AREA)
        {
            SPARK_LOG_ERROR("AreaServer", "Area '%s' cannot migrate entity %u: target area is INVALID_AREA.",
                            m_config.areaName.c_str(), networkID);
            return false;
        }

        std::lock_guard<std::mutex> lock(m_migrationMutex);

        // Look up the entity in our tracked entities
        auto it = m_trackedEntities.find(networkID);
        if (it == m_trackedEntities.end())
        {
            SPARK_LOG_WARN("AreaServer",
                           "Area '%s' migrating entity %u not found in tracked entities, "
                           "using default state.",
                           m_config.areaName.c_str(), networkID);
        }

        // Bound the pending-migration buffer: refuse new migrations once it is
        // full so a never-drained queue cannot exhaust memory. The entity stays
        // tracked and can migrate on a later attempt.
        if (m_pendingMigrations.size() >= kMaxPendingMigrations)
        {
            SPARK_LOG_WARN("AreaServer",
                           "Area '%s' pending-migration buffer full (%zu) — refusing migration for entity %u.",
                           m_config.areaName.c_str(), m_pendingMigrations.size(), networkID);
            return false;
        }

        // Resolve the source entity data (tracked entry, or defaults if untracked).
        MigratingEntity source;
        if (it != m_trackedEntities.end())
        {
            source.ownerID = it->second.ownerID;
            source.entityType = it->second.entityType;
            source.position = it->second.position;
            source.velocity = it->second.velocity;
        }
        else
        {
            source.ownerID = INVALID_CLIENT;
            source.entityType = "unknown";
            source.position = {0.0f, 0.0f, 0.0f};
            source.velocity = {0.0f, 0.0f, 0.0f};
        }

        MigratingEntity migration = BuildMigration(networkID, source);

        m_pendingMigrations.push_back(std::move(migration));

        // Remove from tracked entities
        if (it != m_trackedEntities.end())
        {
            m_trackedEntities.erase(it);
        }

        {
            std::lock_guard<std::mutex> slock(m_statsMutex);
            m_stats.entitiesMigratedOut++;
            m_stats.entityCount = static_cast<uint32_t>(m_trackedEntities.size());
        }

        SPARK_LOG_INFO(
            "AreaServer", "Area '%s' migrating entity %u to area %u (serialized %zu bytes, owner=%u, type='%s').",
            m_config.areaName.c_str(), networkID, targetAreaId, m_pendingMigrations.back().serializedState.size(),
            m_pendingMigrations.back().ownerID, m_pendingMigrations.back().entityType.c_str());
        return true;
    }

    // ============================================================================
    // Cross-Area Communication
    // ============================================================================

    void AreaServer::SendCrossAreaMessage(AreaID targetAreaId, const NetworkMessage& msg)
    {
        // Validate target area
        if (targetAreaId == INVALID_AREA)
        {
            SPARK_LOG_ERROR("AreaServer", "Area '%s' cannot send cross-area message: target area is INVALID_AREA.",
                            m_config.areaName.c_str());
            return;
        }

        // Validate message type (must not be zero / default-constructed)
        if (static_cast<uint16_t>(msg.type) == 0)
        {
            SPARK_LOG_WARN("AreaServer", "Area '%s' dropping cross-area message to area %u: message type is 0.",
                           m_config.areaName.c_str(), targetAreaId);
            return;
        }

        // If the target is ourselves, dispatch directly to registered handlers
        if (targetAreaId == m_config.areaId)
        {
            std::lock_guard<std::mutex> lock(m_crossAreaMutex);
            auto handlerIt = m_crossAreaHandlers.find(static_cast<uint16_t>(msg.type));
            if (handlerIt != m_crossAreaHandlers.end())
            {
                SPARK_LOG_INFO("AreaServer",
                               "Area '%s' dispatching self-targeted cross-area message type %u to local handler.",
                               m_config.areaName.c_str(), static_cast<uint16_t>(msg.type));
                handlerIt->second(m_config.areaId, msg);
            }
            else
            {
                SPARK_LOG_WARN("AreaServer",
                               "Area '%s' received self-targeted cross-area message type %u but no handler registered.",
                               m_config.areaName.c_str(), static_cast<uint16_t>(msg.type));
            }
            return;
        }

        // Queue the message for delivery to the remote area server
        {
            std::lock_guard<std::mutex> lock(m_crossAreaMutex);
            m_crossAreaMessageQueue.push({targetAreaId, msg});
        }

        SPARK_LOG_INFO(
            "AreaServer", "Area '%s' queued cross-area message to area %u: type=%u, payload=%zu bytes, sender=%u.",
            m_config.areaName.c_str(), targetAreaId, static_cast<uint16_t>(msg.type), msg.payload.size(), msg.senderID);
    }

    void AreaServer::RegisterCrossAreaHandler(MessageType type, CrossAreaHandler handler)
    {
        std::lock_guard<std::mutex> lock(m_crossAreaMutex);
        m_crossAreaHandlers[static_cast<uint16_t>(type)] = std::move(handler);
    }

    // ============================================================================
    // Console
    // ============================================================================

    std::string AreaServer::Console_GetStatus() const
    {
        AreaServerStats snapshot;
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            snapshot = m_stats;
        }

        std::ostringstream oss;
        oss << "AreaServer '" << m_config.areaName << "' (ID=" << m_config.areaId
            << "): " << (m_running.load() ? "Running" : "Stopped") << " | Clients: " << snapshot.clientCount << "/"
            << m_config.maxClients << " | Entities: " << snapshot.entityCount << " | Ticks: " << snapshot.totalTicks
            << " | Uptime: " << snapshot.uptimeSeconds << "s" << " | Migrated in/out: " << snapshot.entitiesMigratedIn
            << "/" << snapshot.entitiesMigratedOut;
        return oss.str();
    }

    // ============================================================================
    // Internal
    // ============================================================================

    void AreaServer::TickLoop()
    {
        auto targetTickDuration = std::chrono::microseconds(static_cast<int64_t>(1000000.0f / m_config.tickRate));

        while (m_running.load(std::memory_order_acquire))
        {
            auto tickStart = std::chrono::steady_clock::now();

            float deltaTime = 1.0f / m_config.tickRate;
            Tick(deltaTime);

            auto tickEnd = std::chrono::steady_clock::now();
            auto tickDuration = std::chrono::duration_cast<std::chrono::microseconds>(tickEnd - tickStart);

            float tickMs = static_cast<float>(tickDuration.count()) / 1000.0f;
            {
                std::lock_guard<std::mutex> lock(m_statsMutex);
                m_stats.averageTickMs = m_stats.averageTickMs * 0.95f + tickMs * 0.05f;
                if (tickMs > m_stats.peakTickMs)
                    m_stats.peakTickMs = tickMs;
            }

            if (tickDuration < targetTickDuration)
            {
                std::this_thread::sleep_for(targetTickDuration - tickDuration);
            }
        }
    }

    void AreaServer::AddClient(ClientID clientId)
    {
        float uptime;
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            uptime = m_stats.uptimeSeconds;
        }

        std::lock_guard<std::mutex> lock(m_clientMutex);
        ClientRecord record;
        record.clientId = clientId;
        record.lastHeartbeatTime = uptime;
        m_connectedClients[clientId] = record;

        {
            std::lock_guard<std::mutex> slock(m_statsMutex);
            m_stats.clientCount = static_cast<uint32_t>(m_connectedClients.size());
        }

        SPARK_LOG_INFO("AreaServer", "Area '%s' added client %u (total: %u/%d).", m_config.areaName.c_str(), clientId,
                       m_stats.clientCount, m_config.maxClients);
    }

    void AreaServer::RemoveClient(ClientID clientId)
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        m_connectedClients.erase(clientId);
        uint32_t count = static_cast<uint32_t>(m_connectedClients.size());

        {
            std::lock_guard<std::mutex> slock(m_statsMutex);
            m_stats.clientCount = count;
        }

        SPARK_LOG_INFO("AreaServer", "Area '%s' removed client %u (total: %u/%d).", m_config.areaName.c_str(), clientId,
                       count, m_config.maxClients);
    }

    uint32_t AreaServer::GetClientCount() const
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        return static_cast<uint32_t>(m_connectedClients.size());
    }

    void AreaServer::ProcessClientMessages(float deltaTime)
    {
        // Check heartbeat timeouts for connected clients
        std::vector<ClientID> timedOut;
        {
            std::lock_guard<std::mutex> lock(m_clientMutex);
            float currentUptime;
            {
                std::lock_guard<std::mutex> slock(m_statsMutex);
                currentUptime = m_stats.uptimeSeconds;
            }

            for (auto& [clientId, record] : m_connectedClients)
            {
                float elapsed = currentUptime - record.lastHeartbeatTime;
                if (elapsed > HEARTBEAT_TIMEOUT)
                {
                    SPARK_LOG_WARN("AreaServer", "Area '%s' client %u timed out (%.1fs since last heartbeat).",
                                   m_config.areaName.c_str(), clientId, elapsed);
                    timedOut.push_back(clientId);
                }
            }

            for (ClientID id : timedOut)
            {
                m_connectedClients.erase(id);
            }
            {
                std::lock_guard<std::mutex> slock(m_statsMutex);
                m_stats.clientCount = static_cast<uint32_t>(m_connectedClients.size());
            }
        }

        // Rate-limited periodic status logging
        AreaServerStats snapshot;
        {
            std::lock_guard<std::mutex> slock(m_statsMutex);
            snapshot = m_stats;
        }
        float currentUptime = snapshot.uptimeSeconds;
        if (currentUptime - m_lastStatusLogTime >= STATUS_LOG_INTERVAL)
        {
            m_lastStatusLogTime = currentUptime;
            SPARK_LOG_INFO("AreaServer",
                           "Area '%s' status: clients=%u/%d, entities=%u, ticks=%llu, avgTick=%.2fms, "
                           "peakTick=%.2fms, migratedIn=%u, migratedOut=%u.",
                           m_config.areaName.c_str(), snapshot.clientCount, m_config.maxClients, snapshot.entityCount,
                           static_cast<unsigned long long>(snapshot.totalTicks), snapshot.averageTickMs,
                           snapshot.peakTickMs, snapshot.entitiesMigratedIn, snapshot.entitiesMigratedOut);
        }
    }

    void AreaServer::UpdateSimulation(float deltaTime)
    {
        auto simStart = std::chrono::steady_clock::now();

        // Apply velocity integration to tracked entities
        {
            std::lock_guard<std::mutex> lock(m_migrationMutex);
            for (auto& [networkID, entity] : m_trackedEntities)
            {
                entity.position.x += entity.velocity.x * deltaTime;
                entity.position.y += entity.velocity.y * deltaTime;
                entity.position.z += entity.velocity.z * deltaTime;
            }
            {
                std::lock_guard<std::mutex> slock(m_statsMutex);
                m_stats.entityCount = static_cast<uint32_t>(m_trackedEntities.size());
            }
        }

        // Game-supplied authoritative simulation (runs on this tick thread —
        // see SetSimulation() for the threading contract).
        if (IAreaSimulation* simulation = m_simulation.load(std::memory_order_acquire))
        {
            simulation->OnAreaTick(deltaTime);
        }

        // Performance budget check
        auto simEnd = std::chrono::steady_clock::now();
        float totalSimMs = std::chrono::duration<float, std::milli>(simEnd - simStart).count();
        float targetMs = 1000.0f / m_config.tickRate;
        if (totalSimMs > targetMs * 0.8f)
        {
            uint32_t entityCount;
            {
                std::lock_guard<std::mutex> slock(m_statsMutex);
                entityCount = m_stats.entityCount;
            }
            SPARK_LOG_WARN(
                "AreaServer", "Area '%s' simulation step took %.2fms (%.0f%% of %.2fms budget), entities=%u.",
                m_config.areaName.c_str(), totalSimMs, (totalSimMs / targetMs) * 100.0f, targetMs, entityCount);
        }
    }

    void AreaServer::CheckEntityBoundaries()
    {
        std::lock_guard<std::mutex> lock(m_migrationMutex);

        // Collect entity IDs that need migration (avoid modifying map while iterating)
        std::vector<uint32_t> entitiesToMigrate;

        for (const auto& [networkID, entity] : m_trackedEntities)
        {
            const XMFLOAT3& pos = entity.position;

            // Check if entity position is outside the area's axis-aligned bounding box
            bool outsideBounds = pos.x < m_boundsMin.x || pos.x > m_boundsMax.x || pos.y < m_boundsMin.y ||
                                 pos.y > m_boundsMax.y || pos.z < m_boundsMin.z || pos.z > m_boundsMax.z;

            if (outsideBounds)
            {
                SPARK_LOG_INFO("AreaServer",
                               "Area '%s' entity %u crossed boundary at (%.2f, %.2f, %.2f). "
                               "Bounds: min(%.1f, %.1f, %.1f) max(%.1f, %.1f, %.1f).",
                               m_config.areaName.c_str(), networkID, pos.x, pos.y, pos.z, m_boundsMin.x, m_boundsMin.y,
                               m_boundsMin.z, m_boundsMax.x, m_boundsMax.y, m_boundsMax.z);
                entitiesToMigrate.push_back(networkID);
            }
        }

        if (entitiesToMigrate.empty())
        {
            return;
        }

        // Process migrations inline while we hold the lock, rather than calling
        // MigrateEntityOut (which would try to re-acquire m_migrationMutex).
        for (uint32_t entityID : entitiesToMigrate)
        {
            auto it = m_trackedEntities.find(entityID);
            if (it == m_trackedEntities.end())
            {
                continue;
            }

            // Bound the pending-migration buffer: if it is full, leave the entity
            // tracked (do not enqueue or erase) so a never-drained queue cannot
            // exhaust memory on the tick thread.
            if (m_pendingMigrations.size() >= kMaxPendingMigrations)
            {
                SPARK_LOG_WARN("AreaServer",
                               "Area '%s' pending-migration buffer full (%zu) — deferring boundary migration "
                               "for entity %u.",
                               m_config.areaName.c_str(), m_pendingMigrations.size(), entityID);
                continue;
            }

            // Determine target area based on which boundary was crossed
            // In a full implementation, this would query a spatial partitioning system
            // or SeamlessAreaManager to find the adjacent area. For now, use a
            // placeholder target area ID based on the area ID + 1.
            AreaID targetArea = m_config.areaId + 1;

            MigratingEntity migration = BuildMigration(entityID, it->second);

            m_pendingMigrations.push_back(std::move(migration));
            m_trackedEntities.erase(it);

            {
                std::lock_guard<std::mutex> slock(m_statsMutex);
                m_stats.entitiesMigratedOut++;
            }

            SPARK_LOG_INFO("AreaServer", "Area '%s' queued boundary migration for entity %u to area %u.",
                           m_config.areaName.c_str(), entityID, targetArea);
        }

        {
            std::lock_guard<std::mutex> slock(m_statsMutex);
            m_stats.entityCount = static_cast<uint32_t>(m_trackedEntities.size());
        }
    }

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
