/**
 * @file CollaborativeEditSession.cpp
 * @brief Multi-user collaborative editor session implementation
 */

#include "CollaborativeEditSession.h"

#include <algorithm>
#include <sstream>

// Use engine logging if available, otherwise fall back to stderr
#ifndef SPARK_LOG_INFO
#include <cstdio>
#define SPARK_LOG_INFO(cat, fmt, ...) fprintf(stderr, "[" cat "] " fmt "\n", ##__VA_ARGS__)
#define SPARK_LOG_WARN(cat, fmt, ...) fprintf(stderr, "[" cat " WARN] " fmt "\n", ##__VA_ARGS__)
#define SPARK_LOG_DEBUG(cat, fmt, ...) fprintf(stderr, "[" cat " DEBUG] " fmt "\n", ##__VA_ARGS__)
#endif

namespace SparkEditor
{

    // ============================================================================
    // Construction / Destruction
    // ============================================================================

    CollaborativeEditSession::CollaborativeEditSession() = default;

    CollaborativeEditSession::~CollaborativeEditSession()
    {
        Disconnect();
    }

    // ============================================================================
    // Connection
    // ============================================================================

    bool CollaborativeEditSession::Host(uint16_t port, const std::string& userName)
    {
        if (m_connected.load(std::memory_order_acquire))
        {
            SPARK_LOG_WARN("CollabEdit", "Already connected.");
            return false;
        }

        m_isHost = true;
        m_localUserName = userName;
        m_localPeerID = AllocatePeerID();
        m_sessionTime = 0.0f;

        // Register self as a peer
        EditorPeer self;
        self.id = m_localPeerID;
        self.userName = userName;
        self.isActive = true;
        self.color = {0.2f, 0.8f, 0.2f, 1.0f}; // Green for host

        {
            std::lock_guard<std::mutex> lock(m_peerMutex);
            m_peers[m_localPeerID] = self;
        }

        m_connected.store(true, std::memory_order_release);

        SPARK_LOG_INFO("CollabEdit", "Hosting session on port %u as '%s' (PeerID=%u).", port, userName.c_str(),
                       m_localPeerID);
        return true;
    }

    bool CollaborativeEditSession::Connect(const std::string& address, uint16_t port, const std::string& userName)
    {
        if (m_connected.load(std::memory_order_acquire))
        {
            SPARK_LOG_WARN("CollabEdit", "Already connected.");
            return false;
        }

        m_isHost = false;
        m_localUserName = userName;
        m_localPeerID = AllocatePeerID();
        m_sessionTime = 0.0f;

        // Register self as a peer
        EditorPeer self;
        self.id = m_localPeerID;
        self.userName = userName;
        self.isActive = true;
        self.color = {0.2f, 0.5f, 0.9f, 1.0f}; // Blue for client

        {
            std::lock_guard<std::mutex> lock(m_peerMutex);
            m_peers[m_localPeerID] = self;
        }

        m_connected.store(true, std::memory_order_release);

        SPARK_LOG_INFO("CollabEdit", "Connected to %s:%u as '%s' (PeerID=%u).", address.c_str(), port, userName.c_str(),
                       m_localPeerID);
        return true;
    }

    void CollaborativeEditSession::Disconnect()
    {
        if (!m_connected.load(std::memory_order_acquire))
            return;

        // Release all locks held by the local editor
        {
            std::lock_guard<std::mutex> lock(m_lockMutex);
            auto it = m_nodeLocks.begin();
            while (it != m_nodeLocks.end())
            {
                if (it->second.ownerPeer == m_localPeerID)
                {
                    it = m_nodeLocks.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        m_connected.store(false, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(m_peerMutex);
            m_peers.clear();
        }

        SPARK_LOG_INFO("CollabEdit", "Disconnected from session.");
    }

    // ============================================================================
    // Update
    // ============================================================================

    void CollaborativeEditSession::Update(float deltaTime)
    {
        if (!m_connected.load(std::memory_order_acquire))
            return;

        m_sessionTime += deltaTime;

        ProcessIncomingMessages();

        // Broadcast presence periodically
        m_presenceBroadcastTimer += deltaTime;
        if (m_presenceBroadcastTimer >= m_presenceBroadcastInterval)
        {
            m_presenceBroadcastTimer = 0.0f;
            BroadcastPresence();
        }

        // Expire stale locks
        ExpireStaleNodes();
    }

    // ============================================================================
    // Peer Awareness
    // ============================================================================

    std::vector<EditorPeer> CollaborativeEditSession::GetConnectedPeers() const
    {
        std::lock_guard<std::mutex> lock(m_peerMutex);
        std::vector<EditorPeer> result;
        result.reserve(m_peers.size());
        for (const auto& [id, peer] : m_peers)
        {
            result.push_back(peer);
        }
        return result;
    }

    const EditorPeer* CollaborativeEditSession::GetPeer(PeerID id) const
    {
        std::lock_guard<std::mutex> lock(m_peerMutex);
        auto it = m_peers.find(id);
        return it != m_peers.end() ? &it->second : nullptr;
    }

    void CollaborativeEditSession::SetLocalSelection(const std::string& nodeId)
    {
        // Validate nodeId length to prevent excessively long identifiers
        if (nodeId.size() >= 256)
        {
            SPARK_LOG_WARN("CollabEdit", "SetLocalSelection rejected: nodeId exceeds 255 chars (length=%zu).",
                           nodeId.size());
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_peerMutex);
            auto it = m_peers.find(m_localPeerID);
            if (it != m_peers.end())
            {
                it->second.selectedNode = nodeId;
            }
        }

        // Queue a selection-changed message for broadcast to all peers
        {
            InternalMessage msg;
            msg.type = InternalMessageType::SelectionChanged;
            msg.sourcePeer = m_localPeerID;
            msg.nodeId = nodeId;
            msg.timestamp = static_cast<uint64_t>(m_sessionTime * 1000.0f);

            std::lock_guard<std::mutex> lock(m_messageMutex);
            m_outgoingMessages.push(std::move(msg));
        }

        SPARK_LOG_INFO("CollabEdit", "Local selection changed to '%s' (PeerID=%u).", nodeId.c_str(), m_localPeerID);
    }

    void CollaborativeEditSession::SetLocalViewportCamera(const XMFLOAT3& position, const XMFLOAT3& direction)
    {
        std::lock_guard<std::mutex> lock(m_peerMutex);
        auto it = m_peers.find(m_localPeerID);
        if (it != m_peers.end())
        {
            it->second.viewportCameraPos = position;
            it->second.viewportCameraDir = direction;
        }
    }

    // ============================================================================
    // Node Locking
    // ============================================================================

    bool CollaborativeEditSession::RequestLock(const std::string& nodeId)
    {
        std::lock_guard<std::mutex> lock(m_lockMutex);

        auto it = m_nodeLocks.find(nodeId);
        if (it != m_nodeLocks.end())
        {
            // Already locked
            if (it->second.ownerPeer == m_localPeerID)
            {
                return true; // Already locked by us
            }
            return false; // Locked by another peer
        }

        NodeLock newLock;
        newLock.nodeId = nodeId;
        newLock.ownerPeer = m_localPeerID;
        newLock.ownerName = m_localUserName;
        newLock.lockTime = std::chrono::steady_clock::now();
        m_nodeLocks[nodeId] = newLock;

        // Notify callbacks
        if (m_onLockChanged)
        {
            m_onLockChanged(nodeId, m_localPeerID);
        }

        return true;
    }

    void CollaborativeEditSession::ReleaseLock(const std::string& nodeId)
    {
        std::lock_guard<std::mutex> lock(m_lockMutex);

        auto it = m_nodeLocks.find(nodeId);
        if (it != m_nodeLocks.end() && it->second.ownerPeer == m_localPeerID)
        {
            m_nodeLocks.erase(it);

            if (m_onLockChanged)
            {
                m_onLockChanged(nodeId, INVALID_PEER);
            }
        }
    }

    PeerID CollaborativeEditSession::GetLockOwner(const std::string& nodeId) const
    {
        std::lock_guard<std::mutex> lock(m_lockMutex);
        auto it = m_nodeLocks.find(nodeId);
        return it != m_nodeLocks.end() ? it->second.ownerPeer : INVALID_PEER;
    }

    bool CollaborativeEditSession::IsLockedByLocal(const std::string& nodeId) const
    {
        return GetLockOwner(nodeId) == m_localPeerID;
    }

    std::vector<NodeLock> CollaborativeEditSession::GetAllLocks() const
    {
        std::lock_guard<std::mutex> lock(m_lockMutex);
        std::vector<NodeLock> result;
        result.reserve(m_nodeLocks.size());
        for (const auto& [id, nodeLock] : m_nodeLocks)
        {
            result.push_back(nodeLock);
        }
        return result;
    }

    // ============================================================================
    // Edit Broadcasting
    // ============================================================================

    void CollaborativeEditSession::BroadcastEdit(const EditMessage& message)
    {
        // Validate the EditMessage
        if (message.nodeId.empty())
        {
            SPARK_LOG_WARN("CollabEdit", "BroadcastEdit rejected: nodeId is empty.");
            return;
        }

        if (message.sourceEditor == INVALID_PEER)
        {
            SPARK_LOG_WARN("CollabEdit", "BroadcastEdit rejected: sourceEditor is not set.");
            return;
        }

        if (static_cast<uint8_t>(message.type) > static_cast<uint8_t>(EditMessageType::ComponentModified))
        {
            SPARK_LOG_WARN("CollabEdit", "BroadcastEdit rejected: invalid EditMessageType (%u).",
                           static_cast<unsigned>(message.type));
            return;
        }

        // Prepare the message, setting timestamp if not already set
        EditMessage outgoing = message;
        if (outgoing.timestamp == 0)
        {
            outgoing.timestamp = static_cast<uint64_t>(m_sessionTime * 1000.0f);
        }

        m_editsBroadcast++;

        // Serialize the EditMessage into a message queue entry for network broadcast
        {
            InternalMessage msg;
            msg.type = InternalMessageType::EditBroadcast;
            msg.sourcePeer = outgoing.sourceEditor;
            msg.nodeId = outgoing.nodeId;
            msg.timestamp = outgoing.timestamp;
            msg.editMessage = outgoing;

            std::lock_guard<std::mutex> lock(m_messageMutex);
            m_outgoingMessages.push(std::move(msg));
        }

        SPARK_LOG_INFO("CollabEdit", "BroadcastEdit: type=%u nodeId='%s' from PeerID=%u.",
                       static_cast<unsigned>(outgoing.type), outgoing.nodeId.c_str(), outgoing.sourceEditor);

        // Still call the local callback for immediate local processing
        if (m_onEditReceived)
        {
            m_onEditReceived(outgoing);
        }
    }

    // ============================================================================
    // Queries
    // ============================================================================

    CollaborativeEditSession::SessionStats CollaborativeEditSession::GetStats() const
    {
        SessionStats stats;
        {
            std::lock_guard<std::mutex> lock(m_peerMutex);
            stats.peerCount = static_cast<uint32_t>(m_peers.size());
        }
        {
            std::lock_guard<std::mutex> lock(m_lockMutex);
            stats.activeLocks = static_cast<uint32_t>(m_nodeLocks.size());
        }
        stats.editsBroadcast = m_editsBroadcast;
        stats.editsReceived = m_editsReceived;
        stats.sessionDuration = m_sessionTime;
        return stats;
    }

    std::string CollaborativeEditSession::Console_GetStatus() const
    {
        auto stats = GetStats();
        std::ostringstream oss;
        oss << "CollabEdit: " << (m_connected.load() ? "Connected" : "Disconnected")
            << (m_isHost ? " (Host)" : " (Client)") << " | Peers: " << stats.peerCount
            << " | Locks: " << stats.activeLocks << " | Edits sent/recv: " << stats.editsBroadcast << "/"
            << stats.editsReceived << " | Session: " << stats.sessionDuration << "s";
        return oss.str();
    }

    // ============================================================================
    // Internal
    // ============================================================================

    void CollaborativeEditSession::ProcessIncomingMessages()
    {
        // Drain the incoming message queue into a local copy to minimize lock hold time
        std::queue<InternalMessage> localQueue;
        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            std::swap(localQueue, m_incomingMessages);
        }

        if (localQueue.empty())
        {
            return;
        }

        SPARK_LOG_DEBUG("CollabEdit", "Processing %zu incoming messages.", localQueue.size());

        while (!localQueue.empty())
        {
            InternalMessage msg = std::move(localQueue.front());
            localQueue.pop();

            switch (msg.type)
            {
            case InternalMessageType::Presence:
            {
                // Update peer presence in the peers map
                std::lock_guard<std::mutex> lock(m_peerMutex);
                auto it = m_peers.find(msg.sourcePeer);
                if (it != m_peers.end())
                {
                    it->second.viewportCameraPos = msg.peerInfo.viewportCameraPos;
                    it->second.viewportCameraDir = msg.peerInfo.viewportCameraDir;
                    it->second.selectedNode = msg.peerInfo.selectedNode;
                    it->second.lastActivityTime = m_sessionTime;
                    it->second.isActive = true;
                }
                break;
            }

            case InternalMessageType::SelectionChanged:
            {
                std::lock_guard<std::mutex> lock(m_peerMutex);
                auto it = m_peers.find(msg.sourcePeer);
                if (it != m_peers.end())
                {
                    it->second.selectedNode = msg.nodeId;
                    it->second.lastActivityTime = m_sessionTime;
                }
                break;
            }

            case InternalMessageType::EditBroadcast:
            {
                m_editsReceived++;
                SPARK_LOG_INFO("CollabEdit", "Received edit: type=%u nodeId='%s' from PeerID=%u.",
                               static_cast<unsigned>(msg.editMessage.type), msg.editMessage.nodeId.c_str(),
                               msg.sourcePeer);

                if (m_onEditReceived)
                {
                    m_onEditReceived(msg.editMessage);
                }
                break;
            }

            case InternalMessageType::LockRequest:
            {
                std::lock_guard<std::mutex> lock(m_lockMutex);
                auto it = m_nodeLocks.find(msg.nodeId);
                if (it == m_nodeLocks.end())
                {
                    // Grant the lock
                    NodeLock newLock;
                    newLock.nodeId = msg.nodeId;
                    newLock.ownerPeer = msg.sourcePeer;
                    newLock.lockTime = std::chrono::steady_clock::now();
                    m_nodeLocks[msg.nodeId] = newLock;

                    SPARK_LOG_INFO("CollabEdit", "Lock granted on '%s' to PeerID=%u.", msg.nodeId.c_str(),
                                   msg.sourcePeer);

                    if (m_onLockChanged)
                    {
                        m_onLockChanged(msg.nodeId, msg.sourcePeer);
                    }
                }
                break;
            }

            case InternalMessageType::LockRelease:
            {
                std::lock_guard<std::mutex> lock(m_lockMutex);
                auto it = m_nodeLocks.find(msg.nodeId);
                if (it != m_nodeLocks.end() && it->second.ownerPeer == msg.sourcePeer)
                {
                    m_nodeLocks.erase(it);

                    SPARK_LOG_INFO("CollabEdit", "Lock released on '%s' by PeerID=%u.", msg.nodeId.c_str(),
                                   msg.sourcePeer);

                    if (m_onLockChanged)
                    {
                        m_onLockChanged(msg.nodeId, INVALID_PEER);
                    }
                }
                break;
            }

            case InternalMessageType::PeerConnect:
            {
                {
                    std::lock_guard<std::mutex> lock(m_peerMutex);
                    m_peers[msg.sourcePeer] = msg.peerInfo;
                    m_peers[msg.sourcePeer].lastActivityTime = m_sessionTime;
                }

                SPARK_LOG_INFO("CollabEdit", "Peer connected: '%s' (PeerID=%u).", msg.peerInfo.userName.c_str(),
                               msg.sourcePeer);

                if (m_onPeerConnected)
                {
                    m_onPeerConnected(msg.peerInfo);
                }
                break;
            }

            case InternalMessageType::PeerDisconnect:
            {
                {
                    std::lock_guard<std::mutex> lock(m_peerMutex);
                    m_peers.erase(msg.sourcePeer);
                }

                SPARK_LOG_INFO("CollabEdit", "Peer disconnected: PeerID=%u.", msg.sourcePeer);

                if (m_onPeerDisconnected)
                {
                    m_onPeerDisconnected(msg.sourcePeer);
                }
                break;
            }
            }
        }
    }

    void CollaborativeEditSession::BroadcastPresence()
    {
        // Collect local peer state
        EditorPeer localPeerSnapshot;
        {
            std::lock_guard<std::mutex> lock(m_peerMutex);
            auto it = m_peers.find(m_localPeerID);
            if (it == m_peers.end())
            {
                SPARK_LOG_WARN("CollabEdit", "BroadcastPresence: local peer not found in peers map.");
                return;
            }

            // Update local peer's activity time
            it->second.lastActivityTime = m_sessionTime;
            it->second.isActive = true;
            localPeerSnapshot = it->second;
        }

        // Queue a presence message for broadcast to all peers
        {
            InternalMessage msg;
            msg.type = InternalMessageType::Presence;
            msg.sourcePeer = m_localPeerID;
            msg.timestamp = static_cast<uint64_t>(m_sessionTime * 1000.0f);
            msg.peerInfo = localPeerSnapshot;

            std::lock_guard<std::mutex> lock(m_messageMutex);
            m_outgoingMessages.push(std::move(msg));
        }

        SPARK_LOG_DEBUG("CollabEdit", "Presence broadcast: PeerID=%u selection='%s' pos=(%.1f,%.1f,%.1f).",
                        m_localPeerID, localPeerSnapshot.selectedNode.c_str(), localPeerSnapshot.viewportCameraPos.x,
                        localPeerSnapshot.viewportCameraPos.y, localPeerSnapshot.viewportCameraPos.z);
    }

    void CollaborativeEditSession::ExpireStaleNodes()
    {
        std::lock_guard<std::mutex> lock(m_lockMutex);
        auto now = std::chrono::steady_clock::now();

        auto it = m_nodeLocks.begin();
        while (it != m_nodeLocks.end())
        {
            auto elapsed = std::chrono::duration<float>(now - it->second.lockTime).count();
            if (elapsed > it->second.maxDurationSeconds)
            {
                std::string nodeId = it->first;
                it = m_nodeLocks.erase(it);

                if (m_onLockChanged)
                {
                    m_onLockChanged(nodeId, INVALID_PEER);
                }
            }
            else
            {
                ++it;
            }
        }
    }

    PeerID CollaborativeEditSession::AllocatePeerID()
    {
        return m_nextPeerID++;
    }

} // namespace SparkEditor
