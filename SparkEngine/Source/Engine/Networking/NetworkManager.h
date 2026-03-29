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
#include "NetworkInterpolation.h"

#ifdef ENABLE_NETWORKING

// Platform socket headers
#ifdef SPARK_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
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

// Windows headers define SendMessage as a macro (SendMessageA/SendMessageW).
// Undefine it so our NetworkManager::SendMessage method compiles correctly.
// This must be outside the ENABLE_NETWORKING guard because the class and
// its methods are always declared.
#ifdef SendMessage
#undef SendMessage
#endif

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

        // Reliability
        Ack, ///< Acknowledges receipt of reliable messages (carries sequence + bitfield)

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
        MessageType type;                              ///< What kind of network event this message represents.
        ChannelType channel = ChannelType::Unreliable; ///< Delivery guarantee (reliable ordered, unreliable, etc.).
        ClientID senderID = INVALID_CLIENT; ///< Client that originated this message (INVALID on server-sent).
        SequenceNumber sequence = 0;        ///< Monotonic counter for reliable-ordered delivery.
        std::vector<uint8_t> payload;       ///< Raw serialized message body.
        float timestamp = 0.0f;             ///< Server time when the message was created (seconds).
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
        std::string name; ///< Property identifier (must match between client and server).
        enum class Type
        {
            Int,
            Float,
            Vector3,
            String,
            Bool
        } type;                                      ///< Wire-format type discriminator.
        std::function<void(NetBuffer&)> serialize;   ///< Writes the current value into a NetBuffer.
        std::function<void(NetBuffer&)> deserialize; ///< Reads and applies a value from a NetBuffer.
        bool dirty = false;                          ///< True when the value has changed since last replication.
    };

    struct ReplicatedEntity
    {
        uint32_t networkID;                         ///< Unique network-wide entity identifier.
        ClientID ownerID;                           ///< Client that owns/controls this entity.
        std::string entityType;                     ///< Type name for spawning on remote clients.
        std::vector<ReplicatedProperty> properties; ///< Replicated property list (delta-compressed).
        XMFLOAT3 position{0, 0, 0};                 ///< Last known world-space position.
        XMFLOAT3 rotation{0, 0, 0};                 ///< Last known euler rotation (degrees).
        XMFLOAT3 velocity{0, 0, 0};                 ///< Velocity for dead-reckoning extrapolation.
        float lastUpdateTime = 0.0f;                ///< Server time of the most recent state update.
        bool needsFullSync = true;                  ///< True = send all properties, not just dirty ones.

        /// @brief Client-side interpolation buffer for smooth remote entity rendering.
        NetworkInterpolationBuffer interpolationBuffer;
    };

    // ============================================================================
    // Client Input (for prediction/reconciliation)
    // ============================================================================

    struct ClientInputState
    {
        SequenceNumber inputSequence; ///< Monotonic input frame number (for server reconciliation).
        float moveForward = 0.0f;     ///< Forward/backward axis [-1, 1] (W/S keys).
        float moveRight = 0.0f;       ///< Strafe axis [-1, 1] (A/D keys).
        float lookYaw = 0.0f;         ///< Horizontal look delta (degrees).
        float lookPitch = 0.0f;       ///< Vertical look delta (degrees).
        bool jump = false;            ///< Jump button pressed this frame.
        bool fire = false;            ///< Primary fire button held.
        bool reload = false;          ///< Reload button pressed this frame.
        bool sprint = false;          ///< Sprint button held.
        bool crouch = false;          ///< Crouch button held.
        float deltaTime = 0.0f;       ///< Client frame delta (seconds) for deterministic replay.
        float timestamp = 0.0f;       ///< Client-local time when this input was sampled.
    };

    // ============================================================================
    // Lag Compensation
    // ============================================================================

    struct HistorySnapshot
    {
        float timestamp; ///< Server time this snapshot was recorded.
        struct EntityState
        {
            uint32_t networkID; ///< Network ID of the entity in this state.
            XMFLOAT3 position;  ///< World-space position at snapshot time.
            XMFLOAT3 rotation;  ///< Euler rotation at snapshot time.
            XMFLOAT3 boundsMin; ///< AABB minimum for hitbox rewinding.
            XMFLOAT3 boundsMax; ///< AABB maximum for hitbox rewinding.
        };
        std::vector<EntityState> entities; ///< All entity states captured this frame.
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
        float ping = 0.0f;            ///< Round-trip time in ms
        float jitter = 0.0f;          ///< Ping variance in ms
        float packetLoss = 0.0f;      ///< 0.0 - 1.0
        uint64_t bytesSent = 0;       ///< Total bytes transmitted since connection.
        uint64_t bytesReceived = 0;   ///< Total bytes received since connection.
        uint32_t packetsSent = 0;     ///< Total UDP packets sent.
        uint32_t packetsReceived = 0; ///< Total UDP packets received.
        uint32_t packetsDropped = 0;  ///< Packets detected as lost (sequence gaps).
        float bandwidthUp = 0.0f;     ///< KB/s
        float bandwidthDown = 0.0f;   ///< KB/s
    };

    // ============================================================================
    // Client Connection Info
    // ============================================================================

    struct ClientInfo
    {
        ClientID id = INVALID_CLIENT;                          ///< Unique client identifier assigned on connect.
        std::string name;                                      ///< Player display name.
        ConnectionState state = ConnectionState::Disconnected; ///< Current connection lifecycle state.
        NetworkStats stats;                                    ///< Per-client network statistics.
        float lastHeartbeatTime = 0.0f;                        ///< Server time of most recent heartbeat (for timeout).
        uint32_t playerEntityNetworkID = 0;                    ///< Network ID of this client's player entity.
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

        // State queries (thread-safe)
        NetworkRole GetRole() const { return m_role.load(std::memory_order_acquire); }
        ConnectionState GetConnectionState() const
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            return m_connectionState;
        }
        ClientID GetLocalClientID() const
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            return m_localClientID;
        }
        float GetServerTime() const { return m_serverTime; }
        const NetworkStats& GetStats() const { return m_stats; }
        bool IsInitialized() const { return m_initialized; }

        // Client management (server only)
        const std::unordered_map<ClientID, ClientInfo>& GetClients() const { return m_clients; }
        void KickClient(ClientID client, const std::string& reason = "");

        /// Register a callback for client timeout events (server-side).
        void SetTimeoutHandler(std::function<void(ClientID)> handler) { m_timeoutHandler = std::move(handler); }

        /// Get estimated round-trip time in milliseconds.
        float GetEstimatedRTT() const { return m_smoothedRTT * 1000.0f; }

        // ====================================================================
        // Auto-reconnect (client-side)
        // ====================================================================

        /** @brief Configuration for automatic reconnection after disconnection */
        struct AutoReconnectConfig
        {
            bool enabled = false;     ///< Whether auto-reconnect is active
            float baseDelay = 2.0f;   ///< Base delay before first retry (seconds)
            float maxDelay = 30.0f;   ///< Maximum backoff delay cap (seconds)
            uint32_t maxAttempts = 5; ///< Max reconnect attempts (0 = unlimited)
        };

        /** @brief Enable/configure automatic reconnection after disconnection */
        void SetAutoReconnect(const AutoReconnectConfig& config);

        /** @brief Get current auto-reconnect configuration */
        const AutoReconnectConfig& GetAutoReconnectConfig() const { return m_autoReconnect; }

        /** @brief Register a callback invoked when auto-reconnect gives up */
        void SetReconnectFailedCallback(std::function<void()> callback)
        {
            m_reconnectFailedCallback = std::move(callback);
        }

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
        void FlushOutgoingQueue();
        void HandleRetransmissions();
        std::vector<ClientID> GetConnectedClientIDs() const;
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

        std::atomic<bool> m_initialized{false};
        std::atomic<NetworkRole> m_role{NetworkRole::None};
        ConnectionState m_connectionState = ConnectionState::Disconnected;
        ClientID m_localClientID = INVALID_CLIENT;
        /// @brief Protects m_connectionState, m_localClientID.
        /// m_role is std::atomic so it can be safely read without the mutex.
        /// Lock ordering: m_stateMutex → m_clientsMutex → m_queueMutex → m_replicationMutex → m_inputMutex → m_handlerMutex (never reverse).
        mutable std::mutex m_stateMutex;
        float m_serverTime = 0.0f;
        float m_heartbeatInterval = 1.0f;
        float m_heartbeatTimer = 0.0f;
        float m_connectionTimeout = 10.0f; ///< Seconds before a client is considered timed out

        NetworkStats m_stats;
        LagCompensator m_lagCompensator;

        // Clients (server-side)
        std::unordered_map<ClientID, ClientInfo> m_clients;
        mutable std::mutex m_clientsMutex; ///< Protects m_clients, m_nextClientID
        ClientID m_nextClientID = 1;
        int m_maxClients = 32;

        // Messages
        std::queue<NetworkMessage> m_outgoingQueue;
        std::queue<NetworkMessage> m_incomingQueue;
        std::unordered_map<uint16_t, MessageHandler> m_handlers;
        mutable std::mutex m_queueMutex;   ///< Protects m_outgoingQueue, m_incomingQueue
        mutable std::mutex m_handlerMutex; ///< Protects m_handlers (lowest in lock order)

        // Reliable message tracking
        SequenceNumber m_nextOutgoingSequence = 1;
        std::unordered_map<SequenceNumber, NetworkMessage> m_unacknowledgedMessages;
        std::unordered_map<SequenceNumber, float>
            m_reliableOriginalSendTime; ///< Tracks when each reliable msg was first sent
        /// @brief Per-message retransmit count for exponential backoff.
        std::unordered_map<SequenceNumber, int> m_retransmitCounts;
        float m_reliableRetransmitInterval = 0.5f; ///< Base interval; doubles per retry

        // RTT estimation (Jacobson/Karels algorithm)
        float m_smoothedRTT = 0.0f;    ///< SRTT (smoothed round-trip time)
        float m_rttVariance = 0.0f;    ///< RTTVAR (RTT variance)
        bool m_rttInitialized = false; ///< First RTT sample taken?

        /// Update RTT estimate from an ACK (called when an ACKed message's send time is known)
        void UpdateRTTEstimate(float sampleRTT);

        /// Get the adaptive retransmit timeout (RTO)
        float GetRetransmitTimeout() const;

        // Connection timeout — handler + notification
        using TimeoutHandler = std::function<void(ClientID)>;
        TimeoutHandler m_timeoutHandler; ///< Called when a client times out (server) or server times out (client)

        /// @brief Checks heartbeat freshness and fires timeout callbacks.
        void CheckConnectionTimeouts();

        // ACK processing — tracks the highest received reliable sequence and a
        // 32-bit bitfield of the previous 32 sequences for cumulative ACKs.
        SequenceNumber m_remoteSequenceHighest = 0;        ///< Highest reliable sequence received
        uint32_t m_ackBitfield = 0;                        ///< Bitfield for sequences (highest-1) to (highest-32)
        float m_ackSendTimer = 0.0f;                       ///< Accumulate before sending ACK
        static constexpr float ACK_SEND_INTERVAL = 0.033f; ///< ~30 Hz ACK rate

        // Duplicate detection — set of recently received reliable sequence numbers.
        // Bounded to prevent unbounded growth; old entries pruned periodically.
        std::unordered_map<SequenceNumber, float> m_receivedSequences; ///< seq → receive time
        static constexpr float RECEIVED_SEQ_EXPIRY = 30.0f;            ///< Prune after 30s

        // Ordered delivery — buffer for out-of-order ReliableOrdered messages.
        SequenceNumber m_expectedOrderedSequence = 1; ///< Next sequence to deliver in order
        std::unordered_map<SequenceNumber, NetworkMessage> m_orderedBuffer;

        /// Process an incoming ACK message: remove acknowledged messages from unack map
        void HandleAck(const NetworkMessage& msg);

        /// Send a cumulative ACK packet to the remote endpoint
        void SendAckPacket();

        /// Check for duplicate reliable message
        bool IsDuplicateSequence(SequenceNumber seq) const;

        /// Record a received reliable sequence and update ACK bitfield
        void RecordReceivedSequence(SequenceNumber seq);

        /// Deliver buffered ordered messages that are now in-sequence
        void FlushOrderedBuffer();

        /// Prune old entries from m_receivedSequences
        void PruneReceivedSequences();

        // Replication
        mutable std::mutex m_replicationMutex; ///< Protects m_replicatedEntities
        std::unordered_map<uint32_t, ReplicatedEntity> m_replicatedEntities;
        std::atomic<uint32_t> m_nextNetworkID{1};
        float m_replicationInterval = 0.05f; ///< 20 Hz replication rate
        float m_replicationTimer = 0.0f;

        // Client input
        std::vector<ClientInputState> m_pendingInputs;
        mutable std::mutex m_inputMutex; ///< Protects m_pendingInputs and m_inputHistory
        SequenceNumber m_inputSequence = 0;

        // Prediction
        std::vector<ClientInputState> m_inputHistory; ///< For client-side prediction

        // Bandwidth tracking
        std::chrono::steady_clock::time_point m_lastBandwidthSample;
        uint64_t m_bytesSentSinceSample = 0;
        uint64_t m_bytesReceivedSinceSample = 0;

        // Auto-reconnect state
        AutoReconnectConfig m_autoReconnect;
        uint32_t m_reconnectAttempts = 0;                ///< Current reconnect attempt count
        float m_reconnectNextRetryTime = 0.0f;           ///< Server time when next reconnect allowed
        std::string m_lastServerAddress;                 ///< Last server address for reconnect
        uint16_t m_lastServerPort = 0;                   ///< Last server port for reconnect
        std::string m_lastPlayerName;                    ///< Last player name for reconnect
        bool m_wasConnected = false;                     ///< True if we were connected before disconnect
        std::function<void()> m_reconnectFailedCallback; ///< Called when max attempts exhausted

        /** @brief Attempt automatic reconnection (called from Update) */
        void TryAutoReconnect(float deltaTime);
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
