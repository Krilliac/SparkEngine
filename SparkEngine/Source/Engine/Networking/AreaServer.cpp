/**
 * @file AreaServer.cpp
 * @brief Area-based server process implementation
 */

#include "AreaServer.h"

#ifdef ENABLE_NETWORKING

#include "../../Utils/LogMacros.h"

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
        ProcessClientMessages(deltaTime);
        UpdateSimulation(deltaTime);
        CheckEntityBoundaries();

        m_stats.totalTicks++;
        auto now = std::chrono::steady_clock::now();
        m_stats.uptimeSeconds = std::chrono::duration<float>(now - m_startTime).count();
    }

    // ============================================================================
    // Entity Migration
    // ============================================================================

    bool AreaServer::AcceptMigratingEntity(const MigratingEntity& entity)
    {
        std::lock_guard<std::mutex> lock(m_migrationMutex);

        SPARK_LOG_INFO("AreaServer", "Area '%s' accepted migrating entity %u from owner %u.", m_config.areaName.c_str(),
                       entity.networkID, entity.ownerID);

        m_stats.entitiesMigratedIn++;

        // In a full implementation, this would deserialize the entity's ECS
        // components and spawn it in the local registry.
        return true;
    }

    bool AreaServer::MigrateEntityOut(uint32_t networkID, AreaID targetAreaId)
    {
        std::lock_guard<std::mutex> lock(m_migrationMutex);

        MigratingEntity migration;
        migration.networkID = networkID;
        migration.timestamp = m_stats.uptimeSeconds;

        // In a full implementation, this would serialize the entity's ECS state
        // and remove it from the local registry.
        m_pendingMigrations.push_back(std::move(migration));
        m_stats.entitiesMigratedOut++;

        SPARK_LOG_INFO("AreaServer", "Area '%s' migrating entity %u to area %u.", m_config.areaName.c_str(), networkID,
                       targetAreaId);
        return true;
    }

    // ============================================================================
    // Cross-Area Communication
    // ============================================================================

    void AreaServer::SendCrossAreaMessage(AreaID targetAreaId, const NetworkMessage& msg)
    {
        // In a full implementation, this would route the message through the
        // WorldServer or directly to the target AreaServer via inter-server port.
        SPARK_LOG_INFO("AreaServer", "Area '%s' -> Area %u: message type %u.", m_config.areaName.c_str(), targetAreaId,
                       static_cast<uint16_t>(msg.type));
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
        std::ostringstream oss;
        oss << "AreaServer '" << m_config.areaName << "' (ID=" << m_config.areaId
            << "): " << (m_running.load() ? "Running" : "Stopped") << " | Clients: " << m_stats.clientCount << "/"
            << m_config.maxClients << " | Entities: " << m_stats.entityCount << " | Ticks: " << m_stats.totalTicks
            << " | Uptime: " << m_stats.uptimeSeconds << "s" << " | Migrated in/out: " << m_stats.entitiesMigratedIn
            << "/" << m_stats.entitiesMigratedOut;
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
            m_stats.averageTickMs = m_stats.averageTickMs * 0.95f + tickMs * 0.05f;
            if (tickMs > m_stats.peakTickMs)
                m_stats.peakTickMs = tickMs;

            if (tickDuration < targetTickDuration)
            {
                std::this_thread::sleep_for(targetTickDuration - tickDuration);
            }
        }
    }

    void AreaServer::ProcessClientMessages(float /*deltaTime*/)
    {
        // In a full implementation, this would process incoming client messages
        // via the area's NetworkManager instance.
    }

    void AreaServer::UpdateSimulation(float /*deltaTime*/)
    {
        // In a full implementation, this would step:
        // 1. Physics (if enabled)
        // 2. AI (if enabled)
        // 3. Scripting (if enabled)
        // 4. ECS systems
        // 5. Entity replication to connected clients
    }

    void AreaServer::CheckEntityBoundaries()
    {
        // In a full implementation, this would check all entities against the
        // area's boundaries. Entities that have crossed into an adjacent area's
        // territory would be queued for migration via MigrateEntityOut().
    }

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
