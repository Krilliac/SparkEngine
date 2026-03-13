/**
 * @file CollaborativeEditSession.h
 * @brief Multi-user collaborative editor session with presence and locking
 * @author Spark Engine Team
 * @date 2026
 *
 * Inspired by HeroEngine's live collaborative editing, this system enables
 * multiple editor instances to work on the same scene simultaneously with:
 * - **Presence awareness**: See other editors' cursors and selections
 * - **Node-level locking**: Prevent conflicting edits on the same object
 * - **Edit broadcasting**: See other editors' changes in real-time
 * - **Conflict resolution**: Pessimistic locking with graceful fallback
 *
 * Unlike HeroEngine's full real-time character-by-character sync, this
 * implementation uses a simpler pessimistic locking model suitable for
 * small-to-medium teams (2-10 editors). Editors lock nodes before editing,
 * and completed edits are broadcast to all connected sessions.
 *
 * ## Usage
 * @code
 *   CollaborativeEditSession session;
 *   session.Connect("192.168.1.100", 27030);
 *
 *   // Lock a node before editing
 *   if (session.RequestLock("Entity_42"))
 *   {
 *       // Make edits...
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
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <vector>

#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "Core/Platform.h"
#endif

namespace SparkEditor
{

    // ============================================================================
    // Peer Information
    // ============================================================================

    using PeerID = uint32_t;
    constexpr PeerID INVALID_PEER = 0;

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
        EditMessageType type;
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
        LockRequest,      ///< Lock acquisition request
        LockRelease,      ///< Lock release notification
        PeerConnect,      ///< New peer connected
        PeerDisconnect    ///< Peer disconnected
    };

    /**
     * @brief Internal message structure for the send/receive queues
     */
    struct InternalMessage
    {
        InternalMessageType type;
        PeerID sourcePeer = INVALID_PEER; ///< Originating peer
        std::string nodeId;               ///< Relevant node ID (if applicable)
        std::string payload;              ///< Serialized data payload
        uint64_t timestamp = 0;           ///< Message timestamp (ms since session start)
        EditMessage editMessage;          ///< Embedded edit message (for EditBroadcast type)
        EditorPeer peerInfo;              ///< Embedded peer info (for Presence/PeerConnect)
    };

    // ============================================================================
    // Collaborative Edit Session
    // ============================================================================

    /**
     * @brief Manages a multi-user collaborative editing session
     *
     * Handles peer discovery, node locking, edit broadcasting, and presence
     * awareness for collaborative scene editing.
     */
    class CollaborativeEditSession
    {
      public:
        CollaborativeEditSession();
        ~CollaborativeEditSession();

        // -- Connection --

        /**
         * @brief Host a new collaborative session
         * @param port Port to listen on
         * @param userName Display name for this editor
         * @return true if hosting started
         */
        bool Host(uint16_t port, const std::string& userName);

        /**
         * @brief Connect to an existing collaborative session
         * @param address Host address
         * @param port Host port
         * @param userName Display name for this editor
         * @return true if connection succeeded
         */
        bool Connect(const std::string& address, uint16_t port, const std::string& userName);

        /**
         * @brief Disconnect from the session
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

        /**
         * @brief Update the session (call each frame)
         */
        void Update(float deltaTime);

        // -- Peer Awareness --

        /**
         * @brief Get all connected peers
         */
        std::vector<EditorPeer> GetConnectedPeers() const;

        /**
         * @brief Get a specific peer's info
         */
        const EditorPeer* GetPeer(PeerID id) const;

        /**
         * @brief Get the local peer's ID
         */
        PeerID GetLocalPeerID() const { return m_localPeerID; }

        /**
         * @brief Update the local editor's selection (broadcast to peers)
         * @param nodeId The currently selected node ID
         */
        void SetLocalSelection(const std::string& nodeId);

        /**
         * @brief Update the local editor's viewport camera (broadcast to peers)
         */
        void SetLocalViewportCamera(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& direction);

        // -- Node Locking --

        /**
         * @brief Request a lock on a scene node for editing
         * @param nodeId The node to lock
         * @return true if the lock was acquired (not held by another peer)
         */
        bool RequestLock(const std::string& nodeId);

        /**
         * @brief Release a lock on a scene node
         * @param nodeId The node to unlock
         */
        void ReleaseLock(const std::string& nodeId);

        /**
         * @brief Check if a node is locked
         * @param nodeId The node to check
         * @return PeerID of the lock owner, or INVALID_PEER if unlocked
         */
        PeerID GetLockOwner(const std::string& nodeId) const;

        /**
         * @brief Check if the local editor holds the lock on a node
         */
        bool IsLockedByLocal(const std::string& nodeId) const;

        /**
         * @brief Get all current node locks
         */
        std::vector<NodeLock> GetAllLocks() const;

        // -- Edit Broadcasting --

        /**
         * @brief Broadcast an edit to all connected editors
         * @param message The edit operation to broadcast
         */
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

        /**
         * @brief Get session statistics
         */
        struct SessionStats
        {
            uint32_t peerCount = 0;
            uint32_t activeLocks = 0;
            uint32_t editsBroadcast = 0;
            uint32_t editsReceived = 0;
            float sessionDuration = 0.0f;
        };
        SessionStats GetStats() const;

        /**
         * @brief Console status string
         */
        std::string Console_GetStatus() const;

      private:
        void ProcessIncomingMessages();
        void BroadcastPresence();
        void ExpireStaleNodes();
        PeerID AllocatePeerID();

        // State
        std::atomic<bool> m_connected{false};
        bool m_isHost = false;
        PeerID m_localPeerID = INVALID_PEER;
        std::string m_localUserName;
        float m_sessionTime = 0.0f;

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

        // Message queues
        std::queue<InternalMessage> m_outgoingMessages;
        std::queue<InternalMessage> m_incomingMessages;
        mutable std::mutex m_messageMutex;
    };

} // namespace SparkEditor
