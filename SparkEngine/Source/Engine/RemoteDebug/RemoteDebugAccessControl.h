/**
 * @file RemoteDebugAccessControl.h
 * @brief Server-owned, fail-closed access control for RemoteDebug dispatch.
 *
 * SparkEngine ships no RemoteDebug transport or credential protocol. This
 * policy is therefore only used by the trusted in-process loopback. Its opaque
 * principal stays inside server/session bookkeeping and is never part of a
 * RemoteCommand or a serializable wire representation.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::RemoteDebug
{

    class RemoteDebugServer;

    /** @brief Fixed least-privilege role policy for trusted local loopback. */
    enum class RemoteDebugRole : uint8_t
    {
        Observer,
        Operator,
        Administrator
    };

    /** @brief Per-command capabilities; a command handler must declare one. */
    enum class RemoteDebugCapability : uint32_t
    {
        None = 0,
        Inspect = 1u << 0u,
        ModifyProperties = 1u << 1u,
        ExecuteConsole = 1u << 2u
    };

    [[nodiscard]] constexpr RemoteDebugCapability operator|(RemoteDebugCapability lhs, RemoteDebugCapability rhs)
    {
        return static_cast<RemoteDebugCapability>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    [[nodiscard]] constexpr bool HasRemoteDebugCapability(RemoteDebugCapability granted,
                                                           RemoteDebugCapability required)
    {
        return required == RemoteDebugCapability::None ||
               (static_cast<uint32_t>(granted) & static_cast<uint32_t>(required)) ==
                   static_cast<uint32_t>(required);
    }

    [[nodiscard]] constexpr RemoteDebugCapability CapabilitiesForRemoteDebugRole(RemoteDebugRole role)
    {
        switch (role)
        {
        case RemoteDebugRole::Observer:
            return RemoteDebugCapability::Inspect;
        case RemoteDebugRole::Operator:
            return RemoteDebugCapability::Inspect | RemoteDebugCapability::ModifyProperties;
        case RemoteDebugRole::Administrator:
            return RemoteDebugCapability::Inspect | RemoteDebugCapability::ModifyProperties |
                   RemoteDebugCapability::ExecuteConsole;
        }
        return RemoteDebugCapability::None;
    }

    /** @brief Secret-safe outcome for one attempted dispatch. */
    enum class RemoteDebugAuditDecision : uint8_t
    {
        Allowed,
        AnonymousDenied,
        InvalidPrincipalDenied,
        ExpiredPrincipalDenied,
        ReplayDenied,
        RateLimitedDenied,
        AuthorizationDenied,
        MalformedRequestDenied,
        UnknownCommandDenied
    };

    /** @brief Bounded audit data. Payloads, credentials, and grants are never retained here. */
    struct RemoteDebugAuditEvent
    {
        std::string principal;
        std::string source;
        std::string commandType;
        uint32_t requestId{0};
        RemoteDebugAuditDecision decision{RemoteDebugAuditDecision::AnonymousDenied};
    };

    /**
     * @brief Opaque server-owned association for an authenticated local endpoint.
     *
     * A default object deliberately represents no principal. The only minting
     * API is private to RemoteDebugServer; callers cannot insert the result into
     * a RemoteCommand or retrieve it from a client/session public API.
     */
    class RemoteDebugPrincipal
    {
      public:
        RemoteDebugPrincipal() = default;

        [[nodiscard]] bool IsAuthenticated() const { return m_grantId != 0; }

      private:
        uint64_t m_grantId{0};
        std::string m_subject;
        std::string m_source;
        RemoteDebugRole m_role{RemoteDebugRole::Observer};
        RemoteDebugCapability m_capabilities{RemoteDebugCapability::None};
        uint64_t m_expiresAtMs{0};

        friend class RemoteDebugServer;
        friend class RemoteDebugAccessControl;
    };

    /** @brief Authorization, replay, rate, and audit state owned by RemoteDebugServer. */
    class RemoteDebugAccessControl
    {
      public:
        static constexpr uint32_t kMaxRequestsPerWindow = 8;
        static constexpr uint64_t kRateWindowMilliseconds = 1000;
        static constexpr uint64_t kDefaultLoopbackLifetimeMilliseconds = 5 * 60 * 1000;

        [[nodiscard]] std::vector<RemoteDebugAuditEvent> GetAuditEvents() const
        {
            std::lock_guard lock(m_mutex);
            return m_auditEvents;
        }

      private:
        friend class RemoteDebugServer;

        struct GrantState
        {
            std::string subject;
            std::string source;
            RemoteDebugRole role{RemoteDebugRole::Observer};
            RemoteDebugCapability capabilities{RemoteDebugCapability::None};
            uint64_t expiresAtMs{0};
            uint32_t highestRequestId{0};
            uint64_t rateWindowStartedAtMs{0};
            uint32_t requestsInWindow{0};
        };

        struct AuthorizationResult
        {
            bool allowed{false};
            RemoteDebugAuditDecision denial{RemoteDebugAuditDecision::AnonymousDenied};
        };

        [[nodiscard]] static uint64_t CurrentTimeMilliseconds()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
        }

        [[nodiscard]] static bool IsWellFormedCommandType(const std::string& type)
        {
            if (type.empty() || type.size() > 64)
                return false;
            for (const unsigned char character : type)
            {
                const bool alphaNumeric = (character >= 'a' && character <= 'z') ||
                                          (character >= 'A' && character <= 'Z') ||
                                          (character >= '0' && character <= '9');
                if (!alphaNumeric && character != '_' && character != '-')
                    return false;
            }
            return true;
        }

        [[nodiscard]] static std::string SafeCommandType(const std::string& type)
        {
            return IsWellFormedCommandType(type) ? type : "<invalid>";
        }

        [[nodiscard]] static bool PrincipalMatches(const RemoteDebugPrincipal& principal, const GrantState& state)
        {
            return principal.m_grantId != 0 && principal.m_subject == state.subject &&
                   principal.m_source == state.source && principal.m_role == state.role &&
                   principal.m_capabilities == state.capabilities && principal.m_expiresAtMs == state.expiresAtMs;
        }

        [[nodiscard]] RemoteDebugPrincipal IssueLoopbackPrincipal(RemoteDebugRole role, uint64_t lifetimeMilliseconds)
        {
            std::lock_guard lock(m_mutex);
            const uint64_t now = CurrentTimeMilliseconds();
            uint64_t grantId = m_nextGrantId++;
            if (grantId == 0)
                grantId = m_nextGrantId++;

            const uint64_t expiresAt = lifetimeMilliseconds > std::numeric_limits<uint64_t>::max() - now
                                           ? std::numeric_limits<uint64_t>::max()
                                           : now + lifetimeMilliseconds;
            const auto capabilities = CapabilitiesForRemoteDebugRole(role);
            GrantState state{"trusted-local-loopback", "in-process-loopback", role, capabilities, expiresAt, 0, now,
                             0};
            m_grants.emplace(grantId, state);

            RemoteDebugPrincipal principal;
            principal.m_grantId = grantId;
            principal.m_subject = state.subject;
            principal.m_source = state.source;
            principal.m_role = state.role;
            principal.m_capabilities = state.capabilities;
            principal.m_expiresAtMs = state.expiresAtMs;
            return principal;
        }

        [[nodiscard]] AuthorizationResult Authorize(const RemoteDebugPrincipal& principal, const std::string& commandType,
                                                     uint32_t requestId, size_t payloadSize,
                                                     RemoteDebugCapability requiredCapability)
        {
            std::lock_guard lock(m_mutex);
            if (!principal.IsAuthenticated())
            {
                RecordLocked("anonymous", "unbound", SafeCommandType(commandType), requestId,
                             RemoteDebugAuditDecision::AnonymousDenied);
                return {false, RemoteDebugAuditDecision::AnonymousDenied};
            }

            auto grantIt = m_grants.find(principal.m_grantId);
            if (grantIt == m_grants.end() || !PrincipalMatches(principal, grantIt->second))
            {
                RecordLocked("unverified", "unverified", SafeCommandType(commandType), requestId,
                             RemoteDebugAuditDecision::InvalidPrincipalDenied);
                return {false, RemoteDebugAuditDecision::InvalidPrincipalDenied};
            }

            GrantState& state = grantIt->second;
            const uint64_t now = CurrentTimeMilliseconds();
            if (now >= state.expiresAtMs)
            {
                RecordLocked(state.subject, state.source, SafeCommandType(commandType), requestId,
                             RemoteDebugAuditDecision::ExpiredPrincipalDenied);
                return {false, RemoteDebugAuditDecision::ExpiredPrincipalDenied};
            }

            if (!IsWellFormedCommandType(commandType) || payloadSize > 4096 || requestId == 0)
            {
                RecordLocked(state.subject, state.source, SafeCommandType(commandType), requestId,
                             RemoteDebugAuditDecision::MalformedRequestDenied);
                return {false, RemoteDebugAuditDecision::MalformedRequestDenied};
            }

            if (requestId <= state.highestRequestId)
            {
                RecordLocked(state.subject, state.source, commandType, requestId, RemoteDebugAuditDecision::ReplayDenied);
                return {false, RemoteDebugAuditDecision::ReplayDenied};
            }
            state.highestRequestId = requestId;

            if (now - state.rateWindowStartedAtMs >= kRateWindowMilliseconds)
            {
                state.rateWindowStartedAtMs = now;
                state.requestsInWindow = 0;
            }
            if (state.requestsInWindow >= kMaxRequestsPerWindow)
            {
                RecordLocked(state.subject, state.source, commandType, requestId,
                             RemoteDebugAuditDecision::RateLimitedDenied);
                return {false, RemoteDebugAuditDecision::RateLimitedDenied};
            }
            ++state.requestsInWindow;

            if (!HasRemoteDebugCapability(state.capabilities, requiredCapability))
            {
                RecordLocked(state.subject, state.source, commandType, requestId,
                             RemoteDebugAuditDecision::AuthorizationDenied);
                return {false, RemoteDebugAuditDecision::AuthorizationDenied};
            }
            return {true, RemoteDebugAuditDecision::Allowed};
        }

        void RecordOutcome(const RemoteDebugPrincipal& principal, const std::string& commandType, uint32_t requestId,
                           RemoteDebugAuditDecision decision)
        {
            std::lock_guard lock(m_mutex);
            const auto grantIt = m_grants.find(principal.m_grantId);
            if (grantIt == m_grants.end() || !PrincipalMatches(principal, grantIt->second))
            {
                RecordLocked("unverified", "unverified", SafeCommandType(commandType), requestId,
                             RemoteDebugAuditDecision::InvalidPrincipalDenied);
                return;
            }
            RecordLocked(grantIt->second.subject, grantIt->second.source, SafeCommandType(commandType), requestId,
                         decision);
        }

        /**
         * @brief Revoke every active principal while retaining bounded audit history.
         *
         * StopListening uses this only after taking the server's exclusive
         * execution gate. Therefore an Allowed event always describes an effect
         * that completed before revocation returned, rather than a post-hoc
         * decision recorded after the grant was already removed.
         */
        void RevokeAll()
        {
            std::lock_guard lock(m_mutex);
            m_grants.clear();
        }

        void RecordLocked(const std::string& principal, const std::string& source, const std::string& commandType,
                          uint32_t requestId, RemoteDebugAuditDecision decision)
        {
            constexpr size_t kMaxAuditEvents = 256;
            if (m_auditEvents.size() >= kMaxAuditEvents)
                m_auditEvents.erase(m_auditEvents.begin());
            m_auditEvents.push_back({principal, source, commandType, requestId, decision});
        }

        mutable std::mutex m_mutex;
        std::unordered_map<uint64_t, GrantState> m_grants;
        std::vector<RemoteDebugAuditEvent> m_auditEvents;
        uint64_t m_nextGrantId{1};
    };

} // namespace Spark::RemoteDebug
