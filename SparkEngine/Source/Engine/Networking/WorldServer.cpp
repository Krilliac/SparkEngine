/**
 * @file WorldServer.cpp
 * @brief World server coordinator implementation
 */

#include "WorldServer.h"

#ifdef ENABLE_NETWORKING

#include "../../Utils/LogMacros.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace Spark::Net
{

    // ============================================================================
    // Construction / Destruction
    // ============================================================================

    WorldServer::WorldServer() = default;

    WorldServer::~WorldServer()
    {
        Stop();
    }

    // ============================================================================
    // Lifecycle
    // ============================================================================

    bool WorldServer::Start(const WorldServerConfig& config)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        if (m_running.load(std::memory_order_acquire))
        {
            SPARK_LOG_WARN("WorldServer", "Already running.");
            return false;
        }

        m_config = config;
        m_startTime = std::chrono::steady_clock::now();
        m_stats = {};
        m_running.store(true, std::memory_order_release);

        m_tickThread = std::thread(&WorldServer::TickLoop, this);

        Log("World server '" + config.worldName + "' started on port " + std::to_string(config.port) + ".");
        return true;
    }

    void WorldServer::Stop()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        if (!m_running.load(std::memory_order_acquire))
            return;

        Log("Stopping world server '" + m_config.worldName + "'...");
        m_running.store(false, std::memory_order_release);

        if (m_tickThread.joinable())
        {
            m_tickThread.join();
        }

        {
            std::lock_guard<std::mutex> lock(m_areaMutex);
            m_areas.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_playerMutex);
            m_playerSessions.clear();
        }

        Log("World server stopped.");
    }

    void WorldServer::Tick(float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        ProcessWorldMessages(deltaTime);
        ProcessAreaHeartbeats();
        UpdatePlayerSessions(deltaTime);
        ProcessEntityMigrations();

        // Load balancing
        if (m_config.enableLoadBalancing)
        {
            m_loadBalanceTimer += deltaTime;
            if (m_loadBalanceTimer >= m_config.loadBalanceInterval)
            {
                m_loadBalanceTimer = 0.0f;
                PerformLoadBalancing();
            }
        }

        auto now = std::chrono::steady_clock::now();
        m_stats.uptimeSeconds = std::chrono::duration<float>(now - m_startTime).count();
    }

    // ============================================================================
    // Area Management
    // ============================================================================

    AreaID WorldServer::RegisterAreaServer(const AreaServerConfig& config)
    {
        std::lock_guard<std::mutex> lock(m_areaMutex);

        AreaID id = AllocateAreaID();
        AreaRegistration reg;
        reg.areaId = id;
        reg.areaName = config.areaName;
        reg.port = config.port;
        reg.interServerPort = config.interServerPort;
        reg.maxClients = config.maxClients;
        reg.isOnline = true;
        reg.lastHeartbeat = std::chrono::steady_clock::now();

        m_areas[id] = reg;
        m_stats.activeAreas = static_cast<uint32_t>(m_areas.size());

        Log("Registered area server '" + config.areaName + "' (ID=" + std::to_string(id) + ").");
        return id;
    }

    void WorldServer::UnregisterAreaServer(AreaID areaId)
    {
        std::lock_guard<std::mutex> lock(m_areaMutex);
        auto it = m_areas.find(areaId);
        if (it != m_areas.end())
        {
            Log("Unregistered area server '" + it->second.areaName + "'.");
            m_areas.erase(it);
            m_stats.activeAreas = static_cast<uint32_t>(m_areas.size());
        }
    }

    const AreaRegistration* WorldServer::GetAreaInfo(AreaID areaId) const
    {
        std::lock_guard<std::mutex> lock(m_areaMutex);
        auto it = m_areas.find(areaId);
        return it != m_areas.end() ? &it->second : nullptr;
    }

    std::vector<AreaRegistration> WorldServer::GetAllAreas() const
    {
        std::lock_guard<std::mutex> lock(m_areaMutex);
        std::vector<AreaRegistration> result;
        result.reserve(m_areas.size());
        for (const auto& [id, reg] : m_areas)
        {
            result.push_back(reg);
        }
        return result;
    }

    AreaID WorldServer::GetAreaForPosition(const XMFLOAT3& position) const
    {
        // Validate that position values are finite
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
        {
            SPARK_LOG_ERROR("WorldServer", "GetAreaForPosition called with non-finite position (%.2f, %.2f, %.2f).",
                            position.x, position.y, position.z);
            return INVALID_AREA;
        }

        std::lock_guard<std::mutex> lock(m_areaMutex);

        // First pass: find an online area whose bounds contain the position
        for (const auto& [id, reg] : m_areas)
        {
            if (!reg.isOnline)
            {
                continue;
            }

            float minX = reg.areaPosition.x;
            float minY = reg.areaPosition.y;
            float minZ = reg.areaPosition.z;
            float maxX = minX + reg.areaSize.x;
            float maxY = minY + reg.areaSize.y;
            float maxZ = minZ + reg.areaSize.z;

            if (position.x >= minX && position.x <= maxX && position.y >= minY && position.y <= maxY &&
                position.z >= minZ && position.z <= maxZ)
            {
                return id;
            }
        }

        // Second pass: fall back to the nearest online area by center distance
        AreaID nearestArea = INVALID_AREA;
        float nearestDistSq = std::numeric_limits<float>::max();

        for (const auto& [id, reg] : m_areas)
        {
            if (!reg.isOnline)
            {
                continue;
            }

            float centerX = reg.areaPosition.x + reg.areaSize.x * 0.5f;
            float centerY = reg.areaPosition.y + reg.areaSize.y * 0.5f;
            float centerZ = reg.areaPosition.z + reg.areaSize.z * 0.5f;

            float dx = position.x - centerX;
            float dy = position.y - centerY;
            float dz = position.z - centerZ;
            float distSq = dx * dx + dy * dy + dz * dz;

            if (distSq < nearestDistSq)
            {
                nearestDistSq = distSq;
                nearestArea = id;
            }
        }

        if (nearestArea == INVALID_AREA)
        {
            SPARK_LOG_WARN("WorldServer", "No online area found for position (%.2f, %.2f, %.2f).", position.x,
                           position.y, position.z);
        }
        else
        {
            SPARK_LOG_INFO("WorldServer",
                           "Position (%.2f, %.2f, %.2f) not within any area bounds; "
                           "falling back to nearest area %u.",
                           position.x, position.y, position.z, nearestArea);
        }

        return nearestArea;
    }

    // ============================================================================
    // Player Management
    // ============================================================================

    AreaID WorldServer::HandlePlayerConnect(ClientID clientId, const std::string& playerName,
                                            const XMFLOAT3& spawnPosition)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        SPARK_WARN_IF(Spark::LogCategory::Network, playerName.empty(),
                      "HandlePlayerConnect called with empty playerName");
        AreaID targetArea = GetAreaForPosition(spawnPosition);
        if (targetArea == INVALID_AREA)
        {
            Log("No area available for player '" + playerName + "' at spawn position.");
            return INVALID_AREA;
        }

        {
            std::lock_guard<std::mutex> lock(m_playerMutex);
            PlayerSession session;
            session.clientId = clientId;
            session.playerName = playerName;
            session.currentArea = targetArea;
            session.lastKnownPosition = spawnPosition;
            m_playerSessions[clientId] = session;
        }

        m_stats.totalPlayers = GetTotalPlayerCount();
        if (m_stats.totalPlayers > m_stats.peakPlayers)
        {
            m_stats.peakPlayers = m_stats.totalPlayers;
        }

        Log("Player '" + playerName + "' connected, assigned to area " + std::to_string(targetArea) + ".");
        return targetArea;
    }

    void WorldServer::HandlePlayerDisconnect(ClientID clientId)
    {
        std::lock_guard<std::mutex> lock(m_playerMutex);
        auto it = m_playerSessions.find(clientId);
        if (it != m_playerSessions.end())
        {
            Log("Player '" + it->second.playerName + "' disconnected.");
            m_playerSessions.erase(it);
        }
        m_stats.totalPlayers = static_cast<uint32_t>(m_playerSessions.size());
    }

    bool WorldServer::TransferPlayer(ClientID clientId, AreaID targetArea)
    {
        std::lock_guard<std::mutex> lock(m_playerMutex);
        auto it = m_playerSessions.find(clientId);
        if (it == m_playerSessions.end())
        {
            return false;
        }

        auto& session = it->second;
        if (session.isTransferring)
        {
            Log("Player '" + session.playerName + "' is already transferring.");
            return false;
        }

        session.pendingArea = targetArea;
        session.isTransferring = true;
        m_stats.totalAreaTransfers++;

        Log("Transferring player '" + session.playerName + "' from area " + std::to_string(session.currentArea) +
            " to area " + std::to_string(targetArea) + ".");
        return true;
    }

    const PlayerSession* WorldServer::GetPlayerSession(ClientID clientId) const
    {
        std::lock_guard<std::mutex> lock(m_playerMutex);
        auto it = m_playerSessions.find(clientId);
        return it != m_playerSessions.end() ? &it->second : nullptr;
    }

    uint32_t WorldServer::GetTotalPlayerCount() const
    {
        std::lock_guard<std::mutex> lock(m_playerMutex);
        return static_cast<uint32_t>(m_playerSessions.size());
    }

    // ============================================================================
    // Load Balancing
    // ============================================================================

    void WorldServer::PerformLoadBalancing()
    {
        std::lock_guard<std::mutex> lock(m_areaMutex);

        if (m_areas.empty())
        {
            return;
        }

        // Calculate average load
        float totalLoad = 0.0f;
        for (const auto& [id, reg] : m_areas)
        {
            totalLoad += reg.cpuLoad;
        }
        m_stats.averageAreaLoad = totalLoad / static_cast<float>(m_areas.size());
        m_stats.loadBalanceEvents++;

        // Identify overloaded and underloaded areas
        std::vector<AreaID> overloaded;
        std::vector<AreaID> underloaded;

        for (const auto& [id, reg] : m_areas)
        {
            if (!reg.isOnline)
            {
                continue;
            }

            if (reg.cpuLoad > LOAD_BALANCE_OVERLOAD_THRESHOLD)
            {
                overloaded.push_back(id);
                SPARK_LOG_WARN("WorldServer", "Area '%s' (ID=%u) is overloaded: CPU=%.1f%%.", reg.areaName.c_str(), id,
                               reg.cpuLoad * 100.0f);
            }
            else if (reg.cpuLoad < LOAD_BALANCE_UNDERLOAD_THRESHOLD)
            {
                underloaded.push_back(id);
                SPARK_LOG_INFO("WorldServer", "Area '%s' (ID=%u) is underloaded: CPU=%.1f%%.", reg.areaName.c_str(), id,
                               reg.cpuLoad * 100.0f);
            }
        }

        // Plan migrations from overloaded to underloaded areas
        if (!overloaded.empty() && !underloaded.empty())
        {
            size_t migrationCount = std::min(overloaded.size(), underloaded.size());
            for (size_t i = 0; i < migrationCount; ++i)
            {
                const auto& srcReg = m_areas[overloaded[i]];
                const auto& dstReg = m_areas[underloaded[i]];
                SPARK_LOG_INFO("WorldServer",
                               "Load balance: would migrate work from '%s' (CPU=%.1f%%) "
                               "to '%s' (CPU=%.1f%%).",
                               srcReg.areaName.c_str(), srcReg.cpuLoad * 100.0f, dstReg.areaName.c_str(),
                               dstReg.cpuLoad * 100.0f);
            }
        }
        else if (!overloaded.empty())
        {
            SPARK_LOG_WARN("WorldServer", "Load balance: %zu overloaded area(s) but no underloaded targets available.",
                           overloaded.size());
        }

        SPARK_LOG_INFO("WorldServer", "Load balance pass #%u complete: avg=%.1f%%, overloaded=%zu, underloaded=%zu.",
                       m_stats.loadBalanceEvents, m_stats.averageAreaLoad * 100.0f, overloaded.size(),
                       underloaded.size());
    }

    // ============================================================================
    // Broadcast
    // ============================================================================

    void WorldServer::BroadcastToAllAreas(const NetworkMessage& msg)
    {
        if (msg.payload.empty() && msg.type == MessageType::UserDefined)
        {
            SPARK_LOG_WARN("WorldServer", "BroadcastToAllAreas called with empty user-defined message; ignoring.");
            return;
        }

        std::lock_guard<std::mutex> lock(m_areaMutex);

        uint32_t queuedCount = 0;
        for (auto& [id, reg] : m_areas)
        {
            if (!reg.isOnline)
            {
                continue;
            }

            // Queue the message for this area
            {
                std::lock_guard<std::mutex> msgLock(m_messageMutex);
                NetworkMessage areaCopy = msg;
                areaCopy.senderID = INVALID_CLIENT; // Indicate world-server origin
                m_worldMessageQueue.push(std::move(areaCopy));
            }
            ++queuedCount;
        }

        SPARK_LOG_INFO("WorldServer", "Broadcast message (type=%u) queued for %u online area(s).",
                       static_cast<unsigned>(msg.type), queuedCount);
    }

    void WorldServer::BroadcastToAllPlayers(const NetworkMessage& msg)
    {
        if (msg.payload.empty() && msg.type == MessageType::UserDefined)
        {
            SPARK_LOG_WARN("WorldServer", "BroadcastToAllPlayers called with empty user-defined message; ignoring.");
            return;
        }

        std::lock_guard<std::mutex> playerLock(m_playerMutex);

        if (m_playerSessions.empty())
        {
            SPARK_LOG_INFO("WorldServer", "BroadcastToAllPlayers: no players connected; nothing to send.");
            return;
        }

        uint32_t routedCount = 0;
        for (const auto& [clientId, session] : m_playerSessions)
        {
            if (session.currentArea == INVALID_AREA)
            {
                SPARK_LOG_WARN("WorldServer", "Player '%s' (client=%u) has no valid area; skipping broadcast.",
                               session.playerName.c_str(), clientId);
                continue;
            }

            // Route through the player's current area server by queuing a copy
            // addressed to this specific client.
            {
                std::lock_guard<std::mutex> msgLock(m_messageMutex);
                NetworkMessage playerCopy = msg;
                playerCopy.senderID = clientId;
                m_worldMessageQueue.push(std::move(playerCopy));
            }
            ++routedCount;
        }

        SPARK_LOG_INFO("WorldServer", "Broadcast message (type=%u) routed to %u player(s) across their area servers.",
                       static_cast<unsigned>(msg.type), routedCount);
    }

    // ============================================================================
    // Console
    // ============================================================================

    std::string WorldServer::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "WorldServer '" << m_config.worldName << "': " << (m_running.load() ? "Running" : "Stopped")
            << " | Areas: " << m_stats.activeAreas << " | Players: " << m_stats.totalPlayers
            << " (peak: " << m_stats.peakPlayers << ")" << " | Transfers: " << m_stats.totalAreaTransfers
            << " | Avg load: " << static_cast<int>(m_stats.averageAreaLoad * 100) << "%"
            << " | Uptime: " << m_stats.uptimeSeconds << "s";
        return oss.str();
    }

    // ============================================================================
    // Internal
    // ============================================================================

    void WorldServer::TickLoop()
    {
        auto targetTickDuration = std::chrono::microseconds(static_cast<int64_t>(1000000.0f / m_config.tickRate));

        while (m_running.load(std::memory_order_acquire))
        {
            auto tickStart = std::chrono::steady_clock::now();

            float deltaTime = 1.0f / m_config.tickRate;
            Tick(deltaTime);

            auto tickEnd = std::chrono::steady_clock::now();
            auto tickDuration = tickEnd - tickStart;

            if (tickDuration < targetTickDuration)
            {
                std::this_thread::sleep_for(targetTickDuration - tickDuration);
            }
        }
    }

    void WorldServer::ProcessWorldMessages(float deltaTime)
    {
        std::queue<NetworkMessage> localQueue;
        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            std::swap(localQueue, m_worldMessageQueue);
        }

        uint32_t processedCount = 0;

        while (!localQueue.empty())
        {
            NetworkMessage msg = std::move(localQueue.front());
            localQueue.pop();

            switch (msg.type)
            {
            case MessageType::Connect:
            {
                // Route connection request — use sender as client ID,
                // spawn at origin if no position data is available.
                XMFLOAT3 spawnPos{0.0f, 0.0f, 0.0f};
                HandlePlayerConnect(msg.senderID, "Player_" + std::to_string(msg.senderID), spawnPos);
                break;
            }
            case MessageType::Disconnect:
            {
                HandlePlayerDisconnect(msg.senderID);
                break;
            }
            case MessageType::GameStateSync:
            {
                // Area transfer request — extract target area from first 4 bytes of payload
                if (msg.payload.size() >= sizeof(AreaID))
                {
                    AreaID targetArea = 0;
                    std::memcpy(&targetArea, msg.payload.data(), sizeof(AreaID));
                    TransferPlayer(msg.senderID, targetArea);
                }
                else
                {
                    SPARK_LOG_WARN("WorldServer", "Area transfer request from client %u has insufficient payload.",
                                   msg.senderID);
                }
                break;
            }
            case MessageType::Heartbeat:
            {
                // Update the sending area's heartbeat timestamp
                std::lock_guard<std::mutex> areaLock(m_areaMutex);
                for (auto& [id, reg] : m_areas)
                {
                    // Match by sender ID interpreted as area ID for inter-server heartbeats
                    if (id == static_cast<AreaID>(msg.senderID))
                    {
                        reg.lastHeartbeat = std::chrono::steady_clock::now();
                        if (!reg.isOnline)
                        {
                            reg.isOnline = true;
                            SPARK_LOG_INFO("WorldServer", "Area '%s' (ID=%u) back online via heartbeat.",
                                           reg.areaName.c_str(), id);
                        }
                        break;
                    }
                }
                break;
            }
            default:
                // Unhandled message type — log at debug level
                break;
            }

            ++processedCount;
        }

        m_stats.totalMessagesProcessed += processedCount;

        // Periodic logging of message processing stats (every 10 seconds)
        m_messageLogTimer += deltaTime;
        if (m_messageLogTimer >= 10.0f)
        {
            m_messageLogTimer = 0.0f;
            if (m_stats.totalMessagesProcessed > 0)
            {
                SPARK_LOG_INFO("WorldServer", "Message stats: %u total processed, %u this interval.",
                               m_stats.totalMessagesProcessed, processedCount);
            }
        }
    }

    void WorldServer::ProcessAreaHeartbeats()
    {
        std::lock_guard<std::mutex> lock(m_areaMutex);
        auto now = std::chrono::steady_clock::now();

        for (auto& [id, reg] : m_areas)
        {
            auto elapsed = std::chrono::duration<float>(now - reg.lastHeartbeat).count();
            if (elapsed > 30.0f && reg.isOnline)
            {
                reg.isOnline = false;
                Log("Area '" + reg.areaName + "' heartbeat timeout — marked offline.");
            }
        }
    }

    void WorldServer::UpdatePlayerSessions(float deltaTime)
    {
        std::lock_guard<std::mutex> lock(m_playerMutex);
        for (auto& [clientId, session] : m_playerSessions)
        {
            session.sessionDuration += deltaTime;

            // Complete pending transfers
            if (session.isTransferring && session.pendingArea != INVALID_AREA)
            {
                session.currentArea = session.pendingArea;
                session.pendingArea = INVALID_AREA;
                session.isTransferring = false;
            }
        }
    }

    void WorldServer::ProcessEntityMigrations()
    {
        std::lock_guard<std::mutex> lock(m_areaMutex);

        if (m_areas.size() < 2)
        {
            // No migrations possible with fewer than two areas
            return;
        }

        // Check all registered areas for potential pending entity migrations.
        // In a full implementation each AreaServer would maintain a pending-migration
        // queue; here we inspect area loads and log what would happen.
        for (const auto& [srcId, srcReg] : m_areas)
        {
            if (!srcReg.isOnline)
            {
                continue;
            }

            // Conceptual check: an area with high client count relative to max
            // may have entities that should migrate to a neighbouring area.
            if (srcReg.currentClients <= 0 || srcReg.currentClients < static_cast<int>(srcReg.maxClients * 0.9f))
            {
                continue;
            }

            // Find a neighbouring online area with capacity
            for (const auto& [dstId, dstReg] : m_areas)
            {
                if (dstId == srcId || !dstReg.isOnline)
                {
                    continue;
                }

                if (dstReg.currentClients >= dstReg.maxClients)
                {
                    continue;
                }

                // Route migrating entities: log the planned migration
                SPARK_LOG_INFO("WorldServer",
                               "Entity migration: would route entities from area '%s' (ID=%u, clients=%d/%d) "
                               "to area '%s' (ID=%u, clients=%d/%d).",
                               srcReg.areaName.c_str(), srcId, srcReg.currentClients, srcReg.maxClients,
                               dstReg.areaName.c_str(), dstId, dstReg.currentClients, dstReg.maxClients);

                m_stats.totalEntityMigrations++;

                SPARK_LOG_INFO("WorldServer",
                               "Entity migration confirmed (conceptual): area '%s' -> '%s'. "
                               "Total migrations: %u.",
                               srcReg.areaName.c_str(), dstReg.areaName.c_str(), m_stats.totalEntityMigrations);

                // Only migrate to the first suitable target per source area
                break;
            }
        }
    }

    void WorldServer::Log(const std::string& message)
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        SPARK_LOG_INFO("WorldServer", "%s", message.c_str());
    }

    AreaID WorldServer::AllocateAreaID()
    {
        return m_nextAreaId++;
    }

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
