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
 *   - Real UDP client connect handshake -> ConnectAccepted response
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using namespace Spark::Net;

// ============================================================================
// Raw UDP test client
// ============================================================================

class LiveTestClient
{
  public:
    bool Open()
    {
        m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket == INVALID_SOCKET)
            return false;

#ifdef SPARK_PLATFORM_WINDOWS
        u_long nonBlocking = 1;
        ioctlsocket(m_socket, FIONBIO, &nonBlocking);
#else
        int flags = fcntl(m_socket, F_GETFL, 0);
        fcntl(m_socket, F_SETFL, flags | O_NONBLOCK);
#endif

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        if (::bind(m_socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
        {
            Close();
            return false;
        }
        return true;
    }

    void Close()
    {
        if (m_socket != INVALID_SOCKET)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            closesocket(m_socket);
#else
            close(m_socket);
#endif
            m_socket = INVALID_SOCKET;
        }
    }

    bool SendTo(const void* data, size_t size, uint16_t port)
    {
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr);
        int sent = sendto(m_socket, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
                          reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        return sent > 0;
    }

    int Receive(void* buf, size_t bufSize)
    {
        sockaddr_in sender{};
        socklen_t senderLen = sizeof(sender);
        return recvfrom(m_socket, reinterpret_cast<char*>(buf), static_cast<int>(bufSize), 0,
                        reinterpret_cast<sockaddr*>(&sender), &senderLen);
    }

    ~LiveTestClient() { Close(); }

  private:
    SOCKET m_socket = INVALID_SOCKET;
};

// ============================================================================
// Packet builders — same wire format as NetworkManager::SerializeMessage
// ============================================================================

static std::vector<uint8_t> BuildPacket(MessageType type, ChannelType channel, uint32_t senderID, uint32_t sequence,
                                        float timestamp, const std::vector<uint8_t>& payload)
{
    NetBuffer buf;
    buf.WriteUint32(0x5350524B);
    buf.WriteUint16(static_cast<uint16_t>(type));
    buf.WriteUint8(static_cast<uint8_t>(channel));
    buf.WriteUint32(senderID);
    buf.WriteUint32(sequence);
    buf.WriteFloat(timestamp);
    buf.WriteUint32(static_cast<uint32_t>(payload.size()));
    if (!payload.empty())
        buf.WriteBytes(payload.data(), payload.size());
    return std::vector<uint8_t>(buf.GetData().begin(), buf.GetData().end());
}

static std::vector<uint8_t> BuildConnectPacket(const std::string& name)
{
    NetBuffer nameBuf;
    nameBuf.WriteString(name);
    return BuildPacket(MessageType::Connect, ChannelType::Reliable, 0, 0, 0.0f,
                       std::vector<uint8_t>(nameBuf.GetData().begin(), nameBuf.GetData().end()));
}

static std::vector<uint8_t> BuildChatPacket(uint32_t senderID, const std::string& name, const std::string& text)
{
    // ChatMessage payload per docs/specs/networking-wire-format.md: sender name
    // then text, both NetBuffer strings starting at payload offset 0. The server
    // screens those text fields (PacketValidator's ChatMessage schema declares
    // stringFieldOffset = 0), so a leading header byte here frame-shifts every
    // length prefix and the packet is rejected as BadString before dispatch.
    NetBuffer payload;
    payload.WriteString(name);
    payload.WriteString(text);
    return BuildPacket(MessageType::ChatMessage, ChannelType::ReliableOrdered, senderID, 0, 0.0f,
                       std::vector<uint8_t>(payload.GetData().begin(), payload.GetData().end()));
}

static std::vector<uint8_t> BuildDisconnectPacket(uint32_t senderID)
{
    return BuildPacket(MessageType::Disconnect, ChannelType::Reliable, senderID, 0, 0.0f, {});
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
// Helper: scan received packets for a specific MessageType
// ============================================================================

static bool FindPacketOfType(LiveTestClient& client, MessageType target)
{
    uint8_t buf[4096];
    for (int i = 0; i < 50; ++i)
    {
        int n = client.Receive(buf, sizeof(buf));
        if (n >= 6)
        {
            NetBuffer pkt;
            pkt.WriteBytes(buf, static_cast<size_t>(n));
            uint32_t magic = pkt.ReadUint32();
            uint16_t type = pkt.ReadUint16();
            if (magic == 0x5350524B && type == static_cast<uint16_t>(target))
                return true;
        }
    }
    return false;
}

// ============================================================================
// Helper: drain all pending packets
// ============================================================================

static void DrainPackets(LiveTestClient& client)
{
    uint8_t buf[4096];
    for (int i = 0; i < 50; ++i)
    {
        if (client.Receive(buf, sizeof(buf)) <= 0)
            break;
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

    LiveTestClient client;
    EXPECT_TRUE(client.Open());

    auto pkt = BuildConnectPacket("Tester");
    EXPECT_TRUE(client.SendTo(pkt.data(), pkt.size(), port));

    RunFrames(nm, 10);

    EXPECT_EQ(static_cast<int>(nm.GetClients().size()), 1);

    bool gotAccepted = FindPacketOfType(client, MessageType::ConnectAccepted);
    EXPECT_TRUE(gotAccepted);

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

    LiveTestClient c1, c2, c3;
    EXPECT_TRUE(c1.Open());
    EXPECT_TRUE(c2.Open());
    EXPECT_TRUE(c3.Open());

    auto p1 = BuildConnectPacket("P1");
    c1.SendTo(p1.data(), p1.size(), port);
    RunFrames(nm, 5);
    EXPECT_EQ(static_cast<int>(nm.GetClients().size()), 1);

    auto p2 = BuildConnectPacket("P2");
    c2.SendTo(p2.data(), p2.size(), port);
    RunFrames(nm, 5);
    EXPECT_EQ(static_cast<int>(nm.GetClients().size()), 2);

    auto p3 = BuildConnectPacket("P3");
    c3.SendTo(p3.data(), p3.size(), port);
    RunFrames(nm, 5);
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

    LiveTestClient alice, bob;
    EXPECT_TRUE(alice.Open());
    EXPECT_TRUE(bob.Open());

    auto connA = BuildConnectPacket("Alice");
    alice.SendTo(connA.data(), connA.size(), port);
    RunFrames(nm, 5);
    auto connB = BuildConnectPacket("Bob");
    bob.SendTo(connB.data(), connB.size(), port);
    RunFrames(nm, 5);

    EXPECT_EQ(static_cast<int>(nm.GetClients().size()), 2);

    DrainPackets(alice);
    DrainPackets(bob);

    auto chat = BuildChatPacket(1, "Alice", "Hello from Alice!");
    alice.SendTo(chat.data(), chat.size(), port);

    RunFrames(nm, 10);

    bool gotChat = FindPacketOfType(bob, MessageType::ChatMessage);
    EXPECT_TRUE(gotChat);

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
    LiveTestClient clients[NUM_CLIENTS];
    for (int i = 0; i < NUM_CLIENTS; ++i)
    {
        EXPECT_TRUE(clients[i].Open());
        auto pkt = BuildConnectPacket("Player" + std::to_string(i));
        clients[i].SendTo(pkt.data(), pkt.size(), port);
        RunFrames(nm, 5);
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
        LiveTestClient client;
        EXPECT_TRUE(client.Open());
        auto pkt = BuildConnectPacket("IDCheck");
        client.SendTo(pkt.data(), pkt.size(), port);
        RunFrames(nm, 10);

        uint8_t buf[4096];
        bool foundID = false;
        for (int i = 0; i < 50; ++i)
        {
            int n = client.Receive(buf, sizeof(buf));
            if (n < 6)
                continue;

            NetBuffer recvPkt;
            recvPkt.WriteBytes(buf, static_cast<size_t>(n));
            uint32_t magic = recvPkt.ReadUint32();
            uint16_t type = recvPkt.ReadUint16();
            if (magic == 0x5350524B && type == static_cast<uint16_t>(MessageType::ConnectAccepted))
            {
                recvPkt.ReadUint8();  // channel
                recvPkt.ReadUint32(); // sender
                recvPkt.ReadUint32(); // sequence
                recvPkt.ReadFloat();  // timestamp
                uint32_t payloadLen = recvPkt.ReadUint32();
                EXPECT_TRUE(payloadLen >= 4);

                uint32_t assignedID = recvPkt.ReadUint32();
                EXPECT_TRUE(assignedID != 0);
                foundID = true;
                break;
            }
        }
        EXPECT_TRUE(foundID);
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

    // --- Part C: Graceful disconnect ---
    {
        LiveTestClient client;
        EXPECT_TRUE(client.Open());
        auto pkt = BuildConnectPacket("DisconnectMe");
        client.SendTo(pkt.data(), pkt.size(), port);
        RunFrames(nm, 10);

        auto clients = nm.GetClients();
        EXPECT_EQ(clients.size(), static_cast<size_t>(1));
        if (!clients.empty())
        {
            ClientID myID = clients.begin()->first;
            auto discPkt = BuildDisconnectPacket(myID);
            client.SendTo(discPkt.data(), discPkt.size(), port);
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
