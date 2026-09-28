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
 * When ENABLE_NETWORKING is not defined, NetworkManager retains its
 * deterministic in-process lifecycle, message queues, and state-machine
 * behavior, but never opens a native socket or exposes a bound port. This
 * lets non-network builds exercise local orchestration without claiming an
 * OS-backed network endpoint exists.
 */

#pragma once
#include "../../Core/Platform.h"
#include "NetworkBindPolicy.h"
#include "NetworkClientId.h"
#include "Spark/ServiceInterfaces.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <deque>
#include <cstdint>
#include <chrono>
#include <atomic>
#include <array>
#include "NetworkInterpolation.h"
#include "NetworkTrustStore.h"
#include "NetworkWireLimits.h"
#include "PacketValidator.h"

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
#ifndef SPARK_HAS_CLOSESOCKET_SHIM
#define SPARK_HAS_CLOSESOCKET_SHIM 1
inline int closesocket(SOCKET s)
{
    return ::close(s);
} // Winsock name -> POSIX close
#endif
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

    using SequenceNumber = uint32_t;
    using NetworkTime = float;

    /// Successor of a reliable-channel sequence. Reliable streams start at 1 and a
    /// sequence of 0 is never tracked (it bypasses dedup, ACK and ordering), so a
    /// stream that runs past 0xFFFFFFFF continues at 1 instead of emitting 0.
    [[nodiscard]] constexpr SequenceNumber NextReliableSequence(SequenceNumber sequence) noexcept
    {
        return sequence == 0xFFFFFFFFu ? 1u : sequence + 1u;
    }

    /// Return @p next and advance it with NextReliableSequence.
    [[nodiscard]] constexpr SequenceNumber TakeReliableSequence(SequenceNumber& next) noexcept
    {
        const SequenceNumber taken = next;
        next = NextReliableSequence(next);
        return taken;
    }

    /// RFC 1982 serial-number comparison: true when @p lhs is newer than @p rhs,
    /// including across the uint32 wrap (half-range window).
    [[nodiscard]] constexpr bool IsSequenceNewer(SequenceNumber lhs, SequenceNumber rhs) noexcept
    {
        return lhs != rhs && static_cast<uint32_t>(lhs - rhs) < 0x80000000u;
    }

    constexpr uint16_t DEFAULT_PORT = 27015;

    /// Largest client count one server endpoint accepts. Configuration surfaces (SparkServer
    /// CLI and INI, DedicatedServer) validate against this so an operator value outside
    /// [1, MAX_SERVER_CLIENTS] is a configuration error, never a runtime abort.
    constexpr int MAX_SERVER_CLIENTS = 256;

    /// Magic that opens every Connect payload ("SPNH", Spark network handshake). A Connect
    /// without it predates protocol negotiation and is rejected as ProtocolMissing.
    constexpr uint32_t NETWORK_HANDSHAKE_MAGIC = 0x484E5053;

    /// Session protocol version carried in Connect (inside the ClientHello, so it is bound into
    /// the signed handshake transcript) and echoed in ConnectAccepted. Peers must match exactly;
    /// bump it on every incompatible wire change (docs/specs/networking-wire-format.md).
    /// Version 2 (NET-100): every datagram is framed, and everything but the handshake is sealed.
    constexpr uint16_t NETWORK_PROTOCOL_VERSION = 2;

    /// Outer frame kind, the first byte of every v2 datagram (NET-100).
    /// Handshake frames carry only Connect, ConnectAccepted and ConnectRejected in plaintext;
    /// every other message travels in a Sealed frame: [0x02][SecureChannel packet], whose
    /// plaintext is the serialized message and whose associated data is the frame-kind byte.
    constexpr uint8_t NETWORK_FRAME_HANDSHAKE = 0x01;
    constexpr uint8_t NETWORK_FRAME_SEALED = 0x02;

    /// Send-key rotation policy for a SecureChannel (NET-100 nonce discipline). A sender rotates
    /// after this many sealed packets or this much session time, whichever comes first; once the
    /// 8-bit epoch space is exhausted the channel is dropped and the session must be re-established.
    constexpr uint64_t SECURE_ROTATE_AFTER_PACKETS = uint64_t{1} << 31;
    constexpr float SECURE_ROTATE_AFTER_SECONDS = 600.0f;

    /// Typed reason carried in the ConnectRejected trailer (and recorded for client-side refusals).
    enum class ConnectRejectReason : uint8_t
    {
        Unspecified = 0,            ///< Legacy or malformed rejection with no typed trailer
        ServerFull = 1,             ///< Every client slot is occupied
        ProtocolMissing = 2,        ///< Connect lacks the handshake magic and version
        ProtocolTooOld = 3,         ///< Client protocol version is older than the server's
        ProtocolTooNew = 4,         ///< Client protocol version is newer than the server's
        MalformedHandshake = 5,     ///< Magic and version present but the rest of the payload is malformed
        ProtocolMismatch = 6,       ///< Client-side: ConnectAccepted echoed a different version
        UnsupportedSuite = 7,       ///< ClientHello names a cipher suite the server does not speak
        ServerIdentityMismatch = 8, ///< Client-side: the server's key is not the pinned / recorded one
        HandshakeAuthFailed = 9     ///< Client-side: bad signature, weak key, or unusable trust store
    };

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

    /** @brief Immutable snapshot consumed by first-party discovery endpoints. */
    struct NetworkDiscoveryConfiguration
    {
        NetworkEndpointPolicy endpointPolicy{};
        bool active = false;
        bool allowAdvertisement = false;
    };

    enum class ConnectionState
    {
        Disconnected,
        Connecting,
        Connected,
        Disconnecting,
        Securing ///< Server-side: handshake answered, waiting for the client's sealed ClientFinished
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

        // Delta replication acknowledgement — client echo of the last applied
        // delta sequence. Deltas number their own per-connection sequence space
        // (DeltaSnapshotManager), distinct from the reliable-channel sequences
        // acknowledged by MessageType::Ack; the two must never be mixed.
        DeltaAck,

        // NET-100: first sealed client message; carries the player name and completes admission.
        ClientFinished,

        // Custom
        UserDefined = 1000
    };

    struct NetworkMessage
    {
        NetworkMessage() = default;
        NetworkMessage(const NetworkMessage&) = default;
        NetworkMessage(NetworkMessage&& other) noexcept;
        NetworkMessage& operator=(const NetworkMessage& other);
        NetworkMessage& operator=(NetworkMessage&& other) noexcept;
        ~NetworkMessage();

        /** Promptly overwrite an owned sensitive payload and revoke its sensitive marker. */
        void ClearSensitivePayload() noexcept;

        MessageType type = MessageType::UserDefined;   ///< What kind of network event this message represents.
        ChannelType channel = ChannelType::Unreliable; ///< Delivery guarantee (reliable ordered, unreliable, etc.).
        ClientID senderID = INVALID_CLIENT; ///< Client that originated this message (INVALID on server-sent).
        SequenceNumber sequence = 0;        ///< Monotonic counter for reliable-ordered delivery.
        std::vector<uint8_t> payload;       ///< Raw serialized message body.
        float timestamp = 0.0f;             ///< Server time when the message was created (seconds).
        bool sensitive = false;             ///< Sensitive-payload ownership marker; never serialized onto the network.
        bool localOnly =
            false; ///< Refuse transmission to non-loopback destinations; never serialized onto the network.
        uint64_t ownerLifecycleEpoch = 0; ///< Owning connection lifecycle; process-local and never serialized.
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

        /** Overwrite buffered bytes before resetting cursor and error state. */
        void SecureReset() noexcept;

      private:
        std::vector<uint8_t> m_data;
        size_t m_readPos = 0;
        bool m_error = false;
    };

    /// True for the three messages that travel in plaintext Handshake frames.
    [[nodiscard]] constexpr bool IsHandshakeMessage(MessageType type) noexcept
    {
        return type == MessageType::Connect || type == MessageType::ConnectAccepted ||
               type == MessageType::ConnectRejected;
    }

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
        uint32_t networkID = 0;                     ///< Unique network-wide entity identifier.
        ClientID ownerID = INVALID_CLIENT;          ///< Client that owns/controls this entity.
        std::string entityType;                     ///< Type name for spawning on remote clients.
        std::vector<ReplicatedProperty> properties; ///< Replicated property list (delta-compressed).
        XMFLOAT3 position{0, 0, 0};                 ///< Last known world-space position.
        XMFLOAT3 rotation{0, 0, 0};                 ///< Last known euler rotation (degrees).
        XMFLOAT3 velocity{0, 0, 0};                 ///< Velocity for dead-reckoning extrapolation.
        float lastUpdateTime = 0.0f;                ///< Server time of the most recent state update.
        bool needsFullSync = true;                  ///< True = send all properties, not just dirty ones.

        /// Scope metadata — consumed by ConnectionScopeFilter during replication.
        /// Defaults match-all (areaId=0 means global, masks all bits set) so
        /// entities remain fully replicated until a connection scope is set.
        uint32_t areaId = 0;                   ///< Area bucket for area-based scope filtering (0 = global).
        uint32_t teamMask = 0xFFFFFFFFu;       ///< Team bitmask; must share a bit with the connection scope.
        uint32_t visibilityMask = 0xFFFFFFFFu; ///< Visibility flag bitmask; must share a bit with the scope.

        /// @brief Client-side interpolation buffer for smooth remote entity rendering.
        NetworkInterpolationBuffer interpolationBuffer;
    };

    /**
     * @brief Atomic field patch for a replicated entity.
     *
     * Only populated fields are changed. This value-only API lets callers update
     * replication state without borrowing a pointer or reference to the manager's
     * internal entity map.
     */
    struct ReplicatedEntityUpdate
    {
        std::optional<XMFLOAT3> position;
        std::optional<XMFLOAT3> rotation;
        std::optional<XMFLOAT3> velocity;
        std::optional<uint32_t> areaId;
        std::optional<bool> needsFullSync;
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
        [[nodiscard]] float GetMaxHistoryDuration() const { return m_maxHistoryDuration; }
        void Clear() { m_history.clear(); }

        /// Timestamp of the most recently recorded snapshot — the server's own view
        /// of "now" for hit validation. Callers bound client-supplied timestamps
        /// against this instead of trusting them.
        /// @param outNewest Receives the newest timestamp when history is non-empty.
        /// @return false when no snapshot has been recorded yet.
        [[nodiscard]] bool GetNewestSnapshotTime(float& outNewest) const
        {
            if (m_history.empty())
                return false;
            outNewest = m_history.back().timestamp;
            for (const auto& snapshot : m_history)
            {
                if (snapshot.timestamp > outNewest)
                    outNewest = snapshot.timestamp;
            }
            return true;
        }

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
        uint32_t correctionCount = 0; ///< Number of client prediction corrections observed.
        uint32_t fullEntitySyncs = 0; ///< Initial full snapshots sent to admitted clients.
        float bandwidthUp = 0.0f;     ///< KB/s
        float bandwidthDown = 0.0f;   ///< KB/s

        /// NET-100: sealed frames dropped by SecureChannel::Open, indexed by OpenResult
        /// (Malformed, UnsupportedVersion, UnknownKeyEpoch, AuthenticationFailed, Replayed; [0] unused).
        std::array<uint32_t, 6> securityDrops{};
        uint32_t plaintextFramesDropped = 0;     ///< Unframed, unknown-kind or out-of-state plaintext frames refused
        uint32_t unsealedSendsRefused = 0;       ///< Outgoing non-handshake messages with no SecureChannel to seal them
        uint32_t keyRotations = 0;               ///< Send-key rotations performed
        uint32_t handshakeFailures = 0;          ///< Handshakes refused or abandoned (either role)
        uint32_t handshakeResponsesComputed = 0; ///< Server: ClientHellos that reached RespondToClientHello
        uint32_t connectsRateLimited = 0;        ///< Server: unadmitted Connects dropped by ConnectRateLimiter
        uint32_t unadmittedSendsRefused = 0;     ///< Server: non-handshake sends refused to a non-Connected slot

        /// OPS-110: message-queue occupancy, read live by NetworkManager::GetStats (never part of a
        /// diagnostics snapshot). Depths are the queue sizes when the stats were read; peaks are the
        /// largest size since Initialize. Unreliable traffic is capped at kMaxQueuedMessages but
        /// reliable traffic is not, so a peak that keeps rising across a soak is unbounded growth.
        size_t incomingQueueDepth = 0;
        size_t outgoingQueueDepth = 0;
        size_t incomingQueuePeak = 0;
        size_t outgoingQueuePeak = 0;
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

    // Thread safety: all public state/lifecycle operations are serialized by
    // m_apiMutex. Update owns the socket pump; there is no hidden network
    // thread. Narrow container mutexes remain for internal lock granularity.
    class NetworkManager : public Spark::INetworkService
    {
      public:
        static NetworkManager& GetInstance();

        /// Upper bound on queued Unreliable messages per direction. A hostile or buggy peer that
        /// spams packets cannot grow the queues past it; reliable traffic is never dropped here.
        static constexpr size_t kMaxQueuedMessages = 4096;

        /// Initialize the networking subsystem (platform sockets).
        /// Must be called before StartServer() or Connect().
        bool Initialize() override;

        /// Shut down the networking subsystem and release all resources.
        void Shutdown() override;

        /**
         * @brief Replace the transport security configuration (NET-100)
         *
         * The configuration is not lifecycle state: it survives Shutdown(),
         * StopServer() and Disconnect(). StartServer() refuses without an identity
         * and Connect() refuses without a usable ServerTrust, so there is no
         * unauthenticated mode to fall back to.
         */
        void SetSecurityConfig(NetworkSecurityConfig config);

        /// Copy of the current security configuration (includes the server secret; handle with care).
        [[nodiscard]] NetworkSecurityConfig GetSecurityConfig() const;

        /**
         * @brief Fill in only the missing security configuration from per-user defaults
         *
         * Server role: an unset identity is loaded or created at
         * DefaultNetworkSecurityDirectory()/server_identity.key. Client role: an
         * unusable trust becomes trust-on-first-use in .../known_hosts. Anything a
         * caller configured explicitly (SetSecurityConfig) is kept.
         *
         * @param role Server or Client
         * @return false (and logs) when no default can be established; the caller must not start
         */
        [[nodiscard]] bool UseDefaultSecurityConfig(NetworkRole role);

        /// Initialize as server
        bool StartServer(uint16_t port = DEFAULT_PORT, int maxClients = 32);
        bool StartServer(uint16_t port, int maxClients, const NetworkEndpointPolicy& endpointPolicy);
        bool StartServer(uint16_t port, int maxClients, const NetworkEndpointPolicy& endpointPolicy,
                         bool allowLanAdvertisement);

        /// Stop the server and disconnect all clients
        void StopServer();

        /// Initialize as client and connect to server
        bool Connect(const std::string& address, uint16_t port = DEFAULT_PORT,
                     const std::string& playerName = "Player");
        bool Connect(const std::string& address, uint16_t port, const std::string& playerName,
                     const NetworkEndpointPolicy& endpointPolicy);

        /// Disconnect from server (client) or shut down server
        void Disconnect();

        /// Process incoming messages and send outgoing
        void Update(float deltaTime) override;

        /// Send a message to the connected server (client) or broadcast (server)
        void SendMessage(const NetworkMessage& msg);
        void SendToClient(ClientID client, const NetworkMessage& msg);
        void SendToAll(const NetworkMessage& msg);
        void SendToAllExcept(ClientID excludeClient, const NetworkMessage& msg);

        /// Broadcast a message to all connected clients (alias for SendToAll)
        void BroadcastMessage(const NetworkMessage& msg);

        /// Register or replace an application observer for a message type.
        /// Mandatory protocol handlers are owned separately and cannot be replaced.
        using MessageHandler = std::function<void(const NetworkMessage&)>;
        void RegisterHandler(MessageType type, MessageHandler handler);
        /** Register a handler whose received payload copies must be erased on release. */
        void RegisterSensitiveHandler(MessageType type, MessageHandler handler);
        /// Remove every application observer. Inside a ScopedRegistrationOwner only that owner's are removed.
        void ClearHandlers();

        /**
         * @brief Remove the application observer for @p type.
         *
         * Inside a ScopedRegistrationOwner, a slot owned by a different owner is left in place, so an
         * outgoing module image cannot remove what its hot-reload replacement registered.
         */
        void UnregisterHandler(MessageType type);

        /**
         * @brief Remove every application observer and the timeout handler registered under @p ownerId.
         *
         * ModuleManager calls this after a module's OnUnload and before its image is unmapped, so no
         * std::function whose invoker or destructor lives in that image outlives it. Handler copies taken
         * by Update() exist only for the duration of one dispatch on the game thread; module reload and
         * unload run on the game thread outside Update(), so no copy is in flight when the image goes away.
         *
         * @return Number of observers removed (the timeout handler counts as one).
         */
        size_t UnregisterHandlersByOwner(const std::string& ownerId);

        /**
         * @brief Attributes application handler writes on @p manager to @p ownerId for the scope's lifetime.
         *
         * Every RegisterHandler / RegisterSensitiveHandler / SetTimeoutHandler inside the scope records
         * @p ownerId as the slot's owner. Removal (UnregisterHandler, ClearHandlers, clearing the timeout
         * handler) never touches a slot owned by a different owner. A @p teardown scope additionally
         * refuses to replace another owner's slot, so a module's OnUnload cannot overwrite the handler
         * its already-initialized replacement installed. Scopes nest; the destructor restores the previous
         * owner. Thread affinity: game thread (module lifecycle).
         */
        class ScopedRegistrationOwner final
        {
          public:
            ScopedRegistrationOwner(NetworkManager& manager, std::string ownerId, bool teardown = false);
            ~ScopedRegistrationOwner();

            ScopedRegistrationOwner(const ScopedRegistrationOwner&) = delete;
            ScopedRegistrationOwner& operator=(const ScopedRegistrationOwner&) = delete;

          private:
            NetworkManager& m_manager;
            std::string m_previousOwner;
            bool m_previousTeardown = false;
        };

        // Entity replication
        uint32_t RegisterReplicatedEntity(const ReplicatedEntity& entity);
        void UnregisterReplicatedEntity(uint32_t networkID);
        void MarkPropertyDirty(uint32_t networkID, const std::string& propertyName);
        /// Return a value snapshot that remains valid across concurrent mutation or unregister.
        [[nodiscard]] std::optional<ReplicatedEntity> GetReplicatedEntitySnapshot(uint32_t networkID) const;
        /// Atomically apply populated fields without exposing internal map storage.
        [[nodiscard]] bool UpdateReplicatedEntity(uint32_t networkID, const ReplicatedEntityUpdate& update);

        /// Serialize and send full state for all replicated entities (server only)
        void SendFullEntitySync(ClientID targetClient);

        /// Initial full syncs Update starts per call. Each costs O(replicated entities)
        /// plus two reliable messages per entity, so admissions beyond this wait for
        /// later Updates instead of letting a connect flood multiply that work per frame.
        static constexpr size_t kMaxFullSyncsPerUpdate = 4;

        /// Serialize a single entity's replicated properties into a NetBuffer
        void SerializeEntityState(uint32_t networkID, NetBuffer& outBuffer) const;

        /// Deserialize an entity state update from a NetBuffer and apply it. Truncated or
        /// non-finite transforms are rejected without touching the entity, and unknown
        /// IDs create placeholders only while fewer than kMaxReplicatedEntities exist.
        void DeserializeEntityState(NetBuffer& inBuffer);

        /// Upper bound on entities a client tracks. A server EntityStateUpdate for an
        /// unknown network ID creates a placeholder; past this cap it is dropped, so a
        /// stream of unique IDs cannot grow client memory without limit.
        static constexpr size_t kMaxReplicatedEntities = 16384;

        /// Client -> server input send. The transport does not retain received
        /// ClientInput: a server consumes it through an application observer
        /// (RegisterHandler), which owns validation, attribution and bounding.
        void SendClientInput(const ClientInputState& input);

        // Lag compensation. [game thread] Borrowed mutable subsystem reference.
        LagCompensator& GetLagCompensator() { return m_lagCompensator; }

        // State queries (thread-safe)
        NetworkRole GetRole() const { return m_role.load(std::memory_order_acquire); }
        [[nodiscard]] NetworkDiscoveryConfiguration GetDiscoveryConfiguration() const;
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
        /// Last server-supplied reason for rejecting a connection attempt.
        std::string GetLastConnectionError() const
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            return m_lastConnectionError;
        }
        /// Typed reason for the last rejected or refused connection attempt.
        ConnectRejectReason GetLastConnectRejectReason() const
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            return m_lastConnectRejectReason;
        }
        float GetServerTime() const
        {
            std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
            return m_serverTime;
        }
        NetworkStats GetStats() const
        {
            std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
            NetworkStats stats = m_stats;
            // Documented lock order: m_apiMutex before m_queueMutex, never reversed.
            std::lock_guard<std::mutex> queueLock(m_queueMutex);
            stats.incomingQueueDepth = m_incomingQueue.size();
            stats.outgoingQueueDepth = m_outgoingQueue.size();
            stats.incomingQueuePeak = m_incomingQueuePeak;
            stats.outgoingQueuePeak = m_outgoingQueuePeak;
            return stats;
        }
        /// Replace the telemetry snapshot (diagnostic adapters/tests only).
        /// This never changes transport, connection, or wire state.
        void SetStatsSnapshotForDiagnostics(const NetworkStats& stats)
        {
            std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
            m_stats = stats;
        }
        void SetPredictionCorrectionCount(uint32_t correctionCount)
        {
            std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
            m_stats.correctionCount = correctionCount;
        }
        bool IsInitialized() const override { return m_initialized; }

        // Client management (server only)
        /**
         * @brief [any thread] Admitted clients only (state Connected)
         *
         * A slot still Securing (handshake answered, no ClientFinished yet) is not a player:
         * it has no name and has not proven its channel, so game code never sees it here.
         */
        std::unordered_map<ClientID, ClientInfo> GetClients() const
        {
            std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
            std::lock_guard<std::mutex> clientsLock(m_clientsMutex);
            std::unordered_map<ClientID, ClientInfo> admitted;
            for (const auto& [id, info] : m_clients)
            {
                if (info.state == ConnectionState::Connected)
                {
                    admitted.emplace(id, info);
                }
            }
            return admitted;
        }
        /** @brief [any thread] Every occupied slot, including Securing ones (diagnostics and tests). */
        std::unordered_map<ClientID, ClientInfo> GetClientSlots() const
        {
            std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
            std::lock_guard<std::mutex> clientsLock(m_clientsMutex);
            return m_clients;
        }
        uint16_t GetBoundPort() const;
        /** [any thread, thread-safe] True only for a known client whose endpoint is in IPv4 127/8. */
        [[nodiscard]] bool IsClientLoopback(ClientID client) const;
        void KickClient(ClientID client, const std::string& reason = "");

        // ====================================================================
        // Per-connection interest management (server-side bandwidth filter)
        // ====================================================================

        /**
         * @brief Set the visibility scope for a connected client (server only).
         *
         * Registers the client with ConnectionScopeFilter so that entity
         * replication (SendFullEntitySync and the delta-update loop) filters
         * entities outside the sphere centered at @p position with @p radius.
         * Subsequent calls replace any existing scope for this client.
         *
         * @param client   Target connection ID.
         * @param position World-space center of the visibility sphere (usually the player entity position).
         * @param radius   Visibility radius in world units.
         * @param areaId   Area bucket filter (0 = global; entities with matching areaId will pass).
         * @param teamMask Team bitmask filter (must share a bit with the entity's team mask).
         * @param visibilityMask Custom visibility bitmask (must share a bit with the entity's mask).
         */
        void SetClientScope(ClientID client, const XMFLOAT3& position, float radius, uint32_t areaId = 0,
                            uint32_t teamMask = 0xFFFFFFFFu, uint32_t visibilityMask = 0xFFFFFFFFu);

        /// @brief Remove a client's visibility scope, reverting to "see everything".
        void ClearClientScope(ClientID client);

        /// Register (or, with an empty handler, clear) the callback for client timeout events (server-side).
        /// A client whose server goes silent or closes the session instead transitions to Disconnected
        /// (GetConnectionState/GetLastConnectionError) and auto-reconnect takes over.
        /// Honors the ScopedRegistrationOwner ownership rules described on RegisterHandler's scope.
        void SetTimeoutHandler(std::function<void(ClientID)> handler);

        /// Get estimated round-trip time in milliseconds.
        float GetEstimatedRTT() const
        {
            std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
            return m_smoothedRTT * 1000.0f;
        }

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
        AutoReconnectConfig GetAutoReconnectConfig() const
        {
            std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
            return m_autoReconnect;
        }

        /** @brief Register a callback invoked when auto-reconnect gives up */
        void SetReconnectFailedCallback(std::function<void()> callback)
        {
            std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
            m_reconnectFailedCallback = std::move(callback);
        }

        /// Set the maximum number of retransmissions before a reliable message is dropped
        void SetMaxReliableRetries(int maxRetries)
        {
            std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
            m_maxReliableRetries = maxRetries;
        }

        /// Get the maximum number of retransmissions
        int GetMaxReliableRetries() const
        {
            std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
            return m_maxReliableRetries;
        }

        // ====================================================================
        // Server-side hit validation (lag compensation integration)
        // ====================================================================

        /** @brief Result of a server-side hit validation check */
        struct HitValidationResult
        {
            bool hit = false;           ///< True if the raycast hit an entity
            uint32_t entityID = 0;      ///< Network ID of the entity hit (0 if miss)
            XMFLOAT3 hitPoint{0, 0, 0}; ///< World-space hit point
        };

        /**
         * @brief Validate a client's hitscan request using lag compensation.
         *
         * Rewinds world state to the client's perceived time, performs a raycast
         * against rewound hitboxes, then restores the current state.
         *
         * @param clientTimestamp Client-local time when the shot was fired.
         * @param halfRTT        Half the round-trip time to this client.
         * @param rayOrigin      Origin of the hitscan ray (client's weapon muzzle).
         * @param rayDirection   Normalized direction of the hitscan ray.
         * @param maxDistance     Maximum raycast distance.
         * @return HitValidationResult with hit status and entity ID.
         */
        HitValidationResult ValidateHit(float clientTimestamp, float halfRTT, const XMFLOAT3& rayOrigin,
                                        const XMFLOAT3& rayDirection, float maxDistance = 1000.0f);

        /// Get the packet validator for configuration
        /// [game thread] Borrowed mutable validator reference.
        PacketValidator& GetPacketValidator() { return m_packetValidator; }

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

        friend struct NetworkManagerClientIdTestAccess;
        friend struct NetworkManagerEndpointLifecycleTestAccess;
        friend struct NetworkManagerTransportSecurityTestAccess;

        // Outermost lock for public API/lifecycle access. Recursive because
        // Update dispatches user handlers which may call SendMessage or query
        // state on the same thread. Always acquire this before narrow locks.
        mutable std::recursive_mutex m_apiMutex;
        uint64_t m_lifecycleEpoch =
            0; ///< Invalidates callback-interrupted Update continuations after lifecycle change.

        std::vector<NetworkMessage> ProcessIncoming();
        void ProcessOutgoing();
        void FlushOutgoingQueue();
        void HandleRetransmissions();
        /// Requires m_apiMutex and no narrower subsystem lock to be held.
        void DiscardClientLifecycleTraffic(uint64_t lifecycleEpoch);
        std::vector<ClientID> GetConnectedClientIDs() const;
        /// Caller must not hold m_apiMutex: owns its own acquisition so property
        /// serializers run with the lock completely released. Returns false when
        /// a serializer changed the lifecycle epoch (the calling Update stops).
        bool UpdateReplication(float deltaTime, uint64_t lifecycleEpoch);
        void UpdateHeartbeat(float deltaTime);
        ClientID PrepareNextClientID();
        ClientID HandleConnect(const NetworkMessage& msg);
        /// Send a typed ConnectRejected to a pre-registered pending endpoint and forget that endpoint.
        void RejectPendingConnect(ClientID pendingID, ConnectRejectReason reason, const std::string& text);
        /// Client-side: fail a Connecting handshake closed (state, socket, and queued lifecycle traffic).
        void AbandonClientHandshake(ConnectRejectReason reason, std::string text);
        /// Client-side: verify ConnectAccepted's ServerHello against the configured trust, install the
        /// channel and send the sealed ClientFinished. Runs from the ConnectAccepted protocol handler.
        void CompleteClientHandshake(const NetworkMessage& accepted);
        /// Server-side: a Securing client proved its channel with ClientFinished; admit it.
        /// @return false when the ClientFinished payload is malformed (the client stays Securing).
        bool PromoteSecuringClient(ClientID clientID, const NetworkMessage& finished);
        /// Client-side: end a Connecting/Connected session the server closed (Disconnect) or
        /// that went silent past m_connectionTimeout. Closes the socket and discards the
        /// lifecycle's queued traffic and replicated state. @p keepReconnectArmed is true only
        /// for the silence timeout (m_wasConnected stays set so auto-reconnect can run); a
        /// server-sent Disconnect is authoritative and disarms auto-reconnect. Requires m_apiMutex.
        void TerminateClientSession(const std::string& reason, bool keepReconnectArmed);
        void HandleDisconnect(const NetworkMessage& msg);
        /// Server-side: forget one client everywhere it is tracked (client/address tables,
        /// reliability state, delta baselines, interest scope, owned entities). The single
        /// removal path for graceful disconnect, heartbeat timeout and kick. Requires
        /// m_apiMutex; must not be called with m_clientsMutex held.
        void RemoveClientState(ClientID clientID);
        /// Server-side: release the process-global per-connection state (delta baselines,
        /// interest scope) and pending initial syncs of every admitted client before the
        /// client table is cleared.
        void ReleaseAllClientConnectionState();
        /// Requires m_apiMutex. There is no hidden network worker; Update owns the socket pump.
        [[nodiscard]] bool IsEndpointLifecycleIdle() const;

#ifdef ENABLE_NETWORKING
        /// Create, bind, and configure a non-blocking UDP socket
        bool CreateSocket(uint16_t port, const NetworkEndpointPolicy& endpointPolicy);

        /// Whether an exact IPv4 endpoint is inside the captured lifecycle boundary.
        [[nodiscard]] bool IsEndpointAllowed(const sockaddr_in& address) const noexcept;

        /// Close the socket
        void CloseSocket();

        /// Serialize a NetworkMessage into raw bytes for the wire
        std::vector<uint8_t> SerializeMessage(const NetworkMessage& msg) const;

        /// Deserialize raw bytes into a NetworkMessage
        bool DeserializeMessage(const uint8_t* data, size_t length, NetworkMessage& outMsg) const;

        /// Send raw bytes to a specific address (final boundary; never called with an unframed message)
        bool SendRawTo(const std::vector<uint8_t>& data, const sockaddr_in& addr, bool localOnly = false);

        /**
         * @brief Frame one serialized message for @p peerKey and send it (NET-100)
         *
         * Handshake messages go out as [NETWORK_FRAME_HANDSHAKE][message]. Everything else is
         * sealed with the peer's SecureChannel as [NETWORK_FRAME_SEALED][packet], or refused
         * (unsealedSendsRefused) when the peer has none: there is no plaintext fallback. Sealing
         * happens here, at transmit time, so delayed and retransmitted copies each get a fresh
         * sequence number. Also applies the send-key rotation policy.
         */
        bool SendFrameTo(ClientID peerKey, const std::vector<uint8_t>& serialized, const sockaddr_in& addr,
                         bool localOnly);

        /// Server: answer an unframed (pre-v2) Connect with an unframed typed ConnectRejected it can parse.
        void SendLegacyRejection(const sockaddr_in& addr, const NetworkMessage& legacyConnect);

        /// Send one serialized datagram through the InstabilitySimulator: drop,
        /// duplicate, reorder-hold or delay it, or send it now when impairment
        /// is off. `destination` is the peer key (a ClientID on the server,
        /// SERVER_PEER on a client) that FlushOutgoingQueue resolves again when
        /// a delayed copy is released, so delayed unicasts never broadcast.
        /// Disconnect bypasses impairment (it is terminal). Reliable tracking
        /// stays with the caller. `serialized` may be moved from.
        void SendImpaired(std::vector<uint8_t>& serialized, ClientID destination, const sockaddr_in& addr,
                          const NetworkMessage& msg);

        /// Receive raw data from socket (non-blocking)
        int ReceiveRaw(std::vector<uint8_t>& outData, sockaddr_in& outSender);

        SOCKET m_socket = INVALID_SOCKET;
        sockaddr_in m_serverAddress{};

        /// Map of client ID to their socket address (server-side)
        std::unordered_map<ClientID, sockaddr_in> m_clientAddresses;
#endif // ENABLE_NETWORKING

        NetworkSecurityConfig m_securityConfig; ///< Guarded by m_apiMutex; survives lifecycles (SetSecurityConfig).

        /// One established SecureChannel and its rotation bookkeeping (NET-100).
        struct PeerChannel
        {
            std::unique_ptr<SecureChannel> channel;
            uint64_t sealedSinceRotation = 0;
            float rotatedAt = 0.0f; ///< m_serverTime of the last rotation (or establishment)
        };
        /// Channels keyed by peer (ClientID on a server, SERVER_PEER on a client). Guarded by
        /// m_apiMutex; erased with the peer (RemoveClientState, lifecycle ends, epoch exhaustion).
        std::unordered_map<ClientID, PeerChannel> m_secureChannels;
        /// Client-side: the handshake in flight while Connecting (null otherwise).
        std::unique_ptr<ClientHandshake> m_clientHandshake;
        /// Server-side: the ClientHello and ConnectAccepted of each Securing client, so a retransmitted
        /// identical Connect is answered once more (1:1, never amplified) instead of creating state.
        struct PendingAccept
        {
            std::vector<uint8_t> clientHello;
            NetworkMessage accept;
        };
        std::unordered_map<ClientID, PendingAccept> m_pendingAccepts;
        /// Server-side: per-source budget for unadmitted Connects (configured from
        /// m_securityConfig.connectRate at StartServer). Game thread only.
        ConnectRateLimiter m_connectLimiter;
        NetworkEndpointPolicy m_endpointPolicy{}; ///< Captured once and unchanged for the active socket lifecycle.
        bool m_allowLanAdvertisement = false;     ///< Authoritative server option for discovery publishers.

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
        float m_connectionTimeout = 10.0f;   ///< Seconds before a client is considered timed out
        float m_lastServerPacketTime = 0.0f; ///< Client-side: m_serverTime of the last datagram from the server

        NetworkStats m_stats;
        LagCompensator m_lagCompensator;
        PacketValidator m_packetValidator; ///< Validates incoming packets against schemas

        // Clients (server-side)
        std::unordered_map<ClientID, ClientInfo> m_clients;
        mutable std::mutex m_clientsMutex; ///< Protects m_clients, m_nextClientID
        /// Admitted clients still owed their initial full sync (at most one entry per
        /// admitted client; guarded by m_apiMutex, drained kMaxFullSyncsPerUpdate per Update).
        std::deque<ClientID> m_pendingFullSyncs;
        ClientID m_nextClientID = 1;
        int m_maxClients = 32;

        // Messages (Unreliable traffic is bounded by kMaxQueuedMessages).
        std::atomic<uint64_t> m_droppedIncomingMessages{0};
        std::atomic<uint64_t> m_droppedOutgoingMessages{0};
        std::queue<NetworkMessage> m_outgoingQueue;
        std::queue<NetworkMessage> m_incomingQueue;
        size_t m_outgoingQueuePeak = 0; ///< Largest m_outgoingQueue size since Initialize (guarded by m_queueMutex).
        size_t m_incomingQueuePeak = 0; ///< Largest m_incomingQueue size since Initialize (guarded by m_queueMutex).
        // Protocol handlers run first and are never exposed to application code.
        // Application observers may be replaced/cleared without disabling transport invariants.
        std::unordered_map<uint16_t, MessageHandler> m_internalHandlers;
        std::unordered_map<uint16_t, MessageHandler> m_handlers;
        std::unordered_set<uint16_t> m_sensitiveMessageTypes;
        /// Registration owner of each application observer slot; absent means unowned (engine/host code).
        std::unordered_map<uint16_t, std::string> m_handlerOwners;
        mutable std::mutex m_queueMutex;   ///< Protects both queues and their peaks
        mutable std::mutex m_handlerMutex; ///< Protects m_handlers, m_handlerOwners (lowest in lock order)

        // Active ScopedRegistrationOwner state and the timeout handler's owner (guarded by m_apiMutex).
        std::string m_registrationOwner;
        bool m_registrationTeardown = false;
        std::string m_timeoutHandlerOwner;

        /// True when the current registration scope may replace (@p replacing) or remove the slot owned by
        /// @p slotOwner. Callers hold m_apiMutex.
        [[nodiscard]] bool MayWriteOwnedSlot(const std::string& slotOwner, bool replacing) const;
        /// Detach every observer owned by @p ownerId. The caller destroys the returned callbacks after
        /// m_handlerMutex is released (and while the owner's image is still mapped).
        [[nodiscard]] std::vector<MessageHandler> TakeHandlersOwnedBy(const std::string& ownerId);

        // Reliable message tracking — all sequence-keyed reliability state is
        // per peer (see PeerState below). Two clients both numbering their
        // reliable streams from 1 must never collide in the server's
        // dedup/ACK/ordered bookkeeping, and an ACK from one client must never
        // clear another client's unacknowledged messages.
        float m_reliableRetransmitInterval = 0.5f; ///< Base interval; doubles per retry
        int m_maxReliableRetries = 10;             ///< Max retransmissions before marking connection failed

        /// @brief Reliability state for one remote peer.
        ///
        /// On the server the peer key is the ClientID of the remote client; on
        /// a client there is a single implicit peer — the server — keyed by
        /// SERVER_PEER. Both the outgoing stream (sequence counter, unacked
        /// map, retransmit counts) and the incoming stream (dedup window, ACK
        /// bitfield, ordered reorder buffer) live here.
        struct PeerState
        {
            // Outgoing reliable stream (messages we sent to this peer)
            SequenceNumber nextOutgoingSequence = 1; ///< Next reliable sequence to assign
            std::unordered_map<SequenceNumber, NetworkMessage> unacknowledgedMessages; ///< Awaiting ACK
            std::unordered_map<SequenceNumber, float> reliableOriginalSendTime; ///< First-send time (RTT samples)
            std::unordered_map<SequenceNumber, int> retransmitCounts;           ///< Per-message retries (backoff)

            // Incoming reliable stream (messages this peer sent us)
            SequenceNumber remoteSequenceHighest = 0; ///< Highest reliable sequence received
            uint32_t ackBitfield = 0;                 ///< Bitfield for sequences (highest-1) to (highest-32)
            std::unordered_map<SequenceNumber, float> receivedSequences;      ///< seq → receive time (dedup)
            SequenceNumber expectedOrderedSequence = 1;                       ///< Next sequence to deliver in order
            std::unordered_map<SequenceNumber, NetworkMessage> orderedBuffer; ///< Out-of-order holding buffer
        };

        /// @brief Peer key a client uses for its single implicit peer (the server).
        static constexpr ClientID SERVER_PEER = INVALID_CLIENT;

        std::unordered_map<ClientID, PeerState> m_peers; ///< Reliability state keyed by peer

        /// Get (or lazily create) the reliability state for a peer
        PeerState& GetPeerState(ClientID peerKey) { return m_peers[peerKey]; }

        /// Resolve which peer an incoming message belongs to: on the server the
        /// trusted senderID stamped by ProcessIncoming; on a client SERVER_PEER
        ClientID GetIncomingPeerKey(const NetworkMessage& msg) const;

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
        TimeoutHandler m_timeoutHandler; ///< Server-side: called with each timed-out client ID

        /// @brief Checks heartbeat freshness, removes timed-out clients, and
        /// returns their IDs for callback delivery after m_apiMutex is released.
        std::vector<ClientID> CheckConnectionTimeouts();

        // ACK pacing — per-peer highest-sequence/bitfield state lives in PeerState.
        float m_ackSendTimer = 0.0f;                        ///< Accumulate before sending ACKs
        static constexpr float ACK_SEND_INTERVAL = 0.033f;  ///< ~30 Hz ACK rate
        static constexpr float RECEIVED_SEQ_EXPIRY = 30.0f; ///< Prune received-sequence dedup entries after 30s

        /// Process an incoming ACK: remove acknowledged messages from the
        /// sending peer's unacked map (never another peer's)
        void HandleAck(const NetworkMessage& msg);

        /// Send one cumulative ACK packet per peer, covering only that peer's sequences
        void SendAckPacket();

        /// Check for duplicate reliable message from this peer
        bool IsDuplicateSequence(const PeerState& peer, SequenceNumber seq) const;

        /// Record a reliable sequence received from this peer and update its ACK bitfield
        void RecordReceivedSequence(PeerState& peer, SequenceNumber seq);

        /// Detach the next in-sequence buffered message for a peer. Dispatch is
        /// performed by Update after m_apiMutex is released.
        bool PopNextOrderedMessage(ClientID peerKey, NetworkMessage& outMessage);

        /// Prune old entries from every peer's received-sequence dedup map
        void PruneReceivedSequences();

        // Replication
        mutable std::mutex m_replicationMutex; ///< Protects m_replicatedEntities
        std::unordered_map<uint32_t, ReplicatedEntity> m_replicatedEntities;
        std::atomic<uint32_t> m_nextNetworkID{1};
        uint64_t m_replicationMutationEpoch = 0; ///< Prevents clearing dirtiness changed during an unlocked callback.
        float m_replicationInterval = 0.05f;     ///< 20 Hz replication rate
        float m_replicationTimer = 0.0f;

        // Client input
        mutable std::mutex m_inputMutex; ///< Protects m_inputHistory
        SequenceNumber m_inputSequence = 0;

        // Prediction
        std::vector<ClientInputState> m_inputHistory; ///< For client-side prediction

        // Bandwidth tracking
        std::chrono::steady_clock::time_point m_lastBandwidthSample;
        uint64_t m_bytesSentSinceSample = 0;
        uint64_t m_bytesReceivedSinceSample = 0;

        // Auto-reconnect state
        AutoReconnectConfig m_autoReconnect;
        uint32_t m_reconnectAttempts = 0;      ///< Current reconnect attempt count
        float m_reconnectNextRetryTime = 0.0f; ///< Server time when next reconnect allowed
        std::string m_lastServerAddress;       ///< Last server address for reconnect
        uint16_t m_lastServerPort = 0;         ///< Last server port for reconnect
        std::string m_lastPlayerName;          ///< Last player name for reconnect
        std::string m_lastConnectionError;     ///< Last ConnectRejected reason
        ConnectRejectReason m_lastConnectRejectReason = ConnectRejectReason::Unspecified; ///< Typed form of the above
        bool m_wasConnected = false;                     ///< True if we were connected before disconnect
        std::function<void()> m_reconnectFailedCallback; ///< Called when max attempts exhausted

        /**
         * @brief Attempt automatic reconnection (called from Update).
         * @return A failure callback to invoke after m_apiMutex is released.
         */
        std::function<void()> TryAutoReconnect(float deltaTime);
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
        std::string GetLastConnectionError() const { return {}; }
        bool IsInitialized() const { return false; }
    };

} // namespace Spark::Net

#endif // !ENABLE_NETWORKING
