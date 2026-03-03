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
 */

#pragma once

#include <DirectXMath.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <mutex>
#include <queue>
#include <cstdint>
#include <chrono>

using namespace DirectX;

namespace Spark::Net {

// ============================================================================
// Network Types
// ============================================================================

using ClientID = uint32_t;
using SequenceNumber = uint32_t;
using NetworkTime = float;

constexpr ClientID INVALID_CLIENT = 0;
constexpr uint16_t DEFAULT_PORT = 27015;

enum class ChannelType {
    Unreliable,        ///< Fire and forget (movement, position updates)
    Reliable,          ///< Guaranteed delivery with ordering (chat, state changes)
    ReliableOrdered    ///< Guaranteed delivery in order (important game events)
};

enum class NetworkRole {
    None,
    Server,
    Client
};

enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Disconnecting
};

// ============================================================================
// Network Messages
// ============================================================================

enum class MessageType : uint16_t {
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

struct NetworkMessage {
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

class NetBuffer {
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
    void Reset() { m_data.clear(); m_readPos = 0; }

private:
    std::vector<uint8_t> m_data;
    size_t m_readPos = 0;
};

// ============================================================================
// Entity Replication
// ============================================================================

struct ReplicatedProperty {
    std::string name;
    enum class Type { Int, Float, Vector3, String, Bool } type;
    std::function<void(NetBuffer&)> serialize;
    std::function<void(NetBuffer&)> deserialize;
    bool dirty = false;
};

struct ReplicatedEntity {
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

struct ClientInputState {
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

struct HistorySnapshot {
    float timestamp;
    struct EntityState {
        uint32_t networkID;
        XMFLOAT3 position;
        XMFLOAT3 rotation;
        XMFLOAT3 boundsMin;  ///< AABB for hitbox
        XMFLOAT3 boundsMax;
    };
    std::vector<EntityState> entities;
};

class LagCompensator {
public:
    void RecordSnapshot(const HistorySnapshot& snapshot);

    /// Rewind world state to a specific time for hit validation
    bool RewindToTime(float targetTime, HistorySnapshot& outSnapshot) const;

    void SetMaxHistoryDuration(float seconds) { m_maxHistoryDuration = seconds; }
    void Clear() { m_history.clear(); }

private:
    std::vector<HistorySnapshot> m_history;
    float m_maxHistoryDuration = 1.0f;  ///< Keep 1 second of history
};

// ============================================================================
// Network Statistics
// ============================================================================

struct NetworkStats {
    float ping = 0.0f;              ///< Round-trip time in ms
    float jitter = 0.0f;            ///< Ping variance in ms
    float packetLoss = 0.0f;        ///< 0.0 - 1.0
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
    uint32_t packetsSent = 0;
    uint32_t packetsReceived = 0;
    uint32_t packetsDropped = 0;
    float bandwidthUp = 0.0f;       ///< KB/s
    float bandwidthDown = 0.0f;     ///< KB/s
};

// ============================================================================
// Client Connection Info
// ============================================================================

struct ClientInfo {
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

class NetworkManager {
public:
    static NetworkManager& GetInstance();

    /// Initialize as server
    bool StartServer(uint16_t port = DEFAULT_PORT, int maxClients = 32);

    /// Initialize as client and connect to server
    bool Connect(const std::string& address, uint16_t port = DEFAULT_PORT,
                 const std::string& playerName = "Player");

    /// Disconnect from server (client) or shut down server
    void Disconnect();

    /// Process incoming messages and send outgoing
    void Update(float deltaTime);

    /// Send a message
    void SendMessage(const NetworkMessage& msg);
    void SendToClient(ClientID client, const NetworkMessage& msg);
    void SendToAll(const NetworkMessage& msg);
    void SendToAllExcept(ClientID excludeClient, const NetworkMessage& msg);

    /// Register message handler
    using MessageHandler = std::function<void(const NetworkMessage&)>;
    void RegisterHandler(MessageType type, MessageHandler handler);

    // Entity replication
    uint32_t RegisterReplicatedEntity(const ReplicatedEntity& entity);
    void UnregisterReplicatedEntity(uint32_t networkID);
    void MarkPropertyDirty(uint32_t networkID, const std::string& propertyName);
    ReplicatedEntity* GetReplicatedEntity(uint32_t networkID);

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

    // Client management (server only)
    const std::unordered_map<ClientID, ClientInfo>& GetClients() const { return m_clients; }
    void KickClient(ClientID client, const std::string& reason = "");

    /// Console integration
    std::string Console_GetStatus() const;
    std::string Console_ListClients() const;
    std::string Console_GetStats() const;

private:
    NetworkManager() = default;

    void ProcessIncoming();
    void ProcessOutgoing();
    void UpdateReplication(float deltaTime);
    void UpdateHeartbeat(float deltaTime);
    void HandleConnect(const NetworkMessage& msg);
    void HandleDisconnect(const NetworkMessage& msg);

    NetworkRole m_role = NetworkRole::None;
    ConnectionState m_connectionState = ConnectionState::Disconnected;
    ClientID m_localClientID = INVALID_CLIENT;
    float m_serverTime = 0.0f;
    float m_heartbeatInterval = 1.0f;
    float m_heartbeatTimer = 0.0f;

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

    // Replication
    std::unordered_map<uint32_t, ReplicatedEntity> m_replicatedEntities;
    uint32_t m_nextNetworkID = 1;

    // Client input
    std::vector<ClientInputState> m_pendingInputs;
    SequenceNumber m_inputSequence = 0;

    // Prediction
    std::vector<ClientInputState> m_inputHistory;  ///< For client-side prediction
};

} // namespace Spark::Net
