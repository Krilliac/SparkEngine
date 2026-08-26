/**
 * @file StandaloneCollaborationClient.cpp
 * @brief Typed editor/client transport for SparkCollabServer.
 */

#include "StandaloneCollaborationClient.h"

namespace SparkEditor
{
    namespace
    {
        std::vector<uint8_t> EncodeSessionId(std::string_view sessionId)
        {
            Spark::Daemon::Wire::Writer writer;
            Spark::Daemon::Wire::WriteVersion(writer);
            if (!writer.WriteString(sessionId, Spark::Daemon::kMaximumSessionIdLength))
                return {};
            return writer.Take();
        }
    } // namespace

    StandaloneCollaborationClient::~StandaloneCollaborationClient()
    {
        Disconnect();
    }

    std::expected<void, std::string> StandaloneCollaborationClient::Connect(std::string endpoint)
    {
        Disconnect();
        if (endpoint.empty())
            return std::unexpected("collaboration endpoint is empty");
        auto connected = m_transport.Connect(endpoint);
        if (!connected)
            return std::unexpected(connected.error());
        auto pinged = m_transport.Ping();
        if (!pinged)
        {
            m_transport.Disconnect();
            return std::unexpected(pinged.error());
        }
        m_endpoint = std::move(endpoint);
        return {};
    }

    void StandaloneCollaborationClient::Disconnect()
    {
        if (!m_auth.sessionId.empty() && m_transport.IsConnected())
            (void)LeaveSession();
        m_transport.Disconnect();
        m_auth = {};
        m_endpoint.clear();
    }

    bool StandaloneCollaborationClient::IsConnected() const
    {
        return m_transport.IsConnected();
    }

    std::expected<Spark::Daemon::Response, std::string> StandaloneCollaborationClient::Request(
        Spark::Daemon::CollaborationMessage requestType, Spark::Daemon::CollaborationMessage responseType,
        const std::vector<uint8_t>& payload)
    {
        auto response =
            m_transport.Request(Spark::Daemon::ServiceId::Collab, static_cast<uint16_t>(requestType), payload);
        if (!response)
            return std::unexpected(response.error());
        if (response->messageType != static_cast<uint16_t>(responseType))
            return std::unexpected("SparkCollabServer returned an unexpected response type");
        return std::move(*response);
    }

    std::expected<std::string, std::string> StandaloneCollaborationClient::CreateSession(std::string_view sessionId)
    {
        const auto payload = EncodeSessionId(sessionId);
        if (payload.empty())
            return std::unexpected("collaboration session id exceeds protocol bounds");
        auto response = Request(Spark::Daemon::CollaborationMessage::CreateSessionRequest,
                                Spark::Daemon::CollaborationMessage::CreateSessionResponse, payload);
        if (!response)
            return std::unexpected(response.error());
        std::string returnedSession;
        std::string administrationToken;
        if (!Spark::Daemon::DecodeSessionSecret(response->payload, returnedSession, administrationToken) ||
            returnedSession != sessionId)
            return std::unexpected("SparkCollabServer returned a malformed create-session response");
        return administrationToken;
    }

    std::expected<void, std::string> StandaloneCollaborationClient::DeleteSession(std::string_view sessionId,
                                                                                  std::string_view administrationToken)
    {
        std::vector<uint8_t> payload;
        if (!Spark::Daemon::EncodeSessionSecret(sessionId, administrationToken, payload))
            return std::unexpected("collaboration administration capability exceeds protocol bounds");
        auto response = Request(Spark::Daemon::CollaborationMessage::DeleteSessionRequest,
                                Spark::Daemon::CollaborationMessage::DeleteSessionResponse, payload);
        if (!response)
            return std::unexpected(response.error());
        return {};
    }

    std::expected<uint32_t, std::string> StandaloneCollaborationClient::JoinSession(std::string_view sessionId,
                                                                                    std::string_view peerName)
    {
        if (!m_auth.sessionId.empty())
            return std::unexpected("client has already joined a collaboration session");
        std::vector<uint8_t> payload;
        if (!Spark::Daemon::EncodeJoinRequest(sessionId, peerName, payload))
            return std::unexpected("collaboration join fields exceed protocol bounds");
        auto response = Request(Spark::Daemon::CollaborationMessage::JoinSessionRequest,
                                Spark::Daemon::CollaborationMessage::JoinSessionResponse, payload);
        if (!response)
            return std::unexpected(response.error());
        uint32_t peerId = 0;
        std::string token;
        if (!Spark::Daemon::DecodeJoinResponse(response->payload, peerId, token) || peerId == 0)
            return std::unexpected("SparkCollabServer returned a malformed join response");
        m_auth = {std::string(sessionId), peerId, std::move(token)};
        return peerId;
    }

    std::expected<void, std::string> StandaloneCollaborationClient::LeaveSession()
    {
        if (m_auth.sessionId.empty())
            return {};
        std::vector<uint8_t> payload;
        if (!Spark::Daemon::EncodeCollaborationAuth(m_auth, payload))
            return std::unexpected("collaboration capability exceeds protocol bounds");
        auto response = Request(Spark::Daemon::CollaborationMessage::LeaveSessionRequest,
                                Spark::Daemon::CollaborationMessage::LeaveSessionResponse, payload);
        if (!response)
            return std::unexpected(response.error());
        m_auth = {};
        return {};
    }

    std::expected<void, std::string> StandaloneCollaborationClient::PublishPresence(std::string_view presence)
    {
        std::vector<uint8_t> payload;
        if (!Spark::Daemon::EncodeAuthString(m_auth, presence, Spark::Daemon::kMaximumPresenceLength, payload))
            return std::unexpected("collaboration presence exceeds protocol bounds");
        auto response = Request(Spark::Daemon::CollaborationMessage::PresenceRequest,
                                Spark::Daemon::CollaborationMessage::PresenceResponse, payload);
        if (!response)
            return std::unexpected(response.error());
        return {};
    }

    std::expected<bool, std::string> StandaloneCollaborationClient::AcquireLock(std::string_view nodeId)
    {
        std::vector<uint8_t> payload;
        if (!Spark::Daemon::EncodeAuthString(m_auth, nodeId, Spark::Daemon::kMaximumNodeIdLength, payload))
            return std::unexpected("collaboration node id exceeds protocol bounds");
        auto response = Request(Spark::Daemon::CollaborationMessage::AcquireLockRequest,
                                Spark::Daemon::CollaborationMessage::AcquireLockResponse, payload);
        if (!response)
            return std::unexpected(response.error());
        bool acquired = false;
        if (!Spark::Daemon::DecodeBoolean(response->payload, acquired))
            return std::unexpected("SparkCollabServer returned a malformed lock response");
        return acquired;
    }

    std::expected<void, std::string> StandaloneCollaborationClient::ReleaseLock(std::string_view nodeId)
    {
        std::vector<uint8_t> payload;
        if (!Spark::Daemon::EncodeAuthString(m_auth, nodeId, Spark::Daemon::kMaximumNodeIdLength, payload))
            return std::unexpected("collaboration node id exceeds protocol bounds");
        auto response = Request(Spark::Daemon::CollaborationMessage::ReleaseLockRequest,
                                Spark::Daemon::CollaborationMessage::ReleaseLockResponse, payload);
        if (!response)
            return std::unexpected(response.error());
        return {};
    }

    std::expected<uint64_t, std::string> StandaloneCollaborationClient::SubmitEdit(std::string_view nodeId,
                                                                                   std::string_view edit)
    {
        std::vector<uint8_t> payload;
        if (!Spark::Daemon::EncodeEditRequest(m_auth, nodeId, edit, payload))
            return std::unexpected("collaboration edit exceeds protocol bounds");
        auto response = Request(Spark::Daemon::CollaborationMessage::SubmitEditRequest,
                                Spark::Daemon::CollaborationMessage::SubmitEditResponse, payload);
        if (!response)
            return std::unexpected(response.error());
        uint64_t sequence = 0;
        if (!Spark::Daemon::DecodeSequence(response->payload, sequence) || sequence == 0)
            return std::unexpected("SparkCollabServer returned a malformed edit response");
        return sequence;
    }

    std::expected<Spark::Daemon::CollaborationSnapshot, std::string> StandaloneCollaborationClient::GetSnapshot()
    {
        std::vector<uint8_t> payload;
        if (!Spark::Daemon::EncodeCollaborationAuth(m_auth, payload))
            return std::unexpected("collaboration capability exceeds protocol bounds");
        auto response = Request(Spark::Daemon::CollaborationMessage::SnapshotRequest,
                                Spark::Daemon::CollaborationMessage::SnapshotResponse, payload);
        if (!response)
            return std::unexpected(response.error());
        Spark::Daemon::CollaborationSnapshot snapshot;
        if (!Spark::Daemon::DecodeSnapshot(response->payload, snapshot) || snapshot.sessionId != m_auth.sessionId)
            return std::unexpected("SparkCollabServer returned a malformed snapshot");
        return snapshot;
    }
} // namespace SparkEditor
