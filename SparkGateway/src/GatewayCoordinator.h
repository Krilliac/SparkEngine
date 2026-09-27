/**
 * @file GatewayCoordinator.h
 * @brief Authenticated routing and fenced cross-area handoff coordination.
 */

#pragma once

#include "Engine/Networking/WorldServer.h"

#include <cstdint>
#include <functional>
#include <memory>
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

    class GuardedGatewayAuthenticator;
    struct GatewayAuthenticatorHealth;

    /**
     * Gateway-only session coordinator. It never owns ECS/gameplay state.
     * The source area remains authoritative until commit acknowledgement;
     * failures resolve through an explicit abort before another epoch begins.
     * Every admission goes through a GuardedGatewayAuthenticator around the
     * given authenticator, so a throwing, stalling or failing adapter fails
     * closed and is counted (GetAuthenticationHealth()).
     */
    class GatewayCoordinator
    {
      public:
        GatewayCoordinator(Net::WorldServer& worldServer, IGatewayAuthenticator& authenticator,
                           IAreaControlPlane& controlPlane);
        ~GatewayCoordinator();

        GatewayCoordinator(const GatewayCoordinator&) = delete;
        GatewayCoordinator& operator=(const GatewayCoordinator&) = delete;

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
        /**
         * [any thread] Health readiness: routable, and the authenticator circuit is not failing
         * fast. False during the circuit cooldown, when every admission would be rejected.
         */
        [[nodiscard]] bool IsReady() const;
        /** [any thread] Fault, budget and circuit counters of the guarded authenticator. */
        [[nodiscard]] GatewayAuthenticatorHealth GetAuthenticationHealth() const;

      private:
        struct SessionRecord
        {
            SessionSnapshot snapshot;
        };

        [[nodiscard]] const AreaEndpoint* FindEndpoint(Net::AreaID areaId) const;
        /**
         * World, adapters, areas and drain state allow routing. Admit() gates on this rather than
         * IsReady() so a fail-fast admission reaches the guard and is counted as rejectedWhileOpen.
         */
        [[nodiscard]] bool IsRoutable() const;

        Net::WorldServer* m_worldServer = nullptr;
        // Owned front over the caller's (non-owned) authenticator; set once in the constructor.
        std::unique_ptr<GuardedGatewayAuthenticator> m_authenticator;
        IAreaControlPlane* m_controlPlane = nullptr;
        std::vector<std::pair<Net::AreaID, AreaEndpoint>> m_endpoints;
        std::unordered_map<std::string, SessionRecord> m_sessions;
        bool m_accepting = true;
        mutable std::mutex m_mutex;
    };
} // namespace Spark::Gateway
