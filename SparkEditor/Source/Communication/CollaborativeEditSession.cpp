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
        std::lock_guard<std::mutex> lock(m_peerMutex);
        auto it = m_peers.find(m_localPeerID);
        if (it != m_peers.end())
        {
            it->second.selectedNode = nodeId;
        }
        // In a full implementation, broadcast selection change to peers
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
        m_editsBroadcast++;

        // In a full implementation, serialize and send the edit message to
        // all connected peers via the session's network channel.

        // For now, just notify the local callback (useful for testing)
        if (m_onEditReceived)
        {
            m_onEditReceived(message);
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
        // In a full implementation, receive and process messages from the
        // session's network channel:
        // - Peer presence updates -> update m_peers
        // - Lock requests/releases -> update m_nodeLocks
        // - Edit messages -> invoke m_onEditReceived callback
        // - Peer connect/disconnect -> invoke callbacks
    }

    void CollaborativeEditSession::BroadcastPresence()
    {
        // In a full implementation, send local peer state (selection,
        // camera position) to all connected peers.
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
