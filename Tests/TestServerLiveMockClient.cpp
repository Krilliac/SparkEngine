/**
 * @file TestServerLiveMockClient.cpp
 * @brief Live integration tests — starts a real NetworkManager server and
 *        connects UDP mock clients against it over the loopback interface.
 *
 * Uses NetworkManager directly (the proven pattern from TestNetworkMMOIntegration)
 * with DedicatedServer-style handler registration for chat broadcast. Also
 * exercises DedicatedServer's non-network API (RCON, match state, map rotation).
 *
 * Socket tests are limited to 5 server cycles to avoid a known NetworkManager
 * singleton reset issue where the 6th Shutdown/Initialize/StartServer cycle
 * fails to receive incoming packets on Linux.
 *
 * Exercises the actual socket path (ENABLE_NETWORKING required):
 *   - Real UDP client v2 handshake (Tests/Fixtures/SecureTestPeer.h) -> ConnectAccepted response
 *   - NetworkManager player tracking (GetClients)
 *   - Chat message relay (send from one client, verify another receives it)
 *   - Multiple concurrent clients with unique IDs
 *   - ConnectAccepted contains assigned client ID, server kick, and disconnect
 *   - DedicatedServer RCON, match state, map rotation, and stats
 */

#include "TestFramework.h"

// Platform stubs for non-Windows
#ifndef SPARK_PLATFORM_WINDOWS
#ifndef _XM_NO_INTRINSICS_
#define _XM_NO_INTRINSICS_
#endif
#endif

#include "Engine/Networking/DedicatedServer.h"

#ifdef ENABLE_NETWORKING

#include "Fixtures/SecureTestPeer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using namespace Spark::Net;
using SparkTestFixtures::BuildWire;
using SparkTestFixtures::SecureRawClient;

// ============================================================================
// Message builders — inner messages; SecureRawClient frames and seals them
// (NET-100 v2, docs/specs/networking-wire-format.md)
// ============================================================================

static std::vector<uint8_t> BuildChatWire(uint32_t senderID, const std::string& name, const std::string& text)
{
    // ChatMessage payload per docs/specs/networking-wire-format.md: sender name
    // then text, both NetBuffer strings starting at payload offset 0. The server
    // screens those text fields (PacketValidator's ChatMessage schema declares
    // stringFieldOffset = 0), so a leading header byte here frame-shifts every
    // length prefix and the packet is rejected as BadString before dispatch.
    NetBuffer payload;
    payload.WriteString(name);
    payload.WriteString(text);
    return BuildWire(MessageType::ChatMessage, payload.GetData(), ChannelType::ReliableOrdered, 0, senderID);
}

static std::vector<uint8_t> BuildDisconnectWire(uint32_t senderID)
{
    return BuildWire(MessageType::Disconnect, {}, ChannelType::Reliable, 0, senderID);
}

// ============================================================================
// Port allocator — unique ports to avoid conflicts with other test files
// ============================================================================

static std::atomic<uint16_t> s_liveTestPort{30100};

static uint16_t AllocPort()
{
    return s_liveTestPort.fetch_add(1);
}

// ============================================================================
// Helper: pump NetworkManager server frames
// ============================================================================

static void RunFrames(NetworkManager& nm, int frames, float dt = 0.016f)
{
    for (int i = 0; i < frames; ++i)
    {
        nm.Update(dt);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ============================================================================
// 1. Client connects and receives ConnectAccepted
// ============================================================================

TEST(LiveServer_ClientConnectAccepted)
{
    uint16_t port = AllocPort();
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
    nm.Initialize();
    EXPECT_TRUE(nm.StartServer(port, 32));

    SecureRawClient client;
    EXPECT_TRUE(client.Socket().IsReady());
    EXPECT_TRUE(client.Socket().SendTo(port, client.BeginConnect()));

    const bool gotAccepted = client.AwaitType(nm, MessageType::ConnectAccepted).has_value();
    EXPECT_TRUE(gotAccepted);
    EXPECT_EQ(static_cast<int>(nm.GetClients().size()), 1);

    nm.StopServer();
    nm.Shutdown();
}

// ============================================================================
// 2. Server tracks player count correctly
// ============================================================================

TEST(LiveServer_PlayerCountTracking)
{
    uint16_t port = AllocPort();
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
    nm.Initialize();
    EXPECT_TRUE(nm.StartServer(port, 32));

    SecureRawClient c1, c2, c3;
    EXPECT_TRUE(c1.Connect(nm, "P1"));
    EXPECT_EQ(static_cast<int>(nm.GetClients().size()), 1);
    EXPECT_TRUE(c2.Connect(nm, "P2"));
    EXPECT_EQ(static_cast<int>(nm.GetClients().size()), 2);
    EXPECT_TRUE(c3.Connect(nm, "P3"));
    EXPECT_EQ(static_cast<int>(nm.GetClients().size()), 3);

    nm.StopServer();
    nm.Shutdown();
}

// ============================================================================
// 3. Chat relay — client A sends, client B receives
// ============================================================================

TEST(LiveServer_ChatRelay)
{
    uint16_t port = AllocPort();
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
    nm.Initialize();
    EXPECT_TRUE(nm.StartServer(port, 32));

    // Register chat broadcast handler (same as DedicatedServer does)
    nm.RegisterHandler(MessageType::ChatMessage,
                       [](const NetworkMessage& msg)
                       {
                           auto& mgr = NetworkManager::GetInstance();
                           if (mgr.GetRole() == NetworkRole::Server)
                           {
                               mgr.SendToAllExcept(msg.senderID, msg);
                           }
                       });

    SecureRawClient alice, bob;
    EXPECT_TRUE(alice.Connect(nm, "Alice"));
    EXPECT_TRUE(bob.Connect(nm, "Bob"));
    EXPECT_EQ(static_cast<int>(nm.GetClients().size()), 2);

    EXPECT_TRUE(alice.SendSealed(BuildChatWire(alice.Id(), "Alice", "Hello from Alice!")));
    const bool gotChat = bob.AwaitType(nm, MessageType::ChatMessage).has_value();
    EXPECT_TRUE(gotChat);

    nm.UnregisterHandler(MessageType::ChatMessage);
    nm.StopServer();
    nm.Shutdown();
}

// ============================================================================
// 4. Multiple clients get unique IDs
// ============================================================================

TEST(LiveServer_MultipleClientsUniqueIDs)
{
    uint16_t port = AllocPort();
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
    nm.Initialize();
    EXPECT_TRUE(nm.StartServer(port, 32));

    constexpr int NUM_CLIENTS = 4;
    SecureRawClient clients[NUM_CLIENTS];
    for (int i = 0; i < NUM_CLIENTS; ++i)
    {
        EXPECT_TRUE(clients[i].Connect(nm, "Player" + std::to_string(i)));
    }

    auto connectedClients = nm.GetClients();
    EXPECT_EQ(connectedClients.size(), static_cast<size_t>(NUM_CLIENTS));

    std::vector<ClientID> ids;
    for (const auto& [id, info] : connectedClients)
        ids.push_back(id);

    std::sort(ids.begin(), ids.end());
    auto last = std::unique(ids.begin(), ids.end());
    EXPECT_EQ(std::distance(ids.begin(), last), NUM_CLIENTS);

    nm.StopServer();
    nm.Shutdown();
}

// ============================================================================
// 5. ConnectAccepted ID, kick, and graceful disconnect (combined test)
//    Uses a single server cycle to test multiple client lifecycle operations
// ============================================================================

TEST(LiveServer_ClientLifecycle)
{
    uint16_t port = AllocPort();
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
    nm.Initialize();
    EXPECT_TRUE(nm.StartServer(port, 32));

    // --- Part A: ConnectAccepted contains valid client ID ---
    {
        SecureRawClient client;
        EXPECT_TRUE(client.Connect(nm, "IDCheck"));
        EXPECT_TRUE(client.Id() != 0);
    }

    // IDCheck client is still connected — verify
    EXPECT_EQ(static_cast<int>(nm.GetClients().size()), 1);

    // --- Part B: Kick the connected client ---
    {
        auto clients = nm.GetClients();
        EXPECT_FALSE(clients.empty());
        if (!clients.empty())
        {
            ClientID kickID = clients.begin()->first;
            nm.KickClient(kickID, "Testing kick");
            RunFrames(nm, 5);
            EXPECT_EQ(static_cast<int>(nm.GetClients().size()), 0);
        }
    }

    // --- Part C: Graceful disconnect (sealed, so only the session owner can send it) ---
    {
        SecureRawClient client;
        EXPECT_TRUE(client.Connect(nm, "DisconnectMe"));

        auto clients = nm.GetClients();
        EXPECT_EQ(clients.size(), static_cast<size_t>(1));
        if (!clients.empty())
        {
            EXPECT_TRUE(client.SendSealed(BuildDisconnectWire(client.Id())));
            RunFrames(nm, 10);
            EXPECT_EQ(static_cast<int>(nm.GetClients().size()), 0);
        }
    }

    nm.StopServer();
    nm.Shutdown();
}

// ============================================================================
// 6. DedicatedServer RCON dispatch
// ============================================================================

TEST(LiveServer_RconExecution)
{
    DedicatedServer server;
    ServerConfig config;
    config.serverName = "RCONTest";
    config.port = AllocPort();
    config.maxClients = 4;
    config.tickRate = 60.0f;
    config.enableLanBroadcast = false;
    config.enableLogging = false;
    config.mapRotation = {"dm_alpha", "dm_beta", "dm_gamma"};

    EXPECT_TRUE(server.InitializeOnly(config));

    std::string statusResult = server.ExecuteRcon("status");
    EXPECT_FALSE(statusResult.empty());

    std::string unknownResult = server.ExecuteRcon("nonexistent_cmd");
    EXPECT_FALSE(unknownResult.empty());

    std::string playersResult = server.ExecuteRcon("players");
    EXPECT_FALSE(playersResult.empty());

    server.Stop();
}

// ============================================================================
// 7. DedicatedServer match state
// ============================================================================

TEST(LiveServer_MatchState)
{
    DedicatedServer server;
    ServerConfig config;
    config.serverName = "MatchTest";
    config.port = AllocPort();
    config.maxClients = 4;
    config.tickRate = 60.0f;
    config.timeLimitMinutes = 10.0f;
    config.mapRotation = {"dm_alpha", "dm_beta"};
    config.enableLanBroadcast = false;
    config.enableLogging = false;

    EXPECT_TRUE(server.InitializeOnly(config));

    server.StartMatch();

    EXPECT_TRUE(server.IsMatchInProgress());
    EXPECT_TRUE(server.GetMatchTimeRemaining() > 0.0f);
    EXPECT_EQ(server.GetCurrentMap(), std::string("dm_alpha"));

    for (int i = 0; i < 10; ++i)
        server.Tick(0.016f);

    EXPECT_TRUE(server.GetStats().totalTicksProcessed > 0);

    server.Stop();
}

// ============================================================================
// 8. DedicatedServer map rotation
// ============================================================================

TEST(LiveServer_MapRotation)
{
    DedicatedServer server;
    ServerConfig config;
    config.serverName = "MapRotTest";
    config.port = AllocPort();
    config.maxClients = 4;
    config.tickRate = 60.0f;
    config.mapRotation = {"dm_one", "dm_two", "dm_three"};
    config.enableLanBroadcast = false;
    config.enableLogging = false;

    EXPECT_TRUE(server.InitializeOnly(config));

    EXPECT_EQ(server.GetCurrentMap(), std::string("dm_one"));

    server.ExecuteRcon("nextmap");
    EXPECT_EQ(server.GetCurrentMap(), std::string("dm_two"));

    server.ExecuteRcon("map dm_three");
    EXPECT_EQ(server.GetCurrentMap(), std::string("dm_three"));

    server.Stop();
}

// ============================================================================
// 9. DedicatedServer stats accumulate via Tick()
// ============================================================================

TEST(LiveServer_StatsAccumulate)
{
    DedicatedServer server;
    ServerConfig config;
    config.serverName = "StatsTest";
    config.port = AllocPort();
    config.maxClients = 4;
    config.tickRate = 60.0f;
    config.enableLanBroadcast = false;
    config.enableLogging = false;

    EXPECT_TRUE(server.InitializeOnly(config));

    for (int i = 0; i < 20; ++i)
        server.Tick(0.016f);

    const auto& stats = server.GetStats();
    EXPECT_TRUE(stats.totalTicksProcessed > 0);
    EXPECT_TRUE(stats.uptimeSeconds > 0.0f);

    server.Stop();
}

#else // !ENABLE_NETWORKING

TEST(LiveServer_Skipped)
{
    SKIP_TEST("ENABLE_NETWORKING is OFF in this configuration");
}

#endif // ENABLE_NETWORKING
