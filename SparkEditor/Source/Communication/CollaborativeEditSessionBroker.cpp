/**
 * @file CollaborativeEditSessionBroker.cpp
 * @brief SparkCollabServer-backed CollaborativeEditSession lifecycle and snapshot translation.
 */

#include "CollaborativeEditSession.h"
#include "StandaloneCollaborationClient.h"
#include "Utils/Validate.h"

#include <algorithm>

namespace SparkEditor
{
    bool CollaborativeEditSession::HostStandaloneBroker(const std::string& endpoint, const std::string& sessionId,
                                                        const std::string& userName)
    {
        return ConnectStandaloneBrokerInternal(endpoint, sessionId, userName, true);
    }

    bool CollaborativeEditSession::ConnectStandaloneBroker(const std::string& endpoint, const std::string& sessionId,
                                                           const std::string& userName)
    {
        return ConnectStandaloneBrokerInternal(endpoint, sessionId, userName, false);
    }

    bool CollaborativeEditSession::ConnectStandaloneBrokerInternal(const std::string& endpoint,
                                                                   const std::string& sessionId,
                                                                   const std::string& userName, bool createSession)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (endpoint.empty() || sessionId.empty() || userName.empty())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor,
                           "Cannot connect to standalone collaboration: endpoint, session, and user are required.");
            return false;
        }
        if (m_connected.load(std::memory_order_acquire))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Already connected.");
            return false;
        }

        auto client = std::make_unique<StandaloneCollaborationClient>();
        auto connected = client->Connect(endpoint);
        if (!connected)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Could not connect to SparkCollabServer at '%s': %s",
                            endpoint.c_str(), connected.error().c_str());
            return false;
        }

        std::string administrationToken;
        if (createSession)
        {
            auto created = client->CreateSession(sessionId);
            if (!created)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Could not create collaboration session '%s': %s",
                                sessionId.c_str(), created.error().c_str());
                return false;
            }
            administrationToken = std::move(*created);
        }

        auto joined = client->JoinSession(sessionId, userName);
        if (!joined)
        {
            if (!administrationToken.empty())
                (void)client->DeleteSession(sessionId, administrationToken);
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Could not join collaboration session '%s': %s",
                            sessionId.c_str(), joined.error().c_str());
            return false;
        }

        auto baseline = client->GetSnapshot();
        if (!baseline)
        {
            (void)client->LeaveSession();
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Could not read collaboration session '%s': %s",
                            sessionId.c_str(), baseline.error().c_str());
            return false;
        }

        m_standaloneClient = std::move(client);
        m_isHost = createSession;
        m_localUserName = userName;
        m_localPeerID = *joined;
        m_sessionTime = 0.0f;
        m_port = 0;
        m_hostAddress = endpoint;
        m_standaloneSessionId = sessionId;
        m_standaloneLastEditSequence = baseline->nextSequence > 0 ? baseline->nextSequence - 1 : 0;
        m_standaloneSnapshotInitialized = true;
        m_standaloneSnapshotTimer = 0.1f;
        m_presenceBroadcastTimer = m_presenceBroadcastInterval;
        m_shuttingDown.store(false, std::memory_order_release);
        m_connected.store(true, std::memory_order_release);
        UpdateStandaloneBroker(0.0f);

        SPARK_LOG_INFO(
            Spark::LogCategory::Editor, "%s standalone collaboration session '%s' at '%s' as '%s' (PeerID=%u).",
            createSession ? "Created" : "Joined", sessionId.c_str(), endpoint.c_str(), userName.c_str(), m_localPeerID);
        return true;
    }

    void CollaborativeEditSession::DisconnectStandaloneBroker()
    {
        auto left = m_standaloneClient->LeaveSession();
        if (!left)
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Could not leave standalone collaboration cleanly: %s",
                           left.error().c_str());
        m_standaloneClient->Disconnect();
        m_standaloneClient.reset();
        {
            std::lock_guard<std::mutex> peerLock(m_peerMutex);
            m_peers.clear();
        }
        {
            std::lock_guard<std::mutex> lockLock(m_lockMutex);
            m_nodeLocks.clear();
        }
        m_standaloneSessionId.clear();
        m_standaloneLastEditSequence = 0;
        m_standaloneSnapshotInitialized = false;
        m_standaloneLocalEditSequences.clear();
        m_isHost = false;
        m_localPeerID = INVALID_PEER;
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Disconnected from standalone collaboration session.");
    }

    void CollaborativeEditSession::UpdateStandaloneBroker(float deltaTime)
    {
        m_presenceBroadcastTimer += deltaTime;
        if (m_presenceBroadcastTimer >= m_presenceBroadcastInterval)
        {
            m_presenceBroadcastTimer = 0.0f;
            PublishStandalonePresence();
            if (!m_standaloneClient)
                return;
        }

        m_standaloneSnapshotTimer += deltaTime;
        if (m_standaloneSnapshotTimer < 0.1f)
            return;
        m_standaloneSnapshotTimer = 0.0f;

        auto received = m_standaloneClient->GetSnapshot();
        if (!received)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Standalone collaboration snapshot failed: %s",
                            received.error().c_str());
            Disconnect();
            return;
        }
        auto& snapshot = *received;

        std::unordered_map<PeerID, EditorPeer> updatedPeers;
        updatedPeers.reserve(snapshot.peers.size());
        for (const auto& brokerPeer : snapshot.peers)
        {
            EditorPeer peer;
            peer.id = brokerPeer.id;
            peer.userName = brokerPeer.name;
            peer.isActive = true;
            peer.color = kPeerColors[brokerPeer.id % (sizeof(kPeerColors) / sizeof(kPeerColors[0]))];

            InternalMessage presence;
            if (!brokerPeer.presence.empty() &&
                DeserializeMessage(reinterpret_cast<const uint8_t*>(brokerPeer.presence.data()),
                                   brokerPeer.presence.size(), presence) &&
                presence.type == InternalMessageType::Presence)
            {
                peer = presence.peerInfo;
                peer.id = brokerPeer.id;
                peer.userName = brokerPeer.name;
                peer.color = kPeerColors[brokerPeer.id % (sizeof(kPeerColors) / sizeof(kPeerColors[0]))];
                peer.isActive = true;
            }
            updatedPeers.emplace(peer.id, std::move(peer));
        }

        std::vector<EditorPeer> connectedPeers;
        std::vector<PeerID> disconnectedPeers;
        {
            std::lock_guard<std::mutex> lock(m_peerMutex);
            for (const auto& [id, peer] : updatedPeers)
            {
                if (!m_peers.contains(id))
                    connectedPeers.push_back(peer);
            }
            for (const auto& [id, peer] : m_peers)
            {
                if (!updatedPeers.contains(id))
                    disconnectedPeers.push_back(id);
            }
            m_peers = updatedPeers;
        }
        for (const auto& peer : connectedPeers)
        {
            if (m_onPeerConnected)
                m_onPeerConnected(peer);
        }
        for (PeerID id : disconnectedPeers)
        {
            if (m_onPeerDisconnected)
                m_onPeerDisconnected(id);
        }

        std::unordered_map<std::string, NodeLock> updatedLocks;
        updatedLocks.reserve(snapshot.locks.size());
        for (const auto& brokerLock : snapshot.locks)
        {
            NodeLock nodeLock;
            nodeLock.nodeId = brokerLock.nodeId;
            nodeLock.ownerPeer = brokerLock.ownerPeerId;
            if (const auto owner = updatedPeers.find(brokerLock.ownerPeerId); owner != updatedPeers.end())
                nodeLock.ownerName = owner->second.userName;
            nodeLock.lockTime = std::chrono::steady_clock::now();
            updatedLocks.emplace(nodeLock.nodeId, std::move(nodeLock));
        }

        std::vector<std::pair<std::string, PeerID>> lockChanges;
        {
            std::lock_guard<std::mutex> lock(m_lockMutex);
            for (auto& [nodeId, nodeLock] : updatedLocks)
            {
                if (const auto old = m_nodeLocks.find(nodeId);
                    old != m_nodeLocks.end() && old->second.ownerPeer == nodeLock.ownerPeer)
                    nodeLock.lockTime = old->second.lockTime;
                else
                    lockChanges.emplace_back(nodeId, nodeLock.ownerPeer);
            }
            for (const auto& [nodeId, nodeLock] : m_nodeLocks)
            {
                if (!updatedLocks.contains(nodeId))
                    lockChanges.emplace_back(nodeId, INVALID_PEER);
            }
            m_nodeLocks = std::move(updatedLocks);
        }
        for (const auto& [nodeId, owner] : lockChanges)
        {
            if (m_onLockChanged)
                m_onLockChanged(nodeId, owner);
        }

        uint64_t highestSequence = m_standaloneLastEditSequence;
        for (const auto& brokerEdit : snapshot.edits)
        {
            highestSequence = (std::max)(highestSequence, brokerEdit.sequence);
            if (!m_standaloneSnapshotInitialized || brokerEdit.sequence <= m_standaloneLastEditSequence ||
                m_standaloneLocalEditSequences.erase(brokerEdit.sequence) != 0)
                continue;

            InternalMessage message;
            if (!DeserializeMessage(reinterpret_cast<const uint8_t*>(brokerEdit.payload.data()),
                                    brokerEdit.payload.size(), message) ||
                message.type != InternalMessageType::EditBroadcast)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Editor,
                               "Ignored malformed standalone collaboration edit sequence %llu.",
                               static_cast<unsigned long long>(brokerEdit.sequence));
                continue;
            }
            message.editMessage.sourceEditor = brokerEdit.authorPeerId;
            message.editMessage.nodeId = brokerEdit.nodeId;
            ++m_editsReceived;
            if (m_onEditReceived)
                m_onEditReceived(message.editMessage);
        }
        m_standaloneLastEditSequence = highestSequence;
        m_standaloneSnapshotInitialized = true;
        std::erase_if(m_standaloneLocalEditSequences,
                      [highestSequence](uint64_t sequence) { return sequence <= highestSequence; });
    }

    void CollaborativeEditSession::PublishStandalonePresence()
    {
        if (!m_standaloneClient)
            return;

        EditorPeer localPeer;
        {
            std::lock_guard<std::mutex> lock(m_peerMutex);
            const auto it = m_peers.find(m_localPeerID);
            if (it == m_peers.end())
                return;
            it->second.lastActivityTime = m_sessionTime;
            it->second.isActive = true;
            localPeer = it->second;
        }

        InternalMessage message;
        message.type = InternalMessageType::Presence;
        message.sourcePeer = m_localPeerID;
        message.timestamp = static_cast<uint64_t>(m_sessionTime * 1000.0f);
        message.peerInfo = localPeer;
        const auto bytes = SerializeMessage(message);
        const std::string payload(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        auto published = m_standaloneClient->PublishPresence(payload);
        if (!published)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Standalone collaboration presence failed: %s",
                            published.error().c_str());
            Disconnect();
        }
    }

    bool CollaborativeEditSession::RequestStandaloneLock(const std::string& nodeId)
    {
        if (nodeId.empty())
            return false;
        {
            std::lock_guard<std::mutex> lock(m_lockMutex);
            if (const auto existing = m_nodeLocks.find(nodeId); existing != m_nodeLocks.end())
                return existing->second.ownerPeer == m_localPeerID;
        }
        auto acquired = m_standaloneClient->AcquireLock(nodeId);
        if (!acquired)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Standalone collaboration lock failed: %s",
                            acquired.error().c_str());
            return false;
        }
        if (!*acquired)
            return false;

        NodeLock nodeLock;
        nodeLock.nodeId = nodeId;
        nodeLock.ownerPeer = m_localPeerID;
        nodeLock.ownerName = m_localUserName;
        nodeLock.lockTime = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(m_lockMutex);
            m_nodeLocks[nodeId] = nodeLock;
        }
        if (m_onLockChanged)
            m_onLockChanged(nodeId, m_localPeerID);
        return true;
    }

    void CollaborativeEditSession::ReleaseStandaloneLock(const std::string& nodeId)
    {
        if (!IsLockedByLocal(nodeId))
            return;
        auto released = m_standaloneClient->ReleaseLock(nodeId);
        if (!released)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Standalone collaboration unlock failed: %s",
                            released.error().c_str());
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_lockMutex);
            m_nodeLocks.erase(nodeId);
        }
        if (m_onLockChanged)
            m_onLockChanged(nodeId, INVALID_PEER);
    }

    void CollaborativeEditSession::BroadcastStandaloneEdit(const EditMessage& edit)
    {
        const bool alreadyOwned = IsLockedByLocal(edit.nodeId);
        if (!alreadyOwned && !RequestStandaloneLock(edit.nodeId))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor,
                           "Standalone collaboration edit rejected because '%s' is locked by another peer.",
                           edit.nodeId.c_str());
            return;
        }

        InternalMessage message;
        message.type = InternalMessageType::EditBroadcast;
        message.sourcePeer = m_localPeerID;
        message.nodeId = edit.nodeId;
        message.timestamp = edit.timestamp;
        message.editMessage = edit;
        const auto bytes = SerializeMessage(message);
        const std::string payload(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        auto submitted = m_standaloneClient->SubmitEdit(edit.nodeId, payload);
        if (!submitted)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Standalone collaboration edit failed: %s",
                            submitted.error().c_str());
            if (!alreadyOwned)
                ReleaseStandaloneLock(edit.nodeId);
            return;
        }
        m_standaloneLocalEditSequences.insert(*submitted);
        ++m_editsBroadcast;
        if (m_onEditReceived)
            m_onEditReceived(edit);
        if (!alreadyOwned)
            ReleaseStandaloneLock(edit.nodeId);
    }
} // namespace SparkEditor
