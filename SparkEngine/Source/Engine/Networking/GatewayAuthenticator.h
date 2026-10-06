/**
 * @file GatewayAuthenticator.h
 * @brief Admission authenticator contract and its fail-closed guard (circuit breaker, call budget).
 *
 * The authenticator seam (boundary B1 in docs/specs/online-services.md) is shared by SparkGateway's
 * GatewayCoordinator and by game-module session gates (SparkGameMMO's MMOSessionGate), so it lives in
 * the engine rather than in either product. Both install GuardedGatewayAuthenticator around whatever
 * authenticator they are given, so a product adapter that throws, stalls or loses its backend fails
 * closed and is observable (docs/specs/online-services.md section 5.2):
 * - An exception becomes a rejected admission with a fixed reason. The exception text is
 *   never surfaced or logged, because an adapter may have put the credential in it.
 * - A call slower than the budget (default 2 s, the ingress deadline) is rejected even if the
 *   adapter accepted it, since the ingress client has already given up on the admission.
 * - Exceptions and budget overruns are faults. After failureThreshold consecutive faults the
 *   circuit opens and admissions are rejected without reaching the adapter until the cooldown
 *   has elapsed; the next call is then a probe. A rejection is a healthy answer, not a fault.
 * - A rejection reason that echoes the credential is redacted.
 *
 * Thread affinity: Authenticate() and IsReady() may run concurrently on any transport thread;
 * the guard's counters are guarded by a mutex that is never held across the adapter call.
 * Ownership: the guard is non-owning; the wrapped authenticator must outlive it.
 * Allocation: none per call beyond the adapter's own result strings.
 * Scalability tier: one guard per admission front (gateway process or module session gate).
 */

#pragma once

#include "../../Core/Platform.h"
#include "../../Utils/LogMacros.h"
#include "NetworkClientId.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>

namespace Spark::Gateway
{
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

    /** @brief Budget from spec section 5.2: 2 s per call, 5 consecutive faults open the circuit for 30 s */
    struct GatewayAuthenticatorPolicy
    {
        uint32_t failureThreshold = 5;
        std::chrono::steady_clock::duration cooldown = std::chrono::seconds(30);
        std::chrono::steady_clock::duration callBudget = std::chrono::seconds(2);
    };

    /** @brief Counters reported by GatewayCoordinator::GetAuthenticationHealth() */
    struct GatewayAuthenticatorHealth
    {
        uint64_t accepted = 0;          ///< Admissions the adapter accepted within budget
        uint64_t rejected = 0;          ///< Admissions the adapter rejected within budget (not faults)
        uint64_t faults = 0;            ///< Exceptions plus budget overruns
        uint64_t budgetOverruns = 0;    ///< Calls slower than GatewayAuthenticatorPolicy::callBudget
        uint64_t rejectedWhileOpen = 0; ///< Admissions failed fast without reaching the adapter
        uint32_t consecutiveFaults = 0; ///< Faults since the last in-budget answer
        uint64_t maxCallMicroseconds = 0;
        bool circuitOpen = false;
    };

    /** @brief Fault containment, call budget and circuit breaker around an IGatewayAuthenticator. */
    class GuardedGatewayAuthenticator final : public IGatewayAuthenticator
    {
      public:
        explicit GuardedGatewayAuthenticator(IGatewayAuthenticator& target) : m_target(&target) {}

        GuardedGatewayAuthenticator(const GuardedGatewayAuthenticator&) = delete;
        GuardedGatewayAuthenticator& operator=(const GuardedGatewayAuthenticator&) = delete;

        void SetPolicy(const GatewayAuthenticatorPolicy& policy)
        {
            std::lock_guard lock(m_mutex);
            m_policy = policy;
        }

        [[nodiscard]] GatewayAuthenticatorHealth GetHealth() const
        {
            std::lock_guard lock(m_mutex);
            return m_health;
        }

        /**
         * True while the circuit is open and the cooldown has not elapsed, i.e. every admission is
         * rejected without reaching the adapter. Once the cooldown elapses this is false again even
         * though circuitOpen stays set until a probe answers, so readiness lets the probe through.
         */
        [[nodiscard]] bool IsFailingFast() const
        {
            std::lock_guard lock(m_mutex);
            return m_health.circuitOpen && std::chrono::steady_clock::now() < m_retryAt;
        }

        [[nodiscard]] AuthenticationResult Authenticate(const AdmissionRequest& request) override
        {
            {
                std::lock_guard lock(m_mutex);
                if (m_health.circuitOpen && std::chrono::steady_clock::now() < m_retryAt)
                {
                    ++m_health.rejectedWhileOpen;
                    return {false, {}, "Authentication backend unavailable (circuit open)"};
                }
            }

            AuthenticationResult result;
            bool threw = false;
            const auto started = std::chrono::steady_clock::now();
            try
            {
                result = m_target->Authenticate(request);
            }
            catch (...)
            {
                threw = true;
            }
            const auto elapsed = std::chrono::steady_clock::now() - started;

            std::lock_guard lock(m_mutex);
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            m_health.maxCallMicroseconds = (std::max)(m_health.maxCallMicroseconds, static_cast<uint64_t>(micros));
            const bool overran = elapsed > m_policy.callBudget;
            if (overran)
            {
                ++m_health.budgetOverruns;
            }
            if (threw || overran)
            {
                RecordFault(threw ? "adapter threw" : "call exceeded its budget");
                return {false, {}, threw ? "Authentication backend fault" : "Authentication exceeded its time budget"};
            }

            if (m_health.circuitOpen)
            {
                SPARK_LOG_INFO(Spark::LogCategory::Network,
                               "GatewayAuthenticator: circuit closed after a successful probe");
            }
            m_health.consecutiveFaults = 0;
            m_health.circuitOpen = false;
            if (result.accepted)
            {
                ++m_health.accepted;
            }
            else
            {
                ++m_health.rejected;
            }
            RedactCredential(result.reason, request.credential);
            return result;
        }

        [[nodiscard]] bool IsReady() const override
        {
            try
            {
                return m_target->IsReady();
            }
            catch (...)
            {
                return false;
            }
        }

      private:
        // Caller holds m_mutex.
        void RecordFault(const char* kind)
        {
            ++m_health.faults;
            ++m_health.consecutiveFaults;
            if (m_health.consecutiveFaults < m_policy.failureThreshold)
            {
                return;
            }
            // Opening, or a failed probe after the cooldown: fail fast for another cooldown.
            m_health.circuitOpen = true;
            m_retryAt = std::chrono::steady_clock::now() + m_policy.cooldown;
            SPARK_LOG_WARN(Spark::LogCategory::Network,
                           "GatewayAuthenticator: circuit open after %u consecutive faults (last: %s)",
                           m_health.consecutiveFaults, kind);
        }

        static void RedactCredential(std::string& reason, const std::string& credential)
        {
            static constexpr std::string_view Redacted = "<redacted>";
            if (credential.empty())
            {
                return;
            }
            // Resume after the replacement so a credential that occurs inside it cannot loop forever.
            for (size_t pos = reason.find(credential); pos != std::string::npos;
                 pos = reason.find(credential, pos + Redacted.size()))
            {
                reason.replace(pos, credential.size(), Redacted);
            }
        }

        IGatewayAuthenticator* m_target = nullptr;
        GatewayAuthenticatorPolicy m_policy;
        GatewayAuthenticatorHealth m_health;
        std::chrono::steady_clock::time_point m_retryAt{};
        mutable std::mutex m_mutex;
    };
} // namespace Spark::Gateway
