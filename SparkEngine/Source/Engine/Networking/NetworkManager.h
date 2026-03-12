/**
 * @file NetworkManager.h
 * @brief Multiplayer networking foundation with client/server architecture
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a UDP-based networking system for multiplayer FPS games:
 * - Client/Server architecture
 * - Entity state replication
 * - Client-side prediction and server reconciliation
 * - Lag compensation (hitbox rewinding)
 * - Reliable and unreliable message channels
 *
 * All networking code is guarded by ENABLE_NETWORKING. When the flag is
 * not defined, a minimal stub NetworkManager is provided so that the rest
 * of the engine compiles without linker errors.
 */

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <mutex>
#include <queue>
#include <cstdint>
#include <chrono>
#include <atomic>
#include <array>

#ifdef ENABLE_NETWORKING

// Platform socket headers
#ifdef SPARK_PLATFORM_WINDOWS
#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
#endif // SPARK_PLATFORM_WINDOWS

#endif // ENABLE_NETWORKING

namespace Spark::Net
{

    // ============================================================================
    // Network Types
    // ============================================================================

    using ClientID = uint32_t;
    using SequenceNumber = uint32_t;
    using NetworkTime = float;

    constexpr ClientID INVALID_CLIENT = 0;
    constexpr uint16_t DEFAULT_PORT = 27015;

    enum class ChannelType
    {
        Unreliable,     ///< Fire and forget (movement, position updates)
        Reliable,       ///< Guaranteed delivery with ordering (chat, state changes)
        ReliableOrdered ///< Guaranteed delivery in order (important game events)
    };

    enum class NetworkRole
    {
        None,
        Server,
        Client
    };

    enum class ConnectionState
    {
        Disconnected,
        Connecting,
        Connected,
        Disconnecting
    };

    // ============================================================================
    // Network Messages
    // ============================================================================

    enum class MessageType : uint16_t
    {
        // Connection
        Connect = 1,
        ConnectAccepted,
        ConnectRejected,
        Disconnect,
        Heartbeat,

        // Replication
        EntitySpawn,
        EntityDestroy,
        EntityStateUpdate,
        EntityRPC,

        // Input
        ClientInput,
        InputAck,

        // Game
        ChatMessage,
        GameStateSync,
        MatchStart,
        MatchEnd,
        PlayerRespawn,
        ScoreUpdate,

        // Custom
        UserDefined = 1000
    };

    struct NetworkMessage
    {
        MessageType type;
        ChannelType channel = ChannelType::Unreliable;
        ClientID senderID = INVALID_CLIENT;
        SequenceNumber sequence = 0;
        std::vector<uint8_t> payload;
        float timestamp = 0.0f;
    };

    // ============================================================================
    // Serialization Buffer
    // ============================================================================

    class NetBuffer
    {
      public:
        void WriteUint8(uint8_t val);
        void WriteUint16(uint16_t val);
        void WriteUint32(uint32_t val);
        void WriteFloat(float val);
        void WriteString(const std::string& val);
        void WriteVector3(const XMFLOAT3& val);
        void WriteBytes(const void* data, size_t size);

        uint8_t ReadUint8();
        uint16_t ReadUint16();
        uint32_t ReadUint32();
        float ReadFloat();
        std::string ReadString();
        XMFLOAT3 ReadVector3();
        void ReadBytes(void* data, size_t size);

        const std::vector<uint8_t>& GetData() const { return m_data; }
        size_t GetSize() const { return m_data.size(); }
        size_t GetReadPosition() const { return m_readPos; }
        size_t RemainingBytes() const { return m_readPos <= m_data.size() ? m_data.size() - m_readPos : 0; }
        bool HasError() const { return m_error; }
        bool IsValid() const { return !m_error; }

        /// @brief Check if buffer can satisfy a read of `bytes` without overrun
        bool CanRead(size_t bytes) const { return !m_error && (m_readPos + bytes <= m_data.size()); }

        void Reset()
        {
            m_data.clear();
            m_readPos = 0;
            m_error = false;
        }

      private:
        std::vector<uint8_t> m_data;
        size_t m_readPos = 0;
        bool m_error = false;
    };

    // ============================================================================
    // Entity Replication
    // ============================================================================

    struct ReplicatedProperty
    {
        std::string name;
        enum class Type
        {
            Int,
            Float,
            Vector3,
            String,
            Bool
        } type;
        std::function<void(NetBuffer&)> serialize;
        std::function<void(NetBuffer&)> deserialize;
        bool dirty = false;
    };

    struct ReplicatedEntity
    {
        uint32_t networkID;
        ClientID ownerID;
        std::string entityType;
        std::vector<ReplicatedProperty> properties;
        XMFLOAT3 position{0, 0, 0};
        XMFLOAT3 rotation{0, 0, 0};
        XMFLOAT3 velocity{0, 0, 0};
        float lastUpdateTime = 0.0f;
        bool needsFullSync = true;
    };

    // ============================================================================
    // Client Input (for prediction/reconciliation)
    // ============================================================================

    struct ClientInputState
    {
        SequenceNumber inputSequence;
        float moveForward = 0.0f;
        float moveRight = 0.0f;
        float lookYaw = 0.0f;
        float lookPitch = 0.0f;
        bool jump = false;
        bool fire = false;
        bool reload = false;
        bool sprint = false;
        bool crouch = false;
        float deltaTime = 0.0f;
        float timestamp = 0.0f;
    };

    // ============================================================================
    // Lag Compensation
    // ============================================================================

    struct HistorySnapshot
    {
        float timestamp;
        struct EntityState
        {
            uint32_t networkID;
            XMFLOAT3 position;
            XMFLOAT3 rotation;
            XMFLOAT3 boundsMin; ///< AABB for hitbox
            XMFLOAT3 boundsMax;
        };
        std::vector<EntityState> entities;
    };

    class LagCompensator
    {
      public:
        void RecordSnapshot(const HistorySnapshot& snapshot);

        /// Rewind world state to a specific time for hit validation
        bool RewindToTime(float targetTime, HistorySnapshot& outSnapshot) const;

        void SetMaxHistoryDuration(float seconds) { m_maxHistoryDuration = seconds; }
        void Clear() { m_history.clear(); }

      private:
        std::vector<HistorySnapshot> m_history;
        float m_maxHistoryDuration = 1.0f; ///< Keep 1 second of history
    };

    // ============================================================================
    // Network Statistics
    // ============================================================================

    struct NetworkStats
    {
        float ping = 0.0f;       ///< Round-trip time in ms
        float jitter = 0.0f;     ///< Ping variance in ms
        float packetLoss = 0.0f; ///< 0.0 - 1.0
        uint64_t bytesSent = 0;
        uint64_t bytesReceived = 0;
        uint32_t packetsSent = 0;
        uint32_t packetsReceived = 0;
        uint32_t packetsDropped = 0;
        float bandwidthUp = 0.0f;   ///< KB/s
        float bandwidthDown = 0.0f; ///< KB/s
    };

    // ============================================================================
    // Client Connection Info
    // ============================================================================

    struct ClientInfo
    {
        ClientID id = INVALID_CLIENT;
        std::string name;
        ConnectionState state = ConnectionState::Disconnected;
        NetworkStats stats;
        float lastHeartbeatTime = 0.0f;
        uint32_t playerEntityNetworkID = 0;
    };

    // ============================================================================
    // NetworkManager
    // ============================================================================

    // Thread safety: Queue mutex protects message I/O and handler registration.
    // Socket operations run on a dedicated network thread when connected.
    class NetworkManager
    {
      public:
        static NetworkManager& GetInstance();

        /// Initialize the networking subsystem (platform sockets).
        /// Must be called before StartServer() or Connect().
        bool Initialize();

        /// Shut down the networking subsystem and release all resources.
        void Shutdown();

        /// Initialize as server
        bool StartServer(uint16_t port = DEFAULT_PORT, int maxClients = 32);

        /// Stop the server and disconnect all clients
        void StopServer();

        /// Initialize as client and connect to server
        bool Connect(const std::string& address, uint16_t port = DEFAULT_PORT,
                     const std::string& playerName = "Player");

        /// Disconnect from server (client) or shut down server
        void Disconnect();

        /// Process incoming messages and send outgoing
        void Update(float deltaTime);

        /// Send a message to the connected server (client) or broadcast (server)
        void SendMessage(const NetworkMessage& msg);
        void SendToClient(ClientID client, const NetworkMessage& msg);
        void SendToAll(const NetworkMessage& msg);
        void SendToAllExcept(ClientID excludeClient, const NetworkMessage& msg);

        /// Broadcast a message to all connected clients (alias for SendToAll)
        void BroadcastMessage(const NetworkMessage& msg);

        /// Register message handler
        using MessageHandler = std::function<void(const NetworkMessage&)>;
        void RegisterHandler(MessageType type, MessageHandler handler);

        // Entity replication
        uint32_t RegisterReplicatedEntity(const ReplicatedEntity& entity);
        void UnregisterReplicatedEntity(uint32_t networkID);
        void MarkPropertyDirty(uint32_t networkID, const std::string& propertyName);
        ReplicatedEntity* GetReplicatedEntity(uint32_t networkID);

        /// Serialize and send full state for all replicated entities (server only)
        void SendFullEntitySync(ClientID targetClient);

        /// Serialize a single entity's replicated properties into a NetBuffer
        void SerializeEntityState(uint32_t networkID, NetBuffer& outBuffer) const;

        /// Deserialize an entity state update from a NetBuffer and apply it
        void DeserializeEntityState(NetBuffer& inBuffer);

        // Client input (for server-side processing)
        void SendClientInput(const ClientInputState& input);
        const std::vector<ClientInputState>& GetPendingInputs() const { return m_pendingInputs; }

        // Lag compensation
        LagCompensator& GetLagCompensator() { return m_lagCompensator; }

        // State queries
        NetworkRole GetRole() const { return m_role; }
        ConnectionState GetConnectionState() const { return m_connectionState; }
        ClientID GetLocalClientID() const { return m_localClientID; }
        float GetServerTime() const { return m_serverTime; }
        const NetworkStats& GetStats() const { return m_stats; }
        bool IsInitialized() const { return m_initialized; }

        // Client management (server only)
        const std::unordered_map<ClientID, ClientInfo>& GetClients() const { return m_clients; }
        void KickClient(ClientID client, const std::string& reason = "");

        /// Console integration
        std::string Console_GetStatus() const;
        std::string Console_ListClients() const;
        std::string Console_GetStats() const;

      private:
        NetworkManager() = default;
        ~NetworkManager();

        // Non-copyable
        NetworkManager(const NetworkManager&) = delete;
        NetworkManager& operator=(const NetworkManager&) = delete;

        void ProcessIncoming();
        void ProcessOutgoing();
        void UpdateReplication(float deltaTime);
        void UpdateHeartbeat(float deltaTime);
        void HandleConnect(const NetworkMessage& msg);
        void HandleDisconnect(const NetworkMessage& msg);

#ifdef ENABLE_NETWORKING
        /// Create, bind, and configure a non-blocking UDP socket
        bool CreateSocket(uint16_t port);

        /// Close the socket
        void CloseSocket();

        /// Serialize a NetworkMessage into raw bytes for the wire
        std::vector<uint8_t> SerializeMessage(const NetworkMessage& msg) const;

        /// Deserialize raw bytes into a NetworkMessage
        bool DeserializeMessage(const uint8_t* data, size_t length, NetworkMessage& outMsg) const;

        /// Send raw bytes to a specific address
        bool SendRawTo(const std::vector<uint8_t>& data, const sockaddr_in& addr);

        /// Receive raw data from socket (non-blocking)
        int ReceiveRaw(std::vector<uint8_t>& outData, sockaddr_in& outSender);

        SOCKET m_socket = INVALID_SOCKET;
        sockaddr_in m_serverAddress{};

        /// Map of client ID to their socket address (server-side)
        std::unordered_map<ClientID, sockaddr_in> m_clientAddresses;
#endif // ENABLE_NETWORKING

        bool m_initialized = false;
        NetworkRole m_role = NetworkRole::None;
        ConnectionState m_connectionState = ConnectionState::Disconnected;
        ClientID m_localClientID = INVALID_CLIENT;
        float m_serverTime = 0.0f;
        float m_heartbeatInterval = 1.0f;
        float m_heartbeatTimer = 0.0f;
        float m_connectionTimeout = 10.0f; ///< Seconds before a client is considered timed out

        NetworkStats m_stats;
        LagCompensator m_lagCompensator;

        // Clients (server-side)
        std::unordered_map<ClientID, ClientInfo> m_clients;
        ClientID m_nextClientID = 1;
        int m_maxClients = 32;

        // Messages
        std::queue<NetworkMessage> m_outgoingQueue;
        std::queue<NetworkMessage> m_incomingQueue;
        std::unordered_map<uint16_t, MessageHandler> m_handlers;
        mutable std::mutex m_queueMutex;
        mutable std::mutex m_handlerMutex;

        // Reliable message tracking
        SequenceNumber m_nextOutgoingSequence = 1;
        std::unordered_map<SequenceNumber, NetworkMessage> m_unacknowledgedMessages;
        float m_reliableRetransmitInterval = 0.5f;

        // Replication
        std::unordered_map<uint32_t, ReplicatedEntity> m_replicatedEntities;
        uint32_t m_nextNetworkID = 1;
        float m_replicationInterval = 0.05f; ///< 20 Hz replication rate
        float m_replicationTimer = 0.0f;

        // Client input
        std::vector<ClientInputState> m_pendingInputs;
        SequenceNumber m_inputSequence = 0;

        // Prediction
        std::vector<ClientInputState> m_inputHistory; ///< For client-side prediction

        // Bandwidth tracking
        std::chrono::steady_clock::time_point m_lastBandwidthSample;
        uint64_t m_bytesSentSinceSample = 0;
        uint64_t m_bytesReceivedSinceSample = 0;
    };

} // namespace Spark::Net

// =============================================================================
// Stub NetworkManager when networking is disabled
// =============================================================================

#ifndef ENABLE_NETWORKING

namespace Spark::Net
{
    /// Minimal no-op NetworkManager so the rest of the engine links without
    /// requiring the full networking implementation.
    class NetworkManagerStub
    {
      public:
        static NetworkManagerStub& GetInstance()
        {
            static NetworkManagerStub instance;
            return instance;
        }

        bool Initialize() { return false; }
        void Shutdown() {}
        bool StartServer(uint16_t /*port*/ = 27015, int /*maxClients*/ = 32) { return false; }
        void StopServer() {}
        bool Connect(const std::string& /*address*/, uint16_t /*port*/ = 27015,
                     const std::string& /*playerName*/ = "Player")
        {
            return false;
        }
        void Disconnect() {}
        void Update(float /*deltaTime*/) {}
        NetworkRole GetRole() const { return NetworkRole::None; }
        ConnectionState GetConnectionState() const { return ConnectionState::Disconnected; }
        bool IsInitialized() const { return false; }
    };

} // namespace Spark::Net

#endif // !ENABLE_NETWORKING
