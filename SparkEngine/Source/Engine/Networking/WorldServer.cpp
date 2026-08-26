/**
 * @file WorldServer.cpp
 * @brief World server coordinator implementation
 */

#include "WorldServer.h"

#ifdef ENABLE_NETWORKING

#include "../../Core/FaultIsolation.h"
#include "../../Utils/LogMacros.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

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
        {
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats = {};
        }
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
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.activeAreas = 0;
        }
        {
            std::lock_guard<std::mutex> lock(m_playerMutex);
            m_playerSessions.clear();
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.totalPlayers = 0;
        }

        Log("World server stopped.");
    }

    void WorldServer::Tick(float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        SPARK_GUARDED_UPDATE("World:Messages", "Network", { ProcessWorldMessages(deltaTime); });
        SPARK_GUARDED_UPDATE("World:Heartbeats", "Network", { ProcessAreaHeartbeats(); });
        SPARK_GUARDED_UPDATE("World:Sessions", "Network", { UpdatePlayerSessions(deltaTime); });
        SPARK_GUARDED_UPDATE("World:Migration", "Network", { ProcessEntityMigrations(); });

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
        const float uptimeSeconds = std::chrono::duration<float>(now - m_startTime).count();
        {
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.uptimeSeconds = uptimeSeconds;
        }
    }

    // ============================================================================
    // Area Management
    // ============================================================================

    AreaID WorldServer::RegisterAreaServer(const AreaServerConfig& config)
    {
        AreaID id = INVALID_AREA;
        {
            std::lock_guard<std::mutex> lock(m_areaMutex);

            id = AllocateAreaID();
            AreaRegistration reg;
            reg.areaId = id;
            reg.areaName = config.areaName;
            reg.port = config.port;
            reg.interServerPort = config.interServerPort;
            reg.maxClients = config.maxClients;
            reg.isOnline = true;
            reg.lastHeartbeat = std::chrono::steady_clock::now();

            m_areas[id] = std::move(reg);
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.activeAreas = static_cast<uint32_t>(m_areas.size());
        }

        Log("Registered area server '" + config.areaName + "' (ID=" + std::to_string(id) + ").");
        return id;
    }

    void WorldServer::UnregisterAreaServer(AreaID areaId)
    {
        std::string removedAreaName;
        {
            std::lock_guard<std::mutex> lock(m_areaMutex);
            auto it = m_areas.find(areaId);
            if (it == m_areas.end())
            {
                return;
            }

            removedAreaName = it->second.areaName;
            m_areas.erase(it);
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.activeAreas = static_cast<uint32_t>(m_areas.size());
        }

        Log("Unregistered area server '" + removedAreaName + "'.");
    }

    std::optional<AreaRegistration> WorldServer::GetAreaInfoSnapshot(AreaID areaId) const
    {
        std::lock_guard<std::mutex> lock(m_areaMutex);
        auto it = m_areas.find(areaId);
        if (it == m_areas.end())
        {
            return std::nullopt;
        }
        return it->second;
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

        AreaID nearestArea = INVALID_AREA;
        {
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

        uint32_t playerCount = 0;
        {
            std::lock_guard<std::mutex> lock(m_playerMutex);
            PlayerSession session;
            session.clientId = clientId;
            session.playerName = playerName;
            session.currentArea = targetArea;
            session.lastKnownPosition = spawnPosition;
            m_playerSessions[clientId] = session;
            playerCount = static_cast<uint32_t>(m_playerSessions.size());
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.totalPlayers = playerCount;
            m_stats.peakPlayers = std::max(m_stats.peakPlayers, playerCount);
        }

        Log("Player '" + playerName + "' connected, assigned to area " + std::to_string(targetArea) + ".");
        return targetArea;
    }

    void WorldServer::HandlePlayerDisconnect(ClientID clientId)
    {
        std::string disconnectedPlayerName;
        {
            std::lock_guard<std::mutex> lock(m_playerMutex);
            auto it = m_playerSessions.find(clientId);
            if (it != m_playerSessions.end())
            {
                disconnectedPlayerName = it->second.playerName;
                m_playerSessions.erase(it);
            }
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.totalPlayers = static_cast<uint32_t>(m_playerSessions.size());
        }

        if (!disconnectedPlayerName.empty())
        {
            Log("Player '" + disconnectedPlayerName + "' disconnected.");
        }

        // Scope cleanup runs inside NetworkManager::HandleDisconnect as well,
        // but we call it here defensively for callers that bypass the network
        // layer (tests, direct WorldServer drive-by disconnects).
        NetworkManager::GetInstance().ClearClientScope(clientId);
    }

    bool WorldServer::UpdatePlayerPosition(ClientID clientId, const XMFLOAT3& position, float interestRadius)
    {
        AreaID areaForScope = INVALID_AREA;
        {
            std::lock_guard<std::mutex> lock(m_playerMutex);
            auto it = m_playerSessions.find(clientId);
            if (it == m_playerSessions.end())
            {
                return false;
            }
            it->second.lastKnownPosition = position;
            areaForScope = it->second.currentArea;
        }

        // Forward to NetworkManager so replication filters out-of-interest entities
        // for this connection on subsequent ticks. Defaulted masks accept all teams
        // and visibility flags; game code can use NetworkManager::SetClientScope
        // directly for finer-grained filtering.
        NetworkManager::GetInstance().SetClientScope(
            clientId, position, interestRadius,
            areaForScope == INVALID_AREA ? 0u : static_cast<uint32_t>(areaForScope));
        return true;
    }

    bool WorldServer::TransferPlayer(ClientID clientId, AreaID targetArea)
    {
        std::string playerName;
        AreaID currentArea = INVALID_AREA;
        bool alreadyTransferring = false;
        {
            std::lock_guard<std::mutex> lock(m_playerMutex);
            auto it = m_playerSessions.find(clientId);
            if (it == m_playerSessions.end())
            {
                return false;
            }

            auto& session = it->second;
            playerName = session.playerName;
            currentArea = session.currentArea;
            alreadyTransferring = session.isTransferring;
            if (!alreadyTransferring)
            {
                session.pendingArea = targetArea;
                session.isTransferring = true;
            }
        }

        if (alreadyTransferring)
        {
            Log("Player '" + playerName + "' is already transferring.");
            return false;
        }

        {
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            ++m_stats.totalAreaTransfers;
        }

        Log("Transferring player '" + playerName + "' from area " + std::to_string(currentArea) +
            " to area " + std::to_string(targetArea) + ".");
        return true;
    }

    std::optional<PlayerSession> WorldServer::GetPlayerSessionSnapshot(ClientID clientId) const
    {
        std::lock_guard<std::mutex> lock(m_playerMutex);
        auto it = m_playerSessions.find(clientId);
        if (it == m_playerSessions.end())
        {
            return std::nullopt;
        }
        return it->second;
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
        const std::vector<AreaRegistration> areas = GetAllAreas();
        if (areas.empty())
        {
            return;
        }

        // Calculate average load
        float totalLoad = 0.0f;
        for (const auto& reg : areas)
        {
            totalLoad += reg.cpuLoad;
        }
        const float averageAreaLoad = totalLoad / static_cast<float>(areas.size());
        uint32_t loadBalanceEvent = 0;
        {
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.averageAreaLoad = averageAreaLoad;
            loadBalanceEvent = ++m_stats.loadBalanceEvents;
        }

        // Identify overloaded and underloaded areas
        std::vector<AreaRegistration> overloaded;
        std::vector<AreaRegistration> underloaded;

        for (const auto& reg : areas)
        {
            if (!reg.isOnline)
            {
                continue;
            }

            if (reg.cpuLoad > LOAD_BALANCE_OVERLOAD_THRESHOLD)
            {
                overloaded.push_back(reg);
                SPARK_LOG_WARN("WorldServer", "Area '%s' (ID=%u) is overloaded: CPU=%.1f%%.", reg.areaName.c_str(), reg.areaId,
                               reg.cpuLoad * 100.0f);
            }
            else if (reg.cpuLoad < LOAD_BALANCE_UNDERLOAD_THRESHOLD)
            {
                underloaded.push_back(reg);
                SPARK_LOG_INFO("WorldServer", "Area '%s' (ID=%u) is underloaded: CPU=%.1f%%.", reg.areaName.c_str(), reg.areaId,
                               reg.cpuLoad * 100.0f);
            }
        }

        // Plan migrations from overloaded to underloaded areas
        if (!overloaded.empty() && !underloaded.empty())
        {
            size_t migrationCount = std::min(overloaded.size(), underloaded.size());
            for (size_t i = 0; i < migrationCount; ++i)
            {
                const auto& srcReg = overloaded[i];
                const auto& dstReg = underloaded[i];
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
                       loadBalanceEvent, averageAreaLoad * 100.0f, overloaded.size(), underloaded.size());
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

        uint32_t queuedCount = 0;
        {
            std::lock_guard<std::mutex> lock(m_areaMutex);
            for (const auto& [id, reg] : m_areas)
            {
                if (reg.isOnline)
                {
                    ++queuedCount;
                }
            }
        }

        // Queue one copy for each online area without holding the area map lock.
        {
            std::lock_guard<std::mutex> msgLock(m_messageMutex);
            for (uint32_t i = 0; i < queuedCount; ++i)
            {
                NetworkMessage areaCopy = msg;
                areaCopy.senderID = INVALID_CLIENT; // Indicate world-server origin
                m_worldMessageQueue.push(std::move(areaCopy));
            }
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

        std::vector<ClientID> routedClients;
        std::vector<std::pair<ClientID, std::string>> invalidAreaPlayers;
        {
            std::lock_guard<std::mutex> playerLock(m_playerMutex);
            routedClients.reserve(m_playerSessions.size());
            for (const auto& [clientId, session] : m_playerSessions)
            {
                if (session.currentArea == INVALID_AREA)
                {
                    invalidAreaPlayers.emplace_back(clientId, session.playerName);
                }
                else
                {
                    routedClients.push_back(clientId);
                }
            }
        }

        if (routedClients.empty() && invalidAreaPlayers.empty())
        {
            SPARK_LOG_INFO("WorldServer", "BroadcastToAllPlayers: no players connected; nothing to send.");
            return;
        }

        for (const auto& [clientId, playerName] : invalidAreaPlayers)
        {
            SPARK_LOG_WARN("WorldServer", "Player '%s' (client=%u) has no valid area; skipping broadcast.",
                           playerName.c_str(), clientId);
        }

        // Route through each player's current area server by queuing a copy
        // addressed to that specific client.
        {
            std::lock_guard<std::mutex> msgLock(m_messageMutex);
            for (ClientID clientId : routedClients)
            {
                NetworkMessage playerCopy = msg;
                playerCopy.senderID = clientId;
                m_worldMessageQueue.push(std::move(playerCopy));
            }
        }

        SPARK_LOG_INFO("WorldServer", "Broadcast message (type=%u) routed to %u player(s) across their area servers.",
                       static_cast<unsigned>(msg.type), static_cast<uint32_t>(routedClients.size()));
    }

    // ============================================================================
    // Console
    // ============================================================================

    std::string WorldServer::Console_GetStatus() const
    {
        const WorldServerStats stats = GetStats();
        std::ostringstream oss;
        oss << "WorldServer '" << m_config.worldName << "': " << (m_running.load() ? "Running" : "Stopped")
            << " | Areas: " << stats.activeAreas << " | Players: " << stats.totalPlayers
            << " (peak: " << stats.peakPlayers << ")" << " | Transfers: " << stats.totalAreaTransfers
            << " | Avg load: " << static_cast<int>(stats.averageAreaLoad * 100) << "%"
            << " | Uptime: " << stats.uptimeSeconds << "s";
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
                std::string recoveredAreaName;
                AreaID recoveredAreaId = INVALID_AREA;
                {
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
                                recoveredAreaName = reg.areaName;
                                recoveredAreaId = id;
                            }
                            break;
                        }
                    }
                }
                if (recoveredAreaId != INVALID_AREA)
                {
                    SPARK_LOG_INFO("WorldServer", "Area '%s' (ID=%u) back online via heartbeat.",
                                   recoveredAreaName.c_str(), recoveredAreaId);
                }
                break;
            }
            default:
                // Unhandled message type — log at debug level
                break;
            }

            ++processedCount;
        }

        uint32_t totalMessagesProcessed = 0;
        {
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.totalMessagesProcessed += processedCount;
            totalMessagesProcessed = m_stats.totalMessagesProcessed;
        }

        // Periodic logging of message processing stats (every 10 seconds)
        m_messageLogTimer += deltaTime;
        if (m_messageLogTimer >= 10.0f)
        {
            m_messageLogTimer = 0.0f;
            if (totalMessagesProcessed > 0)
            {
                SPARK_LOG_INFO("WorldServer", "Message stats: %u total processed, %u this interval.",
                               totalMessagesProcessed, processedCount);
            }
        }
    }

    void WorldServer::ProcessAreaHeartbeats()
    {
        std::vector<std::string> timedOutAreaNames;
        {
            std::lock_guard<std::mutex> lock(m_areaMutex);
            const auto now = std::chrono::steady_clock::now();

            for (auto& [id, reg] : m_areas)
            {
                const float elapsed = std::chrono::duration<float>(now - reg.lastHeartbeat).count();
                if (elapsed > 30.0f && reg.isOnline)
                {
                    reg.isOnline = false;
                    timedOutAreaNames.push_back(reg.areaName);
                }
            }
        }

        for (const std::string& areaName : timedOutAreaNames)
        {
            Log("Area '" + areaName + "' heartbeat timeout — marked offline.");
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
        const std::vector<AreaRegistration> areas = GetAllAreas();
        if (areas.size() < 2)
        {
            // No migrations possible with fewer than two areas
            return;
        }

        // Check all registered areas for potential pending entity migrations.
        // In a full implementation each AreaServer would maintain a pending-migration
        // queue; here we inspect area loads and log what would happen.
        struct PlannedMigration
        {
            AreaRegistration source;
            AreaRegistration destination;
            uint32_t totalMigrations = 0;
        };
        std::vector<PlannedMigration> migrations;

        for (const auto& srcReg : areas)
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
            for (const auto& dstReg : areas)
            {
                if (dstReg.areaId == srcReg.areaId || !dstReg.isOnline)
                {
                    continue;
                }

                if (dstReg.currentClients >= dstReg.maxClients)
                {
                    continue;
                }

                migrations.push_back({srcReg, dstReg, 0});

                // Only migrate to the first suitable target per source area
                break;
            }
        }

        {
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            for (PlannedMigration& migration : migrations)
            {
                migration.totalMigrations = ++m_stats.totalEntityMigrations;
            }
        }

        for (const PlannedMigration& migration : migrations)
        {
            const auto& srcReg = migration.source;
            const auto& dstReg = migration.destination;
            SPARK_LOG_INFO("WorldServer",
                           "Entity migration: would route entities from area '%s' (ID=%u, clients=%d/%d) "
                           "to area '%s' (ID=%u, clients=%d/%d).",
                           srcReg.areaName.c_str(), srcReg.areaId, srcReg.currentClients, srcReg.maxClients,
                           dstReg.areaName.c_str(), dstReg.areaId, dstReg.currentClients, dstReg.maxClients);
            SPARK_LOG_INFO("WorldServer",
                           "Entity migration confirmed (conceptual): area '%s' -> '%s'. Total migrations: %u.",
                           srcReg.areaName.c_str(), dstReg.areaName.c_str(), migration.totalMigrations);
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
