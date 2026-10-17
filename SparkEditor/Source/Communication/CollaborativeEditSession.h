/**
 * @file CollaborativeEditSession.h
 * @brief Multi-user collaborative editor session with presence, locking, and TCP networking
 * @author Spark Engine Team
 * @date 2026
 *
 * Inspired by HeroEngine's live collaborative editing, this system enables
 * multiple editor instances to work on the same scene simultaneously with:
 * - **Presence awareness**: See other editors' cursors and selections
 * - **Node-level locking**: Prevent conflicting edits on the same object
 * - **Edit broadcasting**: See other editors' changes in real-time
 * - **Conflict resolution**: Pessimistic locking with graceful fallback
 * - **TCP networking**: Real peer-to-peer communication over TCP sockets
 * - **Live push**: Optional bridge to push edits to a running AreaServer
 *
 * The implementation uses a pessimistic locking model suitable for
 * small-to-medium teams (2-10 editors). Editors lock nodes before editing,
 * and completed edits are broadcast to all connected sessions.
 *
 * ## Peer-session authentication
 * Host() generates a 256-bit join code (GetJoinCode()) that the hosting user
 * shares out of band. A connecting editor must answer the host's per-connection
 * nonce with HMAC-SHA256(joinCode, nonce || userName) before it is registered as
 * a peer; nothing is queued or relayed for a connection until that proof
 * verifies. Frames, queued bytes and concurrent connections are all bounded
 * (see the kCollab* limits below).
 *
 * ## Usage
 * @code
 *   CollaborativeEditSession session;
 *   session.Host(27030, "Alice");
 *   const std::string code = session.GetJoinCode(); // share with collaborators
 *   // With SPARK_NETWORK_BIND_ADDRESS=192.168.1.20/24:
 *   // session.Connect("192.168.1.100", 27030, "Bob", code);
 *
 *   if (session.RequestLock("Entity_42"))
 *   {
 *       session.BroadcastEdit(editMessage);
 *       session.ReleaseLock("Entity_42");
 *   }
 *
 *   // In editor loop:
 *   session.Update(deltaTime);
 *   auto peers = session.GetConnectedPeers();
 * @endcode
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <vector>

#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "Core/Platform.h"
#endif
#include "Engine/Networking/NetworkBindPolicy.h"

namespace SparkEditor
{
    class StandaloneCollaborationClient;

    // ============================================================================
    // Peer Information
    // ============================================================================

    using PeerID = uint32_t;
    constexpr PeerID INVALID_PEER = 0;

    // ============================================================================
    // Legacy peer-session limits (trust boundary: every peer socket)
    // ============================================================================

    /// Random bytes behind a join code; the code itself is this many bytes as lowercase hex.
    inline constexpr size_t kCollabJoinSecretBytes = 32;
    /// Random bytes in the per-connection challenge nonce.
    inline constexpr size_t kCollabChallengeNonceBytes = 32;
    /// Largest frame accepted from an authenticated peer (edit payloads are small text).
    inline constexpr uint32_t kCollabMaxFrameBytes = 1u << 20;
    /// Largest frame accepted before a connection has authenticated.
    inline constexpr uint32_t kCollabMaxHandshakeFrameBytes = 4096;
    /// Largest identifier-like field (node id, user name, component, property) on the wire.
    inline constexpr size_t kCollabMaxIdentifierBytes = 4096;
    /// Largest display name a peer may announce.
    inline constexpr size_t kCollabMaxUserNameBytes = 128;
    /// Concurrent peer connections (pending + authenticated) a host will service.
    inline constexpr size_t kCollabMaxPeerConnections = 16;
    /// Byte budget for each of the incoming and outgoing message queues.
    inline constexpr size_t kCollabMaxQueuedBytes = size_t{64} * 1024 * 1024;
    /// Seconds a connection has to complete the join handshake.
    inline constexpr int kCollabHandshakeTimeoutSeconds = 5;
    /// Seconds a frame has to arrive in full once its first byte has been received.
    inline constexpr int kCollabFrameCompletionSeconds = 10;

#ifdef _WIN32
    // WinSock SOCKET is UINT_PTR on 64-bit Windows. Never narrow it to int.
    using CollaborativeSocketHandle = std::uintptr_t;
    inline constexpr CollaborativeSocketHandle INVALID_COLLAB_SOCKET = static_cast<CollaborativeSocketHandle>(-1);
    static_assert(sizeof(CollaborativeSocketHandle) >= sizeof(void*));
#else
    using CollaborativeSocketHandle = int;
    inline constexpr CollaborativeSocketHandle INVALID_COLLAB_SOCKET = -1;
#endif

    /**
     * @brief Information about a connected editor peer
     */
    struct EditorPeer
    {
        PeerID id = INVALID_PEER;
        std::string userName;
        std::string selectedNode;                        ///< Currently selected scene node
        DirectX::XMFLOAT3 viewportCameraPos = {0, 0, 0}; ///< Peer's viewport camera position
        DirectX::XMFLOAT3 viewportCameraDir = {0, 0, 1}; ///< Peer's viewport camera direction
        float lastActivityTime = 0.0f;
        bool isActive = true;

        /// @brief Color assigned to this peer for viewport visualization
        struct Color
        {
            float r = 1.0f;
            float g = 1.0f;
            float b = 1.0f;
            float a = 1.0f;
        } color;
    };

    // ============================================================================
    // Edit Messages
    // ============================================================================

    /**
     * @brief Types of edits that can be broadcast
     */
    enum class EditMessageType : uint8_t
    {
        NodeAdded,        ///< A new node was added to the scene
        NodeRemoved,      ///< A node was removed from the scene
        NodeModified,     ///< A node's properties were changed
        NodeMoved,        ///< A node's transform was changed
        NodeRenamed,      ///< A node was renamed
        ComponentAdded,   ///< A component was added to an entity
        ComponentRemoved, ///< A component was removed from an entity
        ComponentModified ///< A component's properties were changed
    };

    /**
     * @brief An edit operation broadcast to all connected editors
     */
    struct EditMessage
    {
        EditMessageType type = EditMessageType::NodeModified;
        PeerID sourceEditor = INVALID_PEER; ///< Who made the edit
        std::string nodeId;                 ///< Affected node/entity ID
        std::string componentType;          ///< Component type (if applicable)
        std::string propertyName;           ///< Property name (if applicable)
        std::string newValue;               ///< New value as string
        std::string oldValue;               ///< Previous value for undo
        uint64_t timestamp = 0;             ///< Edit timestamp (ms since session start)
    };

    // ============================================================================
    // Lock State
    // ============================================================================

    /**
     * @brief Lock information for a scene node
     */
    struct NodeLock
    {
        std::string nodeId;
        PeerID ownerPeer = INVALID_PEER;
        std::string ownerName;
        std::chrono::steady_clock::time_point lockTime;
        float maxDurationSeconds = 300.0f; ///< Auto-expire after 5 minutes
    };

    // ============================================================================
    // Session Callbacks
    // ============================================================================

    using PeerConnectedCallback = std::function<void(const EditorPeer&)>;
    using PeerDisconnectedCallback = std::function<void(PeerID)>;
    using EditReceivedCallback = std::function<void(const EditMessage&)>;
    using LockChangedCallback = std::function<void(const std::string& nodeId, PeerID owner)>;

    // ============================================================================
    // Internal Message Types
    // ============================================================================

    /**
     * @brief Types of internal messages exchanged between peers
     */
    enum class InternalMessageType : uint8_t
    {
        Presence,         ///< Peer presence/heartbeat update
        SelectionChanged, ///< Peer selection changed
        EditBroadcast,    ///< Edit operation broadcast
        LockRequest,      ///< Lock acquisition request (client -> host)
        LockRelease,      ///< Lock release notification
        PeerConnect,      ///< New peer connected
        PeerDisconnect,   ///< Peer disconnected
        LockGranted,      ///< Authoritative grant from host (host -> peers)
        LockDenied,       ///< Authoritative denial from host (host -> requesting peer)
        AuthChallenge,    ///< Host -> connecting peer: payload is the join-proof nonce
        AuthAccepted      ///< Host -> peer: join proof verified; sourcePeer is the assigned PeerID
    };

    /**
     * @brief Internal message structure for the send/receive queues
     */
    struct InternalMessage
    {
        InternalMessageType type = InternalMessageType::Presence;
        PeerID sourcePeer = INVALID_PEER; ///< Originating peer
        std::string nodeId;               ///< Relevant node ID (if applicable)
        std::string payload;              ///< Serialized data payload
        uint64_t timestamp = 0;           ///< Message timestamp (ms since session start)
        EditMessage editMessage;          ///< Embedded edit message (for EditBroadcast type)
        EditorPeer peerInfo;              ///< Embedded peer info (for Presence/PeerConnect)
    };

    // ============================================================================
    // Wire Protocol
    // ============================================================================

    /// @brief Serializes an InternalMessage to a byte buffer for TCP transmission
    std::vector<uint8_t> SerializeMessage(const InternalMessage& msg);

    /// @brief Deserializes bytes back into an InternalMessage. Rejects unknown message or
    ///        edit types and identifier fields longer than kCollabMaxIdentifierBytes.
    bool DeserializeMessage(const uint8_t* data, size_t size, InternalMessage& outMsg);

    /// @brief True when @p joinCode is a well-formed join code (64 lowercase hex characters).
    bool IsValidCollabJoinCode(std::string_view joinCode);

    /**
     * @brief Computes the join proof a peer returns for a host challenge.
     * @return Lowercase hex HMAC-SHA256(joinCode, "SparkCollabJoin/v1" || nonce || userName).
     */
    std::string ComputeCollabJoinProof(std::string_view joinCode, std::string_view nonce, std::string_view userName);

    // ============================================================================
    // Collaborative Edit Session
    // ============================================================================

    /**
     * @brief Manages a multi-user collaborative editing session
     *
     * Handles peer discovery, node locking, edit broadcasting, and presence
     * awareness for collaborative scene editing. Uses TCP sockets for reliable
     * communication between editor instances.
     */
    class CollaborativeEditSession
    {
      public:
        CollaborativeEditSession();
        ~CollaborativeEditSession();

        // Non-copyable, non-movable (owns threads and sockets)
        CollaborativeEditSession(const CollaborativeEditSession&) = delete;
        CollaborativeEditSession& operator=(const CollaborativeEditSession&) = delete;

        // -- Connection --

        /**
         * @brief Host a new collaborative session on a TCP port
         * @param port Port to listen on (0 = OS-assigned; GetPort() reports the bound port)
         * @param userName Display name for this editor
         * @return true if hosting started (listener thread spawned). A fresh join code is
         *         generated; peers must present a proof derived from it (see GetJoinCode()).
         */
        bool Host(uint16_t port, const std::string& userName);
        bool Host(uint16_t port, const std::string& userName, const Spark::Net::NetworkEndpointPolicy& endpointPolicy);

        /**
         * @brief Connect to an existing collaborative session via TCP
         * @param address Host address (IP or hostname)
         * @param port Host port
         * @param userName Display name for this editor
         * @param joinCode The host's join code (GetJoinCode() on the host)
         * @return true only if the TCP connection succeeded and the host accepted the join proof
         */
        bool Connect(const std::string& address, uint16_t port, const std::string& userName,
                     const std::string& joinCode);
        bool Connect(const std::string& address, uint16_t port, const std::string& userName,
                     const std::string& joinCode, const Spark::Net::NetworkEndpointPolicy& endpointPolicy);

        /**
         * @brief Create and join a session hosted by SparkCollabServer.
         * @param endpoint Local named-pipe or Unix-domain socket endpoint.
         * @param sessionId Stable project/session identifier.
         * @param userName Display name for this editor.
         */
        bool HostStandaloneBroker(const std::string& endpoint, const std::string& sessionId,
                                  const std::string& userName);

        /** @brief Join an existing SparkCollabServer session. */
        bool ConnectStandaloneBroker(const std::string& endpoint, const std::string& sessionId,
                                     const std::string& userName);

        /**
         * @brief Disconnect from the session, releasing all locks
         */
        void Disconnect();

        /**
         * @brief Check if connected to a session
         */
        bool IsConnected() const { return m_connected.load(std::memory_order_acquire); }

        /**
         * @brief Check if this instance is the session host
         */
        bool IsHost() const { return m_isHost; }

        /// True when the session authority is the standalone broker rather than a peer editor.
        bool IsStandaloneBroker() const { return m_standaloneClient != nullptr; }

        /**
         * @brief Update the session (call each frame)
         */
        void Update(float deltaTime);

        // -- Peer Awareness --

        std::vector<EditorPeer> GetConnectedPeers() const;
        const EditorPeer* GetPeer(PeerID id) const;
        PeerID GetLocalPeerID() const { return m_localPeerID; }
        void SetLocalSelection(const std::string& nodeId);
        void SetLocalViewportCamera(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& direction);

        // -- Node Locking --

        bool RequestLock(const std::string& nodeId);
        void ReleaseLock(const std::string& nodeId);
        PeerID GetLockOwner(const std::string& nodeId) const;
        bool IsLockedByLocal(const std::string& nodeId) const;
        std::vector<NodeLock> GetAllLocks() const;

        // -- Edit Broadcasting --

        void BroadcastEdit(const EditMessage& message);

        // -- Callbacks --

        void SetPeerConnectedCallback(PeerConnectedCallback callback) { m_onPeerConnected = std::move(callback); }
        void SetPeerDisconnectedCallback(PeerDisconnectedCallback callback)
        {
            m_onPeerDisconnected = std::move(callback);
        }
        void SetEditReceivedCallback(EditReceivedCallback callback) { m_onEditReceived = std::move(callback); }
        void SetLockChangedCallback(LockChangedCallback callback) { m_onLockChanged = std::move(callback); }

        // -- Queries --

        struct SessionStats
        {
            uint32_t peerCount = 0;
            uint32_t activeLocks = 0;
            uint32_t editsBroadcast = 0;
            uint32_t editsReceived = 0;
            float sessionDuration = 0.0f;
        };
        SessionStats GetStats() const;
        std::string Console_GetStatus() const;

        /// @brief Get the port this session is listening on (host) or connected to (client)
        uint16_t GetPort() const { return m_port; }

        /// @brief Join code peers must prove knowledge of (host only; empty otherwise)
        const std::string& GetJoinCode() const { return m_joinCode; }

        /// @brief Get the address this session is connected to (client only)
        const std::string& GetHostAddress() const { return m_hostAddress; }

        /// Session identifier used by the standalone broker connection.
        const std::string& GetStandaloneSessionId() const { return m_standaloneSessionId; }

      private:
        // Application-level message processing
        void ProcessIncomingMessages();
        bool ConnectStandaloneBrokerInternal(const std::string& endpoint, const std::string& sessionId,
                                             const std::string& userName, bool createSession);
        void DisconnectStandaloneBroker();
        void UpdateStandaloneBroker(float deltaTime);
        void PublishStandalonePresence();
        bool RequestStandaloneLock(const std::string& nodeId);
        void ReleaseStandaloneLock(const std::string& nodeId);
        void BroadcastStandaloneEdit(const EditMessage& edit);
        void BroadcastPresence();
        void ExpireStaleNodes();
        PeerID AllocatePeerID();

        /// @brief A message queue bounded by both entry count and approximate bytes.
        struct BoundedMessageQueue
        {
            std::queue<InternalMessage> messages;
            size_t queuedBytes = 0;
            bool overflowWarned = false;
        };

        /// @brief Bounded enqueue; drops the oldest entries past kMaxQueuedMessages or
        ///        kCollabMaxQueuedBytes so a fast peer cannot exhaust memory.
        void EnqueueMessage(BoundedMessageQueue& queue, InternalMessage&& msg, const char* queueName);

        /// @brief Moves every queued message out under the queue lock, resetting its byte count.
        std::queue<InternalMessage> TakeQueuedMessages(BoundedMessageQueue& queue);

        // Network I/O
        void SendToAllPeers(const InternalMessage& msg);
        void SendToPeer(PeerID peerId, const InternalMessage& msg);
        void NetworkThreadHost();
        void NetworkThreadClient();
        void HandleClientSocket(CollaborativeSocketHandle clientSocket, PeerID peerId,
                                std::shared_ptr<std::atomic<bool>> finished);
        /// @brief Runs the host side of the join handshake on an unregistered socket.
        bool AuthenticatePeer(CollaborativeSocketHandle clientSocket, PeerID peerId, InternalMessage& outConnect);
        /// @brief Runs the client side of the join handshake; returns the host-assigned PeerID.
        PeerID AuthenticateToHost(const std::string& userName, const std::string& joinCode);
        void ReapFinishedClientThreads(); ///< Join+remove client handler threads that have exited
        void ShutdownAllSockets();        ///< shutdown() all sockets to unblock recv()/accept()
        void CloseAllSockets();

        // State
        std::atomic<bool> m_connected{false};
        std::atomic<bool> m_shuttingDown{false};
        bool m_isHost = false;
        PeerID m_localPeerID = INVALID_PEER;
        std::string m_localUserName;
        std::string m_joinCode; ///< Host only: secret peers must prove knowledge of
        float m_sessionTime = 0.0f;
        uint16_t m_port = 0;
        std::string m_hostAddress;
        std::string m_standaloneSessionId;
        std::unique_ptr<StandaloneCollaborationClient> m_standaloneClient;
        float m_standaloneSnapshotTimer = 0.0f;
        uint64_t m_standaloneLastEditSequence = 0;
        bool m_standaloneSnapshotInitialized = false;
        std::unordered_set<uint64_t> m_standaloneLocalEditSequences;

        // Peers
        std::unordered_map<PeerID, EditorPeer> m_peers;
        mutable std::mutex m_peerMutex;
        PeerID m_nextPeerID = 1;

        // Locks
        std::unordered_map<std::string, NodeLock> m_nodeLocks;
        mutable std::mutex m_lockMutex;

        // Stats
        uint32_t m_editsBroadcast = 0;
        uint32_t m_editsReceived = 0;

        // Callbacks
        PeerConnectedCallback m_onPeerConnected;
        PeerDisconnectedCallback m_onPeerDisconnected;
        EditReceivedCallback m_onEditReceived;
        LockChangedCallback m_onLockChanged;

        // Presence broadcasting
        float m_presenceBroadcastInterval = 1.0f;
        float m_presenceBroadcastTimer = 0.0f;

        // Message queues (bounded — see kMaxQueuedMessages / kCollabMaxQueuedBytes / EnqueueMessage)
        static constexpr size_t kMaxQueuedMessages = 8192;
        BoundedMessageQueue m_outgoingMessages;
        BoundedMessageQueue m_incomingMessages;
        mutable std::mutex m_messageMutex;

        // Networking — TCP sockets and threads
        CollaborativeSocketHandle m_listenSocket = INVALID_COLLAB_SOCKET;
        Spark::Net::NetworkEndpointPolicy m_endpointPolicy{};
        CollaborativeSocketHandle m_clientSocket = INVALID_COLLAB_SOCKET; ///< Client's connection to host
        std::unordered_map<PeerID, CollaborativeSocketHandle> m_peerSockets;
        mutable std::mutex m_socketMutex;
        std::thread m_networkThread;

        /// @brief A per-client handler thread plus a flag it sets when it exits,
        ///        so finished threads can be joined and reaped from the main thread.
        struct ClientConnection
        {
            std::thread thread;
            std::shared_ptr<std::atomic<bool>> finished;
        };
        std::vector<ClientConnection> m_clientThreads;
        mutable std::mutex m_clientThreadsMutex;

        // Peer color palette for viewport visualization
        static constexpr EditorPeer::Color kPeerColors[] = {
            {0.2f, 0.8f, 0.2f, 1.0f}, // Green (host)
            {0.2f, 0.5f, 0.9f, 1.0f}, // Blue
            {0.9f, 0.4f, 0.2f, 1.0f}, // Orange
            {0.8f, 0.2f, 0.8f, 1.0f}, // Purple
            {0.9f, 0.9f, 0.2f, 1.0f}, // Yellow
            {0.2f, 0.9f, 0.9f, 1.0f}, // Cyan
            {0.9f, 0.2f, 0.4f, 1.0f}, // Pink
            {0.5f, 0.8f, 0.3f, 1.0f}, // Lime
        };
    };

} // namespace SparkEditor
