/**
 * @file StandaloneCollaborationClient.h
 * @brief Typed editor/client transport for the standalone collaboration broker.
 */

#pragma once

#include "CollaborationProtocol.h"
#include "Utils/DaemonClient.h"

#include <expected>
#include <string>
#include <string_view>

namespace SparkEditor
{
    /**
     * @brief Production client for SparkCollabServer's capability-based protocol.
     *
     * The client owns one local IPC connection and one optional peer capability.
     * It intentionally contains no editor scene logic; CollaborativeEditSession
     * translates editor presence and edit messages at this boundary.
     */
    class StandaloneCollaborationClient
    {
      public:
        StandaloneCollaborationClient() = default;
        ~StandaloneCollaborationClient();

        StandaloneCollaborationClient(const StandaloneCollaborationClient&) = delete;
        StandaloneCollaborationClient& operator=(const StandaloneCollaborationClient&) = delete;

        [[nodiscard]] std::expected<void, std::string> Connect(std::string endpoint);
        void Disconnect();
        [[nodiscard]] bool IsConnected() const;

        [[nodiscard]] std::expected<std::string, std::string> CreateSession(std::string_view sessionId);
        [[nodiscard]] std::expected<void, std::string> DeleteSession(std::string_view sessionId,
                                                                     std::string_view administrationToken);
        [[nodiscard]] std::expected<uint32_t, std::string> JoinSession(std::string_view sessionId,
                                                                       std::string_view peerName);
        [[nodiscard]] std::expected<void, std::string> LeaveSession();

        [[nodiscard]] std::expected<void, std::string> PublishPresence(std::string_view presence);
        [[nodiscard]] std::expected<bool, std::string> AcquireLock(std::string_view nodeId);
        [[nodiscard]] std::expected<void, std::string> ReleaseLock(std::string_view nodeId);
        [[nodiscard]] std::expected<uint64_t, std::string> SubmitEdit(std::string_view nodeId,
                                                                      std::string_view payload);
        [[nodiscard]] std::expected<Spark::Daemon::CollaborationSnapshot, std::string> GetSnapshot();

        [[nodiscard]] uint32_t GetPeerId() const noexcept { return m_auth.peerId; }
        [[nodiscard]] const std::string& GetEndpoint() const noexcept { return m_endpoint; }
        [[nodiscard]] const std::string& GetSessionId() const noexcept { return m_auth.sessionId; }

      private:
        [[nodiscard]] std::expected<Spark::Daemon::Response, std::string> Request(
            Spark::Daemon::CollaborationMessage requestType, Spark::Daemon::CollaborationMessage responseType,
            const std::vector<uint8_t>& payload);

        Spark::Daemon::DaemonClient m_transport;
        Spark::Daemon::CollaborationAuth m_auth;
        std::string m_endpoint;
    };
} // namespace SparkEditor
