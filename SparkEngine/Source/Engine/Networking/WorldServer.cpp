/**
 * @file WorldServer.cpp
 * @brief World server coordinator implementation
 */

#include "WorldServer.h"

#ifdef ENABLE_NETWORKING

#include "../../Utils/LogMacros.h"

#include <algorithm>
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

    AreaID WorldServer::GetAreaForPosition(const XMFLOAT3& /*position*/) const
    {
        // In a full implementation, this would use the SeamlessAreaManager's
        // area definitions to determine which area contains the position.
        // For now, return the first online area.
        std::lock_guard<std::mutex> lock(m_areaMutex);
        for (const auto& [id, reg] : m_areas)
        {
            if (reg.isOnline)
                return id;
        }
        return INVALID_AREA;
    }

    // ============================================================================
    // Player Management
    // ============================================================================

    AreaID WorldServer::HandlePlayerConnect(ClientID clientId, const std::string& playerName,
                                            const XMFLOAT3& spawnPosition)
    {
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
            return;

        // Calculate average load
        float totalLoad = 0.0f;
        for (const auto& [id, reg] : m_areas)
        {
            totalLoad += reg.cpuLoad;
        }
        m_stats.averageAreaLoad = totalLoad / static_cast<float>(m_areas.size());

        // In a full implementation, this would:
        // 1. Identify overloaded areas (cpuLoad > threshold)
        // 2. Find underloaded machines
        // 3. Migrate areas between machines to balance load
        // For now, we just track the average.
    }

    // ============================================================================
    // Broadcast
    // ============================================================================

    void WorldServer::BroadcastToAllAreas(const NetworkMessage& /*msg*/)
    {
        // In a full implementation, send to all registered AreaServer inter-server ports.
    }

    void WorldServer::BroadcastToAllPlayers(const NetworkMessage& /*msg*/)
    {
        // In a full implementation, route through each AreaServer to reach all clients.
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

    void WorldServer::ProcessWorldMessages(float /*deltaTime*/)
    {
        // In a full implementation, process incoming messages from AreaServers
        // and clients (connection requests, area transfer requests, etc.).
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
        // In a full implementation, this would:
        // 1. Collect pending migrations from all AreaServers
        // 2. Route migrating entities to the target AreaServer
        // 3. Confirm successful migration
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
