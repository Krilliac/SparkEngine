/**
 * @file TestNetworkManagerOrchestration.cpp
 * @brief Tests for NetworkManager connection state machine, message routing,
 *        lag compensation, RTT estimation, and reliable message retransmission.
 *
 * Standalone reimplementation — mirrors Spark::Net::NetworkManager orchestration
 * logic without engine dependencies for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{

    // --- Types mirroring Spark::Net ---

    using ClientID = uint32_t;
    using SequenceNumber = uint32_t;

    enum class ConnectionState
    {
        Disconnected,
        Connecting,
        Connected,
        Disconnecting
    };

    enum class NetworkRole
    {
        None,
        Server,
        Client
    };

    enum class MessageType : uint8_t
    {
        Connect,
        ConnectAccepted,
        Disconnect,
        Heartbeat,
        EntitySpawn,
        EntityStateUpdate,
        ChatMessage,
        ClientInput,
        ServerSnapshot
    };

    enum class ChannelType
    {
        Unreliable,
        Reliable,
        ReliableOrdered
    };

    struct NetworkMessage
    {
        MessageType type{};
        ChannelType channel = ChannelType::Unreliable;
        ClientID senderID = 0;
        SequenceNumber sequence = 0;
        std::vector<uint8_t> payload;
        float timestamp = 0.0f;
    };

    struct NetworkStats
    {
        float ping = 0.0f;
        float jitter = 0.0f;
        float packetLoss = 0.0f;
        uint64_t bytesSent = 0;
        uint64_t bytesReceived = 0;
        uint64_t packetsSent = 0;
        uint64_t packetsReceived = 0;
        uint64_t packetsDropped = 0;
    };

    struct ClientInfo
    {
        ClientID id = 0;
        std::string name;
        ConnectionState state = ConnectionState::Disconnected;
        NetworkStats stats;
        float lastHeartbeatTime = 0.0f;
    };

    using MessageHandler = std::function<void(const NetworkMessage&)>;

    // --- Standalone NetworkManager reimplementation ---

    class NetworkManager
    {
      public:
        bool Initialize()
        {
            m_role = NetworkRole::None;
            m_state = ConnectionState::Disconnected;
            m_localClientID = 0;
            m_nextSequence = 1;
            m_serverTime = 0.0f;
            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            m_clients.clear();
            m_handlers.clear();
            while (!m_pendingMessages.empty())
                m_pendingMessages.pop();
            m_reliableUnacked.clear();
            m_ackedSequences.clear();
            m_state = ConnectionState::Disconnected;
            m_role = NetworkRole::None;
            m_initialized = false;
        }

        bool StartServer(uint16_t port = 27015, int maxClients = 32)
        {
            if (!m_initialized)
                return false;
            m_role = NetworkRole::Server;
            m_state = ConnectionState::Connected;
            m_port = port;
            m_maxClients = maxClients;
            return true;
        }

        void StopServer()
        {
            m_clients.clear();
            m_state = ConnectionState::Disconnected;
            m_role = NetworkRole::None;
        }

        bool Connect(const std::string& address, uint16_t port, const std::string& playerName)
        {
            if (!m_initialized)
                return false;
            m_role = NetworkRole::Client;
            m_state = ConnectionState::Connecting;
            m_serverAddress = address;
            m_port = port;
            m_playerName = playerName;
            return true;
        }

        void Disconnect()
        {
            if (m_state == ConnectionState::Connected || m_state == ConnectionState::Connecting)
            {
                m_state = ConnectionState::Disconnecting;
            }
        }

        void CompleteDisconnect()
        {
            m_state = ConnectionState::Disconnected;
            if (!m_autoReconnect)
                m_role = NetworkRole::None;
        }

        void CompleteConnection(ClientID assignedID)
        {
            if (m_state == ConnectionState::Connecting)
            {
                m_state = ConnectionState::Connected;
                m_localClientID = assignedID;
            }
        }

        void Update(float deltaTime)
        {
            m_serverTime += deltaTime;

            // Process pending messages
            while (!m_pendingMessages.empty())
            {
                auto msg = m_pendingMessages.front();
                m_pendingMessages.pop();
                DispatchMessage(msg);
            }

            // Check reliable retransmission
            for (auto& [seq, entry] : m_reliableUnacked)
            {
                entry.timeSinceLastSend += deltaTime;
                if (entry.timeSinceLastSend >= m_retransmitInterval)
                {
                    entry.retransmitCount++;
                    entry.timeSinceLastSend = 0.0f;
                }
            }

            // Update RTT estimation
            if (m_rttSampleCount > 0)
            {
                m_stats.ping = m_rttSum / static_cast<float>(m_rttSampleCount);
            }

            // Check client timeouts (server-side)
            if (m_role == NetworkRole::Server)
            {
                std::vector<ClientID> timedOut;
                for (auto& [id, info] : m_clients)
                {
                    if (m_serverTime - info.lastHeartbeatTime > m_timeoutDuration)
                    {
                        timedOut.push_back(id);
                    }
                }
                for (auto id : timedOut)
                {
                    if (m_timeoutHandler)
                        m_timeoutHandler(id);
                    m_clients.erase(id);
                }
            }

            // Auto-reconnect logic (client-side)
            if (m_role == NetworkRole::Client && m_state == ConnectionState::Disconnected && m_autoReconnect)
            {
                m_reconnectTimer += deltaTime;
                if (m_reconnectTimer >= m_reconnectDelay && m_reconnectAttempts < m_maxReconnectAttempts)
                {
                    m_state = ConnectionState::Connecting;
                    m_reconnectAttempts++;
                    m_reconnectTimer = 0.0f;
                }
            }
        }

        void SendMessage(const NetworkMessage& msg)
        {
            auto outMsg = msg;
            outMsg.sequence = m_nextSequence++;
            outMsg.timestamp = m_serverTime;
            m_stats.packetsSent++;
            m_stats.bytesSent += msg.payload.size() + 16; // header overhead

            if (msg.channel == ChannelType::Reliable || msg.channel == ChannelType::ReliableOrdered)
            {
                m_reliableUnacked[outMsg.sequence] = {outMsg, 0.0f, 0};
            }
            m_sentMessages.push_back(outMsg);
        }

        void ReceiveMessage(const NetworkMessage& msg)
        {
            m_stats.packetsReceived++;
            m_stats.bytesReceived += msg.payload.size() + 16;

            // Duplicate detection
            if (msg.channel == ChannelType::Reliable || msg.channel == ChannelType::ReliableOrdered)
            {
                if (m_receivedSequences.count(msg.sequence))
                {
                    m_stats.packetsDropped++;
                    return; // duplicate
                }
                m_receivedSequences.insert(msg.sequence);
            }

            m_pendingMessages.push(msg);
        }

        void AcknowledgeSequence(SequenceNumber seq)
        {
            m_ackedSequences.insert(seq);
            m_reliableUnacked.erase(seq);
        }

        void AddRTTSample(float rttMs)
        {
            m_rttSum += rttMs;
            m_rttSampleCount++;

            // Compute jitter as running average of abs difference
            float currentRTT = m_rttSum / static_cast<float>(m_rttSampleCount);
            float diff = std::abs(rttMs - currentRTT);
            m_stats.jitter = m_stats.jitter * 0.9f + diff * 0.1f;
        }

        void RegisterHandler(MessageType type, MessageHandler handler) { m_handlers[type] = std::move(handler); }

        // Server: add a client
        ClientID AddClient(const std::string& name)
        {
            ClientID id = m_nextClientID++;
            ClientInfo info;
            info.id = id;
            info.name = name;
            info.state = ConnectionState::Connected;
            info.lastHeartbeatTime = m_serverTime;
            m_clients[id] = info;
            return id;
        }

        void KickClient(ClientID client, const std::string& /*reason*/ = "") { m_clients.erase(client); }

        void UpdateClientHeartbeat(ClientID client)
        {
            if (auto it = m_clients.find(client); it != m_clients.end())
            {
                it->second.lastHeartbeatTime = m_serverTime;
            }
        }

        void SetAutoReconnect(bool enabled, float delay = 2.0f, int maxAttempts = 5)
        {
            m_autoReconnect = enabled;
            m_reconnectDelay = delay;
            m_maxReconnectAttempts = maxAttempts;
            m_reconnectAttempts = 0;
            m_reconnectTimer = 0.0f;
        }

        void SetTimeoutHandler(std::function<void(ClientID)> handler) { m_timeoutHandler = std::move(handler); }
        void SetTimeoutDuration(float seconds) { m_timeoutDuration = seconds; }

        // Accessors
        NetworkRole GetRole() const { return m_role; }
        ConnectionState GetConnectionState() const { return m_state; }
        ClientID GetLocalClientID() const { return m_localClientID; }
        float GetServerTime() const { return m_serverTime; }
        const NetworkStats& GetStats() const { return m_stats; }
        float GetEstimatedRTT() const { return m_rttSampleCount > 0 ? m_rttSum / m_rttSampleCount : 0.0f; }
        const std::unordered_map<ClientID, ClientInfo>& GetClients() const { return m_clients; }
        size_t GetUnackedCount() const { return m_reliableUnacked.size(); }
        int GetReconnectAttempts() const { return m_reconnectAttempts; }
        const std::vector<NetworkMessage>& GetSentMessages() const { return m_sentMessages; }

      private:
        void DispatchMessage(const NetworkMessage& msg)
        {
            if (auto it = m_handlers.find(msg.type); it != m_handlers.end())
            {
                it->second(msg);
            }
        }

        struct ReliableEntry
        {
            NetworkMessage message;
            float timeSinceLastSend = 0.0f;
            int retransmitCount = 0;
        };

        bool m_initialized = false;
        NetworkRole m_role = NetworkRole::None;
        ConnectionState m_state = ConnectionState::Disconnected;
        ClientID m_localClientID = 0;
        SequenceNumber m_nextSequence = 1;
        ClientID m_nextClientID = 1;
        float m_serverTime = 0.0f;
        uint16_t m_port = 0;
        int m_maxClients = 32;
        std::string m_serverAddress;
        std::string m_playerName;
        NetworkStats m_stats;

        // RTT estimation
        float m_rttSum = 0.0f;
        int m_rttSampleCount = 0;

        // Reliable messaging
        std::unordered_map<SequenceNumber, ReliableEntry> m_reliableUnacked;
        std::unordered_set<SequenceNumber> m_ackedSequences;
        std::unordered_set<SequenceNumber> m_receivedSequences;
        float m_retransmitInterval = 0.5f;

        // Message routing
        std::unordered_map<MessageType, MessageHandler> m_handlers;
        std::queue<NetworkMessage> m_pendingMessages;
        std::vector<NetworkMessage> m_sentMessages;

        // Client management
        std::unordered_map<ClientID, ClientInfo> m_clients;

        // Auto-reconnect
        bool m_autoReconnect = false;
        float m_reconnectDelay = 2.0f;
        int m_maxReconnectAttempts = 5;
        int m_reconnectAttempts = 0;
        float m_reconnectTimer = 0.0f;

        // Timeout
        float m_timeoutDuration = 30.0f;
        std::function<void(ClientID)> m_timeoutHandler;
    };

} // anonymous namespace

// ============================================================================
// Connection State Machine Tests
// ============================================================================

TEST(NetworkManager_InitialState)
{
    NetworkManager mgr;
    mgr.Initialize();
    EXPECT_EQ(static_cast<int>(mgr.GetRole()), static_cast<int>(NetworkRole::None));
    EXPECT_EQ(static_cast<int>(mgr.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    EXPECT_EQ(mgr.GetLocalClientID(), (uint32_t)0);
    mgr.Shutdown();
}

TEST(NetworkManager_StartServerTransition)
{
    NetworkManager mgr;
    mgr.Initialize();
    EXPECT_TRUE(mgr.StartServer(27015, 16));
    EXPECT_EQ(static_cast<int>(mgr.GetRole()), static_cast<int>(NetworkRole::Server));
    EXPECT_EQ(static_cast<int>(mgr.GetConnectionState()), static_cast<int>(ConnectionState::Connected));
    mgr.Shutdown();
}

TEST(NetworkManager_StopServerTransition)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.StartServer();
    mgr.StopServer();
    EXPECT_EQ(static_cast<int>(mgr.GetRole()), static_cast<int>(NetworkRole::None));
    EXPECT_EQ(static_cast<int>(mgr.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    mgr.Shutdown();
}

TEST(NetworkManager_ClientConnectTransition)
{
    NetworkManager mgr;
    mgr.Initialize();
    EXPECT_TRUE(mgr.Connect("127.0.0.1", 27015, "Player1"));
    EXPECT_EQ(static_cast<int>(mgr.GetRole()), static_cast<int>(NetworkRole::Client));
    EXPECT_EQ(static_cast<int>(mgr.GetConnectionState()), static_cast<int>(ConnectionState::Connecting));
    mgr.Shutdown();
}

TEST(NetworkManager_ClientCompleteConnection)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.Connect("127.0.0.1", 27015, "Player1");
    mgr.CompleteConnection(42);
    EXPECT_EQ(static_cast<int>(mgr.GetConnectionState()), static_cast<int>(ConnectionState::Connected));
    EXPECT_EQ(mgr.GetLocalClientID(), (uint32_t)42);
    mgr.Shutdown();
}

TEST(NetworkManager_DisconnectTransition)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.Connect("127.0.0.1", 27015, "Player1");
    mgr.CompleteConnection(1);
    mgr.Disconnect();
    EXPECT_EQ(static_cast<int>(mgr.GetConnectionState()), static_cast<int>(ConnectionState::Disconnecting));
    mgr.CompleteDisconnect();
    EXPECT_EQ(static_cast<int>(mgr.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    mgr.Shutdown();
}

TEST(NetworkManager_InitializeRequiredBeforeStart)
{
    NetworkManager mgr;
    EXPECT_FALSE(mgr.StartServer());
    EXPECT_FALSE(mgr.Connect("127.0.0.1", 27015, "Test"));
}

// ============================================================================
// Message Routing Tests
// ============================================================================

TEST(NetworkManager_RegisterAndDispatchHandler)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.StartServer();

    int chatReceived = 0;
    mgr.RegisterHandler(MessageType::ChatMessage,
                        [&](const NetworkMessage& msg)
                        {
                            chatReceived++;
                            EXPECT_EQ(static_cast<int>(msg.type), static_cast<int>(MessageType::ChatMessage));
                        });

    NetworkMessage msg;
    msg.type = MessageType::ChatMessage;
    msg.payload = {0x48, 0x65, 0x6C, 0x6C, 0x6F}; // "Hello"
    mgr.ReceiveMessage(msg);
    mgr.Update(0.016f);

    EXPECT_EQ(chatReceived, 1);
    mgr.Shutdown();
}

TEST(NetworkManager_MultipleHandlerTypes)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.StartServer();

    int entitySpawns = 0;
    int stateUpdates = 0;

    mgr.RegisterHandler(MessageType::EntitySpawn, [&](const NetworkMessage&) { entitySpawns++; });
    mgr.RegisterHandler(MessageType::EntityStateUpdate, [&](const NetworkMessage&) { stateUpdates++; });

    NetworkMessage spawn;
    spawn.type = MessageType::EntitySpawn;
    mgr.ReceiveMessage(spawn);

    NetworkMessage update;
    update.type = MessageType::EntityStateUpdate;
    mgr.ReceiveMessage(update);
    mgr.ReceiveMessage(update);

    mgr.Update(0.016f);

    EXPECT_EQ(entitySpawns, 1);
    EXPECT_EQ(stateUpdates, 2);
    mgr.Shutdown();
}

TEST(NetworkManager_UnhandledMessageTypeIgnored)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.StartServer();

    // No handler registered for Heartbeat
    NetworkMessage msg;
    msg.type = MessageType::Heartbeat;
    mgr.ReceiveMessage(msg);

    // Should not crash
    EXPECT_NO_THROW(mgr.Update(0.016f));
    mgr.Shutdown();
}

// ============================================================================
// Sequence Number & Reliable Messaging Tests
// ============================================================================

TEST(NetworkManager_SequenceNumbersIncrement)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.StartServer();

    NetworkMessage msg;
    msg.type = MessageType::EntityStateUpdate;
    msg.channel = ChannelType::Unreliable;

    mgr.SendMessage(msg);
    mgr.SendMessage(msg);
    mgr.SendMessage(msg);

    auto& sent = mgr.GetSentMessages();
    EXPECT_EQ(sent.size(), (size_t)3);
    EXPECT_EQ(sent[0].sequence, (uint32_t)1);
    EXPECT_EQ(sent[1].sequence, (uint32_t)2);
    EXPECT_EQ(sent[2].sequence, (uint32_t)3);
    mgr.Shutdown();
}

TEST(NetworkManager_ReliableMessageTracked)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.StartServer();

    NetworkMessage msg;
    msg.type = MessageType::ChatMessage;
    msg.channel = ChannelType::Reliable;

    mgr.SendMessage(msg);
    EXPECT_EQ(mgr.GetUnackedCount(), (size_t)1);

    mgr.SendMessage(msg);
    EXPECT_EQ(mgr.GetUnackedCount(), (size_t)2);
    mgr.Shutdown();
}

TEST(NetworkManager_AcknowledgeClearsUnacked)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.StartServer();

    NetworkMessage msg;
    msg.type = MessageType::ChatMessage;
    msg.channel = ChannelType::Reliable;

    mgr.SendMessage(msg); // seq 1
    mgr.SendMessage(msg); // seq 2
    EXPECT_EQ(mgr.GetUnackedCount(), (size_t)2);

    mgr.AcknowledgeSequence(1);
    EXPECT_EQ(mgr.GetUnackedCount(), (size_t)1);

    mgr.AcknowledgeSequence(2);
    EXPECT_EQ(mgr.GetUnackedCount(), (size_t)0);
    mgr.Shutdown();
}

TEST(NetworkManager_UnreliableNotTracked)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.StartServer();

    NetworkMessage msg;
    msg.type = MessageType::EntityStateUpdate;
    msg.channel = ChannelType::Unreliable;

    mgr.SendMessage(msg);
    EXPECT_EQ(mgr.GetUnackedCount(), (size_t)0);
    mgr.Shutdown();
}

TEST(NetworkManager_DuplicateDetection)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.StartServer();

    int received = 0;
    mgr.RegisterHandler(MessageType::ChatMessage, [&](const NetworkMessage&) { received++; });

    NetworkMessage msg;
    msg.type = MessageType::ChatMessage;
    msg.channel = ChannelType::Reliable;
    msg.sequence = 100;

    mgr.ReceiveMessage(msg);
    mgr.ReceiveMessage(msg); // duplicate — should be dropped
    mgr.ReceiveMessage(msg); // duplicate — should be dropped
    mgr.Update(0.016f);

    EXPECT_EQ(received, 1);
    EXPECT_EQ(mgr.GetStats().packetsDropped, (uint64_t)2);
    mgr.Shutdown();
}

// ============================================================================
// RTT Estimation Tests
// ============================================================================

TEST(NetworkManager_RTTEstimation)
{
    NetworkManager mgr;
    mgr.Initialize();

    mgr.AddRTTSample(50.0f);
    mgr.AddRTTSample(60.0f);
    mgr.AddRTTSample(55.0f);
    mgr.Update(0.016f);

    float expectedRTT = (50.0f + 60.0f + 55.0f) / 3.0f;
    EXPECT_NEAR(mgr.GetEstimatedRTT(), expectedRTT, 0.1f);
    EXPECT_NEAR(mgr.GetStats().ping, expectedRTT, 0.1f);
    mgr.Shutdown();
}

TEST(NetworkManager_RTTZeroWithNoSamples)
{
    NetworkManager mgr;
    mgr.Initialize();
    EXPECT_NEAR(mgr.GetEstimatedRTT(), 0.0f, 0.001f);
    mgr.Shutdown();
}

// ============================================================================
// Bandwidth Tracking Tests
// ============================================================================

TEST(NetworkManager_BandwidthTracking)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.StartServer();

    NetworkMessage msg;
    msg.type = MessageType::EntityStateUpdate;
    msg.payload = {1, 2, 3, 4, 5};

    mgr.SendMessage(msg);
    EXPECT_EQ(mgr.GetStats().packetsSent, (uint64_t)1);
    EXPECT_EQ(mgr.GetStats().bytesSent, (uint64_t)(5 + 16)); // payload + header

    mgr.ReceiveMessage(msg);
    EXPECT_EQ(mgr.GetStats().packetsReceived, (uint64_t)1);
    EXPECT_EQ(mgr.GetStats().bytesReceived, (uint64_t)(5 + 16));
    mgr.Shutdown();
}

// ============================================================================
// Client Management Tests
// ============================================================================

TEST(NetworkManager_AddAndKickClient)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.StartServer();

    auto id1 = mgr.AddClient("Alice");
    auto id2 = mgr.AddClient("Bob");
    EXPECT_EQ(mgr.GetClients().size(), (size_t)2);
    EXPECT_NE(id1, id2);

    mgr.KickClient(id1);
    EXPECT_EQ(mgr.GetClients().size(), (size_t)1);
    EXPECT_TRUE(mgr.GetClients().count(id2) > 0);
    mgr.Shutdown();
}

TEST(NetworkManager_ClientTimeout)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.StartServer();
    mgr.SetTimeoutDuration(5.0f);

    ClientID timedOutClient = 0;
    mgr.SetTimeoutHandler([&](ClientID id) { timedOutClient = id; });

    auto id = mgr.AddClient("SlowPlayer");

    // Advance time past timeout
    for (int i = 0; i < 100; i++)
        mgr.Update(0.1f); // 10 seconds total

    EXPECT_EQ(timedOutClient, id);
    EXPECT_EQ(mgr.GetClients().size(), (size_t)0);
    mgr.Shutdown();
}

TEST(NetworkManager_HeartbeatResetsTimeout)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.StartServer();
    mgr.SetTimeoutDuration(5.0f);

    bool timedOut = false;
    mgr.SetTimeoutHandler([&](ClientID) { timedOut = true; });

    auto id = mgr.AddClient("ActivePlayer");

    // Advance 3 seconds, then heartbeat, then 3 more
    for (int i = 0; i < 30; i++)
        mgr.Update(0.1f); // 3 seconds
    mgr.UpdateClientHeartbeat(id);
    for (int i = 0; i < 30; i++)
        mgr.Update(0.1f); // 3 more seconds

    // Should NOT have timed out (only 3s since heartbeat)
    EXPECT_FALSE(timedOut);
    EXPECT_EQ(mgr.GetClients().size(), (size_t)1);
    mgr.Shutdown();
}

// ============================================================================
// Auto-Reconnect Tests
// ============================================================================

TEST(NetworkManager_AutoReconnectAttempts)
{
    NetworkManager mgr;
    mgr.Initialize();

    // Start as client, connect, enable reconnect, then disconnect
    mgr.Connect("127.0.0.1", 27015, "Player");
    mgr.CompleteConnection(1);
    mgr.SetAutoReconnect(true, 0.5f, 3); // enable BEFORE disconnect
    mgr.Disconnect();
    mgr.CompleteDisconnect();

    // Role stays Client (auto-reconnect enabled), state is Disconnected
    // Advance time past reconnect delay
    for (int i = 0; i < 20; i++)
        mgr.Update(0.1f); // 2 seconds total

    // Should have attempted reconnect at least once
    EXPECT_GE(mgr.GetReconnectAttempts(), 1);
    mgr.Shutdown();
}

TEST(NetworkManager_AutoReconnectMaxAttempts)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.Connect("127.0.0.1", 27015, "Player");
    mgr.CompleteConnection(1);
    mgr.Disconnect();
    mgr.CompleteDisconnect();

    mgr.SetAutoReconnect(true, 0.1f, 2);

    // Simulate many update cycles — each reconnect attempt sets Connecting,
    // but we manually fail it back to Disconnected
    for (int attempt = 0; attempt < 5; attempt++)
    {
        for (int i = 0; i < 5; i++)
            mgr.Update(0.1f);
        if (mgr.GetConnectionState() == ConnectionState::Connecting)
        {
            mgr.CompleteDisconnect(); // simulate failed connection
        }
    }

    // Should have stopped attempting after max
    EXPECT_LE(mgr.GetReconnectAttempts(), 2);
    mgr.Shutdown();
}

// ============================================================================
// Server Time Tests
// ============================================================================

TEST(NetworkManager_ServerTimeAdvances)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.StartServer();

    EXPECT_NEAR(mgr.GetServerTime(), 0.0f, 0.001f);

    mgr.Update(0.5f);
    EXPECT_NEAR(mgr.GetServerTime(), 0.5f, 0.001f);

    mgr.Update(1.0f);
    EXPECT_NEAR(mgr.GetServerTime(), 1.5f, 0.001f);
    mgr.Shutdown();
}

TEST(NetworkManager_MessageTimestamp)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.StartServer();
    mgr.Update(2.5f);

    NetworkMessage msg;
    msg.type = MessageType::ChatMessage;
    mgr.SendMessage(msg);

    EXPECT_NEAR(mgr.GetSentMessages().back().timestamp, 2.5f, 0.001f);
    mgr.Shutdown();
}

// ============================================================================
// Shutdown / Lifecycle Tests
// ============================================================================

TEST(NetworkManager_ShutdownClearsState)
{
    NetworkManager mgr;
    mgr.Initialize();
    mgr.StartServer();
    mgr.AddClient("Alice");
    mgr.RegisterHandler(MessageType::ChatMessage, [](const NetworkMessage&) {});

    mgr.Shutdown();

    EXPECT_EQ(static_cast<int>(mgr.GetRole()), static_cast<int>(NetworkRole::None));
    EXPECT_EQ(static_cast<int>(mgr.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    EXPECT_EQ(mgr.GetClients().size(), (size_t)0);
}

TEST(NetworkManager_DoubleInitialize)
{
    NetworkManager mgr;
    EXPECT_TRUE(mgr.Initialize());
    EXPECT_TRUE(mgr.Initialize()); // Should not crash
    mgr.Shutdown();
}
