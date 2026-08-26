/**
 * @file CollaborationService.cpp
 * @brief Headless collaboration broker implementation.
 */

#include "CollaborationService.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <random>

namespace Spark::Daemon
{
    CollaborationService::CollaborationService(CollaborationConfig config) : m_config(std::move(config))
    {
        m_config.maximumSessions = std::clamp<size_t>(m_config.maximumSessions, 1, 1024);
        m_config.maximumPeersPerSession = std::clamp<size_t>(m_config.maximumPeersPerSession, 1, 1024);
        m_config.maximumLocksPerSession = std::clamp<size_t>(m_config.maximumLocksPerSession, 1, 1'000'000);
        m_config.maximumEditHistory = std::clamp<size_t>(m_config.maximumEditHistory, 1, 1'000'000);
        m_config.peerTimeout =
            std::clamp(m_config.peerTimeout, std::chrono::seconds(5), std::chrono::seconds(24 * 60 * 60));
    }

    std::optional<ServiceResponse> CollaborationService::HandleMessage(uint16_t messageType,
                                                                       const std::vector<uint8_t>& payload)
    {
        switch (static_cast<CollaborationMessage>(messageType))
        {
        case CollaborationMessage::CreateSessionRequest:
            return Create(payload);
        case CollaborationMessage::DeleteSessionRequest:
            return Delete(payload);
        case CollaborationMessage::JoinSessionRequest:
            return Join(payload);
        case CollaborationMessage::LeaveSessionRequest:
            return Leave(payload);
        case CollaborationMessage::PresenceRequest:
            return Presence(payload);
        case CollaborationMessage::AcquireLockRequest:
            return AcquireLock(payload);
        case CollaborationMessage::ReleaseLockRequest:
            return ReleaseLock(payload);
        case CollaborationMessage::SubmitEditRequest:
            return SubmitEdit(payload);
        case CollaborationMessage::SnapshotRequest:
            return Snapshot(payload);
        default:
            return MakeError("unsupported collaboration message");
        }
    }

    ServiceResponse CollaborationService::Create(const std::vector<uint8_t>& payload)
    {
        Wire::Reader reader(payload);
        std::string id;
        if (!Wire::ReadVersion(reader) || !reader.ReadString(id, kMaximumSessionIdLength) || !reader.Finished() ||
            !IsValidIdentifier(id))
            return MakeError("malformed or invalid session id");

        std::lock_guard lock(m_mutex);
        if (m_sessions.contains(id))
            return MakeError("session already exists");
        if (m_sessions.size() >= m_config.maximumSessions)
            return MakeError("session limit reached");
        SessionRecord session;
        session.id = id;
        session.administrationToken = GenerateToken();
        if (session.administrationToken.empty())
            return MakeError("secure token generation failed");
        const std::string token = session.administrationToken;
        m_sessions.emplace(id, std::move(session));

        ServiceResponse response;
        response.messageType = static_cast<uint16_t>(CollaborationMessage::CreateSessionResponse);
        if (!EncodeSessionSecret(id, token, response.payload))
            return MakeError("could not encode create-session response");
        return response;
    }

    ServiceResponse CollaborationService::Delete(const std::vector<uint8_t>& payload)
    {
        std::string id;
        std::string token;
        if (!DecodeSessionSecret(payload, id, token))
            return MakeError("malformed delete-session request");
        std::lock_guard lock(m_mutex);
        auto it = m_sessions.find(id);
        if (it == m_sessions.end() || !SecureEquals(it->second.administrationToken, token))
            return MakeError("session not found or administration token invalid");
        m_sessions.erase(it);
        return MakeAck(CollaborationMessage::DeleteSessionResponse);
    }

    ServiceResponse CollaborationService::Join(const std::vector<uint8_t>& payload)
    {
        std::string sessionId;
        std::string name;
        if (!DecodeJoinRequest(payload, sessionId, name) || name.empty())
            return MakeError("malformed join request");
        std::lock_guard lock(m_mutex);
        auto sessionIt = m_sessions.find(sessionId);
        if (sessionIt == m_sessions.end())
            return MakeError("session not found");
        auto& session = sessionIt->second;
        PruneExpiredLocked(session, std::chrono::steady_clock::now());
        if (session.peers.size() >= m_config.maximumPeersPerSession || session.nextPeerId == 0)
            return MakeError("session peer limit reached");

        PeerRecord record;
        record.peer.id = session.nextPeerId++;
        record.peer.name = std::move(name);
        record.token = GenerateToken();
        if (record.token.empty())
            return MakeError("secure token generation failed");
        record.lastSeen = std::chrono::steady_clock::now();
        const uint32_t peerId = record.peer.id;
        const std::string token = record.token;
        session.peers.emplace(peerId, std::move(record));

        ServiceResponse response;
        response.messageType = static_cast<uint16_t>(CollaborationMessage::JoinSessionResponse);
        if (!EncodeJoinResponse(peerId, token, response.payload))
            return MakeError("could not encode join response");
        return response;
    }

    ServiceResponse CollaborationService::Leave(const std::vector<uint8_t>& payload)
    {
        CollaborationAuth auth;
        if (!DecodeCollaborationAuth(payload, auth))
            return MakeError("malformed leave request");
        std::lock_guard lock(m_mutex);
        auto* session = AuthenticateLocked(auth);
        if (!session)
            return MakeError("invalid collaboration capability");
        RemovePeerLocked(*session, auth.peerId);
        return MakeAck(CollaborationMessage::LeaveSessionResponse);
    }

    ServiceResponse CollaborationService::Presence(const std::vector<uint8_t>& payload)
    {
        CollaborationAuth auth;
        std::string presence;
        if (!DecodeAuthString(payload, auth, presence, kMaximumPresenceLength))
            return MakeError("malformed presence request");
        std::lock_guard lock(m_mutex);
        auto* session = AuthenticateLocked(auth);
        if (!session)
            return MakeError("invalid collaboration capability");
        auto& peer = session->peers.at(auth.peerId);
        peer.peer.presence = std::move(presence);
        peer.lastSeen = std::chrono::steady_clock::now();
        return MakeAck(CollaborationMessage::PresenceResponse);
    }

    ServiceResponse CollaborationService::AcquireLock(const std::vector<uint8_t>& payload)
    {
        CollaborationAuth auth;
        std::string nodeId;
        if (!DecodeAuthString(payload, auth, nodeId, kMaximumNodeIdLength) || nodeId.empty())
            return MakeError("malformed lock request");
        std::lock_guard lock(m_mutex);
        auto* session = AuthenticateLocked(auth);
        if (!session)
            return MakeError("invalid collaboration capability");
        PruneExpiredLocked(*session, std::chrono::steady_clock::now());
        bool acquired = false;
        auto lockIt = session->locks.find(nodeId);
        if (lockIt != session->locks.end())
            acquired = lockIt->second == auth.peerId;
        else if (session->locks.size() < m_config.maximumLocksPerSession)
        {
            session->locks.emplace(std::move(nodeId), auth.peerId);
            acquired = true;
        }
        ServiceResponse response;
        response.messageType = static_cast<uint16_t>(CollaborationMessage::AcquireLockResponse);
        EncodeBoolean(acquired, response.payload);
        return response;
    }

    ServiceResponse CollaborationService::ReleaseLock(const std::vector<uint8_t>& payload)
    {
        CollaborationAuth auth;
        std::string nodeId;
        if (!DecodeAuthString(payload, auth, nodeId, kMaximumNodeIdLength) || nodeId.empty())
            return MakeError("malformed release-lock request");
        std::lock_guard lock(m_mutex);
        auto* session = AuthenticateLocked(auth);
        if (!session)
            return MakeError("invalid collaboration capability");
        auto lockIt = session->locks.find(nodeId);
        if (lockIt == session->locks.end() || lockIt->second != auth.peerId)
            return MakeError("peer does not own the node lock");
        session->locks.erase(lockIt);
        return MakeAck(CollaborationMessage::ReleaseLockResponse);
    }

    ServiceResponse CollaborationService::SubmitEdit(const std::vector<uint8_t>& payload)
    {
        CollaborationAuth auth;
        std::string nodeId;
        std::string editPayload;
        if (!DecodeEditRequest(payload, auth, nodeId, editPayload) || nodeId.empty() || editPayload.empty())
            return MakeError("malformed edit request");
        std::lock_guard lock(m_mutex);
        auto* session = AuthenticateLocked(auth);
        if (!session)
            return MakeError("invalid collaboration capability");
        auto lockIt = session->locks.find(nodeId);
        if (lockIt == session->locks.end() || lockIt->second != auth.peerId)
            return MakeError("edit requires a node lock owned by the submitting peer");
        CollaborationEdit edit;
        edit.sequence = session->nextSequence++;
        edit.authorPeerId = auth.peerId;
        edit.nodeId = std::move(nodeId);
        edit.payload = std::move(editPayload);
        const uint64_t sequence = edit.sequence;
        session->edits.push_back(std::move(edit));
        while (session->edits.size() > m_config.maximumEditHistory)
            session->edits.pop_front();

        ServiceResponse response;
        response.messageType = static_cast<uint16_t>(CollaborationMessage::SubmitEditResponse);
        EncodeSequence(sequence, response.payload);
        return response;
    }

    ServiceResponse CollaborationService::Snapshot(const std::vector<uint8_t>& payload)
    {
        CollaborationAuth auth;
        if (!DecodeCollaborationAuth(payload, auth))
            return MakeError("malformed snapshot request");
        std::lock_guard lock(m_mutex);
        auto* session = AuthenticateLocked(auth);
        if (!session)
            return MakeError("invalid collaboration capability");
        PruneExpiredLocked(*session, std::chrono::steady_clock::now());

        CollaborationSnapshot snapshot;
        snapshot.sessionId = session->id;
        snapshot.nextSequence = session->nextSequence;
        snapshot.peers.reserve(session->peers.size());
        for (const auto& [id, peer] : session->peers)
            snapshot.peers.push_back(peer.peer);
        std::sort(snapshot.peers.begin(), snapshot.peers.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.id < rhs.id; });
        snapshot.locks.reserve(session->locks.size());
        for (const auto& [nodeId, peerId] : session->locks)
            snapshot.locks.push_back({nodeId, peerId});
        std::sort(snapshot.locks.begin(), snapshot.locks.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.nodeId < rhs.nodeId; });
        snapshot.edits.assign(session->edits.begin(), session->edits.end());

        ServiceResponse response;
        response.messageType = static_cast<uint16_t>(CollaborationMessage::SnapshotResponse);
        if (!EncodeSnapshot(snapshot, response.payload))
            return MakeError("could not encode collaboration snapshot");
        return response;
    }

    size_t CollaborationService::GetSessionCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_sessions.size();
    }

    ServiceResponse CollaborationService::MakeError(std::string message) const
    {
        ServiceResponse response;
        response.messageType = static_cast<uint16_t>(ControlMessage::ErrorResponse);
        response.payload.assign(message.begin(), message.end());
        return response;
    }

    ServiceResponse CollaborationService::MakeAck(CollaborationMessage type) const
    {
        Wire::Writer writer;
        Wire::WriteVersion(writer);
        return {static_cast<uint16_t>(type), writer.Take()};
    }

    CollaborationService::SessionRecord* CollaborationService::AuthenticateLocked(const CollaborationAuth& auth)
    {
        auto sessionIt = m_sessions.find(auth.sessionId);
        if (sessionIt == m_sessions.end())
            return nullptr;
        auto peerIt = sessionIt->second.peers.find(auth.peerId);
        if (peerIt == sessionIt->second.peers.end() || !SecureEquals(peerIt->second.token, auth.token))
            return nullptr;
        peerIt->second.lastSeen = std::chrono::steady_clock::now();
        return &sessionIt->second;
    }

    void CollaborationService::RemovePeerLocked(SessionRecord& session, uint32_t peerId)
    {
        session.peers.erase(peerId);
        for (auto it = session.locks.begin(); it != session.locks.end();)
        {
            if (it->second == peerId)
                it = session.locks.erase(it);
            else
                ++it;
        }
    }

    void CollaborationService::PruneExpiredLocked(SessionRecord& session, std::chrono::steady_clock::time_point now)
    {
        std::vector<uint32_t> expired;
        for (const auto& [peerId, peer] : session.peers)
            if (now - peer.lastSeen > m_config.peerTimeout)
                expired.push_back(peerId);
        for (uint32_t peerId : expired)
            RemovePeerLocked(session, peerId);
    }

    std::string CollaborationService::GenerateToken()
    {
        std::array<uint8_t, 32> bytes{};
#if !defined(_WIN32)
        std::ifstream random("/dev/urandom", std::ios::binary);
        random.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!random)
            return {};
#else
        std::random_device random;
        for (auto& byte : bytes)
            byte = static_cast<uint8_t>(random());
#endif
        static constexpr char hex[] = "0123456789abcdef";
        std::string token;
        token.resize(bytes.size() * 2);
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            token[i * 2] = hex[bytes[i] >> 4];
            token[i * 2 + 1] = hex[bytes[i] & 0x0f];
        }
        return token;
    }

    bool CollaborationService::SecureEquals(std::string_view lhs, std::string_view rhs) noexcept
    {
        size_t difference = lhs.size() ^ rhs.size();
        const size_t count = std::max(lhs.size(), rhs.size());
        for (size_t i = 0; i < count; ++i)
        {
            const unsigned char left = i < lhs.size() ? static_cast<unsigned char>(lhs[i]) : 0;
            const unsigned char right = i < rhs.size() ? static_cast<unsigned char>(rhs[i]) : 0;
            difference |= left ^ right;
        }
        return difference == 0;
    }

    bool CollaborationService::IsValidIdentifier(std::string_view id)
    {
        if (id.empty() || id.size() > kMaximumSessionIdLength)
            return false;
        return std::all_of(id.begin(), id.end(),
                           [](unsigned char c) { return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.'; });
    }
} // namespace Spark::Daemon
