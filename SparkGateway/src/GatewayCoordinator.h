/**
 * @file GatewayCoordinator.h
 * @brief Authenticated routing and fenced cross-area handoff coordination.
 */

#pragma once

#include "Engine/Networking/WorldServer.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Spark::Gateway
{
    struct AreaEndpoint
    {
        Net::AreaServerConfig area;
        std::string host = "127.0.0.1";
    };

    struct AdmissionRequest
    {
        Net::ClientID clientId = Net::INVALID_CLIENT;
        std::string sessionId;
        std::string playerName;
        std::string credential;
        XMFLOAT3 spawnPosition{0.0f, 0.0f, 0.0f};
    };

    struct AuthenticationResult
    {
        bool accepted = false;
        std::string principalId;
        std::string reason;
    };

    class IGatewayAuthenticator
    {
      public:
        virtual ~IGatewayAuthenticator() = default;
        /** [any transport thread, thread-safe] Validate an opaque credential. Never log it. */
        [[nodiscard]] virtual AuthenticationResult Authenticate(const AdmissionRequest& request) = 0;
        [[nodiscard]] virtual bool IsReady() const = 0;
    };

    enum class HandoffOperationResult : uint8_t
    {
        Applied,
        Duplicate,
        Rejected,
        Unavailable
    };

    struct HandoffCommand
    {
        std::string sessionId;
        uint64_t epoch = 0;
        Net::AreaID sourceArea = Net::INVALID_AREA;
        Net::AreaID targetArea = Net::INVALID_AREA;
    };

    /**
     * Authenticated control-plane channel to source/target SparkServer processes.
     * Implementations own transport security and must make operations idempotent
     * for the tuple (sessionId, epoch, phase).
     */
    class IAreaControlPlane
    {
      public:
        virtual ~IAreaControlPlane() = default;
        // All methods may run concurrently for different sessions and must be thread-safe.
        [[nodiscard]] virtual bool IsReady() const = 0;
        /** [any thread] Authenticated liveness of one registered area endpoint. */
        [[nodiscard]] virtual bool IsEndpointReady(Net::AreaID id) const
        {
            (void)id;
            return IsReady();
        }
        [[nodiscard]] virtual HandoffOperationResult Prepare(const HandoffCommand& command) = 0;
        [[nodiscard]] virtual HandoffOperationResult Transfer(const HandoffCommand& command) = 0;
        [[nodiscard]] virtual HandoffOperationResult Commit(const HandoffCommand& command) = 0;
        [[nodiscard]] virtual HandoffOperationResult Acknowledge(const HandoffCommand& command) = 0;
        [[nodiscard]] virtual HandoffOperationResult Abort(const HandoffCommand& command) = 0;
        /** Called after WorldServer allocates the stable runtime area ID. */
        virtual void RegisterEndpoint(Net::AreaID, const AreaEndpoint&) {}
    };

    enum class SessionState : uint8_t
    {
        Active,
        Preparing,
        Transferring,
        Committing,
        AwaitingAcknowledgement,
        Aborting
    };

    struct SessionSnapshot
    {
        std::string sessionId;
        std::string principalId;
        Net::ClientID clientId = Net::INVALID_CLIENT;
        SessionState state = SessionState::Active;
        uint64_t epoch = 0;
        Net::AreaID authoritativeArea = Net::INVALID_AREA;
        Net::AreaID targetArea = Net::INVALID_AREA;
    };

    enum class RouteFailure : uint8_t
    {
        None,
        NotReady,
        InvalidRequest,
        AuthenticationFailed,
        DuplicateSession,
        CapacityReached,
        NoAreaAvailable
    };

    struct RouteResult
    {
        bool accepted = false;
        RouteFailure failure = RouteFailure::None;
        std::string reason;
        SessionSnapshot session;
        std::string host;
        uint16_t port = 0;
    };

    enum class AdvanceResult : uint8_t
    {
        Advanced,
        WaitingForRetry,
        Aborting,
        Completed,
        StaleEpoch,
        InvalidSession,
        InvalidState
    };

    /**
     * Gateway-only session coordinator. It never owns ECS/gameplay state.
     * The source area remains authoritative until commit acknowledgement;
     * failures resolve through an explicit abort before another epoch begins.
     */
    class GatewayCoordinator
    {
      public:
        GatewayCoordinator(Net::WorldServer& worldServer, IGatewayAuthenticator& authenticator,
                           IAreaControlPlane& controlPlane);

        /** [startup thread] Register routable server endpoints with WorldServer. */
        [[nodiscard]] bool RegisterAreas(const std::vector<AreaEndpoint>& endpoints);
        /** [transport thread] Authenticate and route a new session. */
        [[nodiscard]] RouteResult Admit(const AdmissionRequest& request);
        /** [transport thread] Start or deduplicate a fenced handoff. */
        [[nodiscard]] std::optional<uint64_t> BeginHandoff(std::string_view sessionId, Net::AreaID targetArea);
        /** [transport thread] Advance exactly one handoff phase. */
        [[nodiscard]] AdvanceResult AdvanceHandoff(std::string_view sessionId, uint64_t epoch);
        /** [transport thread] Disconnect an active session; handoffs must resolve first. */
        [[nodiscard]] bool Disconnect(std::string_view sessionId);
        /** [startup thread] Reject new admissions while in-flight handoffs resolve. */
        void BeginDrain();
        /** [any thread] True when no handoff is between prepare and abort/ack resolution. */
        [[nodiscard]] bool CanShutdown() const;
        /** [any thread] Return a copy safe for health/admin reporting. */
        [[nodiscard]] std::optional<SessionSnapshot> GetSession(std::string_view sessionId) const;
        [[nodiscard]] size_t GetSessionCount() const;
        [[nodiscard]] bool IsReady() const;

      private:
        struct SessionRecord
        {
            SessionSnapshot snapshot;
        };

        [[nodiscard]] const AreaEndpoint* FindEndpoint(Net::AreaID areaId) const;

        Net::WorldServer* m_worldServer = nullptr;
        IGatewayAuthenticator* m_authenticator = nullptr;
        IAreaControlPlane* m_controlPlane = nullptr;
        std::vector<std::pair<Net::AreaID, AreaEndpoint>> m_endpoints;
        std::unordered_map<std::string, SessionRecord> m_sessions;
        bool m_accepting = true;
        mutable std::mutex m_mutex;
    };
} // namespace Spark::Gateway
