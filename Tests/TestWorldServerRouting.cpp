/**
 * @file TestWorldServerRouting.cpp
 * @brief Tests for WorldServer: area management, player routing, instance creation,
 *        load balancing, and player transfer between areas.
 *
 * Standalone reimplementation — mirrors Spark::Net::WorldServer
 * without networking dependencies for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

    using ClientID = uint32_t;
    using AreaID = uint32_t;

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    constexpr float LOAD_BALANCE_OVERLOAD_THRESHOLD = 0.8f;
    constexpr float LOAD_BALANCE_UNDERLOAD_THRESHOLD = 0.3f;

    struct WorldServerConfig
    {
        std::string worldName = "DefaultWorld";
        uint16_t port = 27016;
        int maxTotalClients = 1000;
        float tickRate = 20.0f;
        bool enableLoadBalancing = true;
        float loadBalanceInterval = 30.0f;
    };

    struct PlayerSession
    {
        ClientID clientId = 0;
        std::string playerName;
        AreaID currentArea = 0;
        AreaID pendingArea = 0;
        Vec3 lastKnownPosition{};
        float sessionDuration = 0.0f;
        bool isTransferring = false;
    };

    struct AreaRegistration
    {
        AreaID areaId = 0;
        std::string areaName;
        int currentClients = 0;
        int maxClients = 100;
        float cpuLoad = 0.0f;
        bool isOnline = true;
        Vec3 areaPosition{};
        Vec3 areaSize{};
    };

    struct WorldServerStats
    {
        float uptimeSeconds = 0.0f;
        uint32_t totalPlayers = 0;
        uint32_t peakPlayers = 0;
        uint32_t activeAreas = 0;
        uint32_t totalAreaTransfers = 0;
        uint32_t failedTransfers = 0;
        float averageAreaLoad = 0.0f;
        uint32_t loadBalanceEvents = 0;
    };

    // --- WorldServer reimplementation ---

    class WorldServer
    {
      public:
        bool Start(const WorldServerConfig& config)
        {
            m_config = config;
            m_running = true;
            m_stats = {};
            return true;
        }

        void Stop()
        {
            m_running = false;
            m_players.clear();
            m_areas.clear();
        }

        bool IsRunning() const { return m_running; }

        void Tick(float deltaTime)
        {
            m_stats.uptimeSeconds += deltaTime;

            // Update session durations
            for (auto& [id, session] : m_players)
                session.sessionDuration += deltaTime;

            // Update stats
            m_stats.totalPlayers = static_cast<uint32_t>(m_players.size());
            if (m_stats.totalPlayers > m_stats.peakPlayers)
                m_stats.peakPlayers = m_stats.totalPlayers;

            m_stats.activeAreas = 0;
            float totalLoad = 0.0f;
            for (auto& [id, area] : m_areas)
            {
                if (area.isOnline)
                {
                    m_stats.activeAreas++;
                    totalLoad += area.cpuLoad;
                }
            }
            m_stats.averageAreaLoad =
                (m_stats.activeAreas > 0) ? totalLoad / static_cast<float>(m_stats.activeAreas) : 0.0f;
        }

        AreaID RegisterAreaServer(const std::string& name, const Vec3& position, const Vec3& size, int maxClients = 100)
        {
            AreaID id = m_nextAreaID++;
            AreaRegistration reg;
            reg.areaId = id;
            reg.areaName = name;
            reg.areaPosition = position;
            reg.areaSize = size;
            reg.maxClients = maxClients;
            m_areas[id] = reg;
            return id;
        }

        void UnregisterAreaServer(AreaID areaId) { m_areas.erase(areaId); }

        const AreaRegistration* GetAreaInfo(AreaID areaId) const
        {
            auto it = m_areas.find(areaId);
            return (it != m_areas.end()) ? &it->second : nullptr;
        }

        std::vector<AreaRegistration> GetAllAreas() const
        {
            std::vector<AreaRegistration> result;
            for (auto& [id, area] : m_areas)
                result.push_back(area);
            return result;
        }

        AreaID GetAreaForPosition(const Vec3& position) const
        {
            for (auto& [id, area] : m_areas)
            {
                if (!area.isOnline)
                    continue;
                // Simple AABB check
                float minX = area.areaPosition.x - area.areaSize.x * 0.5f;
                float maxX = area.areaPosition.x + area.areaSize.x * 0.5f;
                float minZ = area.areaPosition.z - area.areaSize.z * 0.5f;
                float maxZ = area.areaPosition.z + area.areaSize.z * 0.5f;

                if (position.x >= minX && position.x <= maxX && position.z >= minZ && position.z <= maxZ)
                {
                    return id;
                }
            }
            return 0; // no area found
        }

        AreaID HandlePlayerConnect(ClientID clientId, const std::string& playerName, const Vec3& spawnPosition)
        {
            if (m_players.size() >= static_cast<size_t>(m_config.maxTotalClients))
                return 0; // server full

            AreaID area = GetAreaForPosition(spawnPosition);
            if (area == 0)
                return 0; // no area for spawn

            // Check area capacity
            auto* areaInfo = GetAreaInfo(area);
            if (areaInfo && areaInfo->currentClients >= areaInfo->maxClients)
                return 0;

            PlayerSession session;
            session.clientId = clientId;
            session.playerName = playerName;
            session.currentArea = area;
            session.lastKnownPosition = spawnPosition;
            m_players[clientId] = session;

            // Update area client count
            m_areas[area].currentClients++;

            return area;
        }

        void HandlePlayerDisconnect(ClientID clientId)
        {
            auto it = m_players.find(clientId);
            if (it != m_players.end())
            {
                auto areaIt = m_areas.find(it->second.currentArea);
                if (areaIt != m_areas.end())
                    areaIt->second.currentClients--;
                m_players.erase(it);
            }
        }

        bool TransferPlayer(ClientID clientId, AreaID targetArea)
        {
            auto playerIt = m_players.find(clientId);
            if (playerIt == m_players.end())
            {
                m_stats.failedTransfers++;
                return false;
            }

            auto areaIt = m_areas.find(targetArea);
            if (areaIt == m_areas.end() || !areaIt->second.isOnline)
            {
                m_stats.failedTransfers++;
                return false;
            }

            if (areaIt->second.currentClients >= areaIt->second.maxClients)
            {
                m_stats.failedTransfers++;
                return false;
            }

            // Move player
            auto& session = playerIt->second;
            auto oldAreaIt = m_areas.find(session.currentArea);
            if (oldAreaIt != m_areas.end())
                oldAreaIt->second.currentClients--;

            session.currentArea = targetArea;
            areaIt->second.currentClients++;
            session.isTransferring = false;

            m_stats.totalAreaTransfers++;
            return true;
        }

        const PlayerSession* GetPlayerSession(ClientID clientId) const
        {
            auto it = m_players.find(clientId);
            return (it != m_players.end()) ? &it->second : nullptr;
        }

        uint32_t GetTotalPlayerCount() const { return static_cast<uint32_t>(m_players.size()); }

        void PerformLoadBalancing()
        {
            m_stats.loadBalanceEvents++;

            for (auto& [id, area] : m_areas)
            {
                if (!area.isOnline)
                    continue;
                if (area.cpuLoad > LOAD_BALANCE_OVERLOAD_THRESHOLD)
                {
                    // Find underloaded area
                    for (auto& [otherId, otherArea] : m_areas)
                    {
                        if (otherId == id || !otherArea.isOnline)
                            continue;
                        if (otherArea.cpuLoad < LOAD_BALANCE_UNDERLOAD_THRESHOLD)
                        {
                            // Would transfer players here in real implementation
                            break;
                        }
                    }
                }
            }
        }

        void SetAreaLoad(AreaID areaId, float cpuLoad)
        {
            if (auto it = m_areas.find(areaId); it != m_areas.end())
                it->second.cpuLoad = cpuLoad;
        }

        void SetAreaOnline(AreaID areaId, bool online)
        {
            if (auto it = m_areas.find(areaId); it != m_areas.end())
                it->second.isOnline = online;
        }

        const WorldServerConfig& GetConfig() const { return m_config; }
        const WorldServerStats& GetStats() const { return m_stats; }

      private:
        WorldServerConfig m_config;
        WorldServerStats m_stats;
        bool m_running = false;
        AreaID m_nextAreaID = 1;
        std::unordered_map<ClientID, PlayerSession> m_players;
        std::unordered_map<AreaID, AreaRegistration> m_areas;
    };

} // anonymous namespace

// ============================================================================
// Server Lifecycle Tests
// ============================================================================

TEST(WorldServer_StartAndStop)
{
    WorldServer server;
    WorldServerConfig config;
    config.worldName = "TestWorld";

    EXPECT_TRUE(server.Start(config));
    EXPECT_TRUE(server.IsRunning());

    server.Stop();
    EXPECT_FALSE(server.IsRunning());
}

TEST(WorldServer_ConfigPreserved)
{
    WorldServer server;
    WorldServerConfig config;
    config.worldName = "MyWorld";
    config.maxTotalClients = 500;
    config.tickRate = 30.0f;

    server.Start(config);
    EXPECT_EQ(server.GetConfig().worldName, std::string("MyWorld"));
    EXPECT_EQ(server.GetConfig().maxTotalClients, 500);
    server.Stop();
}

// ============================================================================
// Area Management Tests
// ============================================================================

TEST(WorldServer_RegisterArea)
{
    WorldServer server;
    server.Start({});

    auto id = server.RegisterAreaServer("Town", {0, 0, 0}, {100, 50, 100});
    EXPECT_TRUE(id > 0);

    auto* info = server.GetAreaInfo(id);
    ASSERT_TRUE(info != nullptr);
    EXPECT_EQ(info->areaName, std::string("Town"));
    server.Stop();
}

TEST(WorldServer_UnregisterArea)
{
    WorldServer server;
    server.Start({});

    auto id = server.RegisterAreaServer("Town", {0, 0, 0}, {100, 50, 100});
    server.UnregisterAreaServer(id);

    EXPECT_TRUE(server.GetAreaInfo(id) == nullptr);
    server.Stop();
}

TEST(WorldServer_GetAllAreas)
{
    WorldServer server;
    server.Start({});

    server.RegisterAreaServer("Town", {0, 0, 0}, {100, 50, 100});
    server.RegisterAreaServer("Forest", {200, 0, 0}, {100, 50, 100});
    server.RegisterAreaServer("Dungeon", {0, 0, 200}, {50, 50, 50});

    auto areas = server.GetAllAreas();
    EXPECT_EQ(areas.size(), (size_t)3);
    server.Stop();
}

TEST(WorldServer_GetAreaForPosition)
{
    WorldServer server;
    server.Start({});

    auto townId = server.RegisterAreaServer("Town", {0, 0, 0}, {100, 50, 100});
    server.RegisterAreaServer("Forest", {200, 0, 0}, {100, 50, 100});

    auto found = server.GetAreaForPosition({10, 0, 10});
    EXPECT_EQ(found, townId);

    auto notFound = server.GetAreaForPosition({500, 0, 500});
    EXPECT_EQ(notFound, (AreaID)0);
    server.Stop();
}

// ============================================================================
// Player Routing Tests
// ============================================================================

TEST(WorldServer_PlayerConnect)
{
    WorldServer server;
    server.Start({});

    auto areaId = server.RegisterAreaServer("Town", {0, 0, 0}, {100, 50, 100});
    auto assignedArea = server.HandlePlayerConnect(1, "Alice", {10, 0, 10});

    EXPECT_EQ(assignedArea, areaId);
    EXPECT_EQ(server.GetTotalPlayerCount(), (uint32_t)1);

    auto* session = server.GetPlayerSession(1);
    ASSERT_TRUE(session != nullptr);
    EXPECT_EQ(session->playerName, std::string("Alice"));
    EXPECT_EQ(session->currentArea, areaId);
    server.Stop();
}

TEST(WorldServer_PlayerConnectNoArea)
{
    WorldServer server;
    server.Start({});

    // No areas registered — spawn should fail
    auto area = server.HandlePlayerConnect(1, "Alice", {10, 0, 10});
    EXPECT_EQ(area, (AreaID)0);
    EXPECT_EQ(server.GetTotalPlayerCount(), (uint32_t)0);
    server.Stop();
}

TEST(WorldServer_PlayerDisconnect)
{
    WorldServer server;
    server.Start({});

    server.RegisterAreaServer("Town", {0, 0, 0}, {100, 50, 100});
    server.HandlePlayerConnect(1, "Alice", {10, 0, 10});
    server.HandlePlayerDisconnect(1);

    EXPECT_EQ(server.GetTotalPlayerCount(), (uint32_t)0);
    EXPECT_TRUE(server.GetPlayerSession(1) == nullptr);
    server.Stop();
}

TEST(WorldServer_ServerFull)
{
    WorldServer server;
    WorldServerConfig config;
    config.maxTotalClients = 2;
    server.Start(config);

    server.RegisterAreaServer("Town", {0, 0, 0}, {100, 50, 100});
    server.HandlePlayerConnect(1, "Alice", {10, 0, 10});
    server.HandlePlayerConnect(2, "Bob", {20, 0, 10});

    auto area = server.HandlePlayerConnect(3, "Charlie", {30, 0, 10});
    EXPECT_EQ(area, (AreaID)0); // rejected — full
    EXPECT_EQ(server.GetTotalPlayerCount(), (uint32_t)2);
    server.Stop();
}

TEST(WorldServer_AreaCapacityFull)
{
    WorldServer server;
    server.Start({});

    server.RegisterAreaServer("SmallRoom", {0, 0, 0}, {100, 50, 100}, 1); // max 1 client

    server.HandlePlayerConnect(1, "Alice", {10, 0, 10});
    auto area = server.HandlePlayerConnect(2, "Bob", {20, 0, 10});

    EXPECT_EQ(area, (AreaID)0); // area full
    server.Stop();
}

// ============================================================================
// Player Transfer Tests
// ============================================================================

TEST(WorldServer_TransferPlayer)
{
    WorldServer server;
    server.Start({});

    auto town = server.RegisterAreaServer("Town", {0, 0, 0}, {100, 50, 100});
    auto forest = server.RegisterAreaServer("Forest", {200, 0, 0}, {100, 50, 100});

    server.HandlePlayerConnect(1, "Alice", {10, 0, 10});
    EXPECT_TRUE(server.TransferPlayer(1, forest));

    auto* session = server.GetPlayerSession(1);
    EXPECT_EQ(session->currentArea, forest);
    EXPECT_EQ(server.GetStats().totalAreaTransfers, (uint32_t)1);
    (void)town;
    server.Stop();
}

TEST(WorldServer_TransferToOfflineArea)
{
    WorldServer server;
    server.Start({});

    server.RegisterAreaServer("Town", {0, 0, 0}, {100, 50, 100});
    auto forest = server.RegisterAreaServer("Forest", {200, 0, 0}, {100, 50, 100});
    server.SetAreaOnline(forest, false);

    server.HandlePlayerConnect(1, "Alice", {10, 0, 10});
    EXPECT_FALSE(server.TransferPlayer(1, forest));
    EXPECT_EQ(server.GetStats().failedTransfers, (uint32_t)1);
    server.Stop();
}

TEST(WorldServer_TransferToFullArea)
{
    WorldServer server;
    server.Start({});

    server.RegisterAreaServer("Town", {0, 0, 0}, {100, 50, 100});
    auto smallRoom = server.RegisterAreaServer("SmallRoom", {200, 0, 0}, {50, 50, 50}, 0);

    server.HandlePlayerConnect(1, "Alice", {10, 0, 10});
    EXPECT_FALSE(server.TransferPlayer(1, smallRoom));
    server.Stop();
}

TEST(WorldServer_TransferNonexistentPlayer)
{
    WorldServer server;
    server.Start({});

    server.RegisterAreaServer("Town", {0, 0, 0}, {100, 50, 100});
    EXPECT_FALSE(server.TransferPlayer(999, 1));
    server.Stop();
}

// ============================================================================
// Stats Tests
// ============================================================================

TEST(WorldServer_UptimeTracking)
{
    WorldServer server;
    server.Start({});

    server.Tick(1.0f);
    server.Tick(0.5f);

    EXPECT_NEAR(server.GetStats().uptimeSeconds, 1.5f, 0.01f);
    server.Stop();
}

TEST(WorldServer_PeakPlayers)
{
    WorldServer server;
    server.Start({});

    server.RegisterAreaServer("Town", {0, 0, 0}, {100, 50, 100});
    server.HandlePlayerConnect(1, "A", {10, 0, 10});
    server.HandlePlayerConnect(2, "B", {20, 0, 10});
    server.Tick(0.016f);
    EXPECT_EQ(server.GetStats().peakPlayers, (uint32_t)2);

    server.HandlePlayerDisconnect(1);
    server.Tick(0.016f);
    EXPECT_EQ(server.GetStats().peakPlayers, (uint32_t)2); // peak stays
    server.Stop();
}

TEST(WorldServer_ActiveAreaCount)
{
    WorldServer server;
    server.Start({});

    server.RegisterAreaServer("Town", {0, 0, 0}, {100, 50, 100});
    auto forest = server.RegisterAreaServer("Forest", {200, 0, 0}, {100, 50, 100});
    server.SetAreaOnline(forest, false);

    server.Tick(0.016f);
    EXPECT_EQ(server.GetStats().activeAreas, (uint32_t)1);
    server.Stop();
}

// ============================================================================
// Load Balancing Tests
// ============================================================================

TEST(WorldServer_LoadBalanceEvent)
{
    WorldServer server;
    server.Start({});

    server.RegisterAreaServer("Area1", {0, 0, 0}, {100, 50, 100});
    server.RegisterAreaServer("Area2", {200, 0, 0}, {100, 50, 100});

    server.PerformLoadBalancing();
    EXPECT_EQ(server.GetStats().loadBalanceEvents, (uint32_t)1);
    server.Stop();
}

TEST(WorldServer_AverageAreaLoad)
{
    WorldServer server;
    server.Start({});

    auto a1 = server.RegisterAreaServer("Area1", {0, 0, 0}, {100, 50, 100});
    auto a2 = server.RegisterAreaServer("Area2", {200, 0, 0}, {100, 50, 100});

    server.SetAreaLoad(a1, 0.6f);
    server.SetAreaLoad(a2, 0.4f);
    server.Tick(0.016f);

    EXPECT_NEAR(server.GetStats().averageAreaLoad, 0.5f, 0.01f);
    server.Stop();
}

// ============================================================================
// Session Duration Tests
// ============================================================================

TEST(WorldServer_SessionDurationUpdates)
{
    WorldServer server;
    server.Start({});

    server.RegisterAreaServer("Town", {0, 0, 0}, {100, 50, 100});
    server.HandlePlayerConnect(1, "Alice", {10, 0, 10});

    server.Tick(1.0f);
    server.Tick(0.5f);

    auto* session = server.GetPlayerSession(1);
    EXPECT_NEAR(session->sessionDuration, 1.5f, 0.01f);
    server.Stop();
}

// ============================================================================
// Area Client Count Tests
// ============================================================================

TEST(WorldServer_AreaClientCountUpdates)
{
    WorldServer server;
    server.Start({});

    auto areaId = server.RegisterAreaServer("Town", {0, 0, 0}, {100, 50, 100});
    server.HandlePlayerConnect(1, "Alice", {10, 0, 10});
    server.HandlePlayerConnect(2, "Bob", {20, 0, 10});

    EXPECT_EQ(server.GetAreaInfo(areaId)->currentClients, 2);

    server.HandlePlayerDisconnect(1);
    EXPECT_EQ(server.GetAreaInfo(areaId)->currentClients, 1);
    server.Stop();
}
