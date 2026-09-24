/** @file GatewayAreaControl.h @brief Authenticated local area-control transport. */
#pragma once

#include "GatewaySecurity.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace Spark::Gateway
{
    class GatewayCoordinator;
    enum class AreaControlPhase : uint8_t
    {
        Prepare = 1,
        Transfer = 2,
        Commit = 3,
        Acknowledge = 4,
        Abort = 5,
        Probe = 6
    };

    /**
     * @brief Bounded classification of every frame the area-control service receives.
     *
     * Each connection produces exactly one audit record (one log line plus one
     * counter increment). Only @c Accepted, @c Replay and @c LedgerFull are
     * reached after the MAC verified; every other reason describes an
     * unauthenticated frame whose claimed fields must not be trusted.
     */
    enum class AreaControlAuditReason : uint8_t
    {
        Accepted = 0,    ///< Authenticated and fresh; the phase was handed to the epoch fence
        Incomplete,      ///< Connection closed or timed out before a whole frame arrived
        PeerMismatch,    ///< Peer is not the same operating-system user as the service
        Oversize,        ///< Frame payload exceeds GatewayMaximumBodySize
        WrongService,    ///< Frame addressed to a service other than Orchestration
        DecodeFailed,    ///< Payload is not a well-formed area-control request
        PhaseMismatch,   ///< Signed phase differs from the frame message type
        TimestampWindow, ///< Request timestamp outside the +/-60 s freshness window
        MacInvalid,      ///< HMAC-SHA256 did not verify under the service key
        Replay,          ///< Nonce already seen inside the freshness window
        LedgerFull,      ///< Replay ledger is at capacity; new nonces fail closed
        Count
    };

    /** @brief Stable lower_snake_case name of @p reason as written to the audit log. */
    [[nodiscard]] std::string_view AreaControlAuditReasonName(AreaControlAuditReason reason);

    /** Gateway-side one-request-per-connection local named-pipe adapter. */
    class LocalAreaControlPlane final : public IAreaControlPlane
    {
      public:
        explicit LocalAreaControlPlane(const std::filesystem::path& keyFile);
        explicit LocalAreaControlPlane(std::vector<uint8_t> key);
        ~LocalAreaControlPlane() override;

        [[nodiscard]] bool IsReady() const override;
        [[nodiscard]] bool IsEndpointReady(Net::AreaID id) const override;
        [[nodiscard]] HandoffOperationResult Prepare(const HandoffCommand& command) override;
        [[nodiscard]] HandoffOperationResult Transfer(const HandoffCommand& command) override;
        [[nodiscard]] HandoffOperationResult Commit(const HandoffCommand& command) override;
        [[nodiscard]] HandoffOperationResult Acknowledge(const HandoffCommand& command) override;
        [[nodiscard]] HandoffOperationResult Abort(const HandoffCommand& command) override;
        void RegisterEndpoint(Net::AreaID id, const AreaEndpoint& endpoint) override;

      private:
        [[nodiscard]] HandoffOperationResult Send(AreaControlPhase phase, const HandoffCommand& command);
        [[nodiscard]] HandoffOperationResult SendToEndpoint(std::string_view endpoint, AreaControlPhase phase,
                                                            const HandoffCommand& command) const;

        std::vector<uint8_t> m_key;
        std::string m_error;
        std::unordered_map<Net::AreaID, std::string> m_endpoints;
        mutable std::mutex m_mutex;
        mutable std::atomic<uint64_t> m_nonce{0};
    };

    /** SparkServer-side authority for handoff phase fencing and idempotency. */
    class LocalAreaControlService
    {
      public:
        LocalAreaControlService(std::string endpoint, std::filesystem::path keyFile,
                                std::filesystem::path epochStateFile);
        LocalAreaControlService(std::string endpoint, std::vector<uint8_t> key, std::filesystem::path epochStateFile);
        ~LocalAreaControlService();
        [[nodiscard]] bool Start();
        void Stop();
        [[nodiscard]] bool IsReady() const { return m_ready.load(std::memory_order_acquire); }
        [[nodiscard]] std::string GetLastError() const;
        /** @brief Number of frames classified as @p reason since construction. */
        [[nodiscard]] uint64_t GetAuditCount(AreaControlAuditReason reason) const;

      private:
        struct SessionFence
        {
            uint64_t epoch = 0;
            AreaControlPhase phase = AreaControlPhase::Abort;
        };
        void Run();
        void SetError(std::string error);
        /** Authenticates one received frame, applies it when valid, and writes its audit record. */
        [[nodiscard]] HandoffOperationResult HandleFrame(uint16_t serviceId, uint16_t messageType,
                                                         const std::vector<uint8_t>& payload);
        void RecordAudit(AreaControlAuditReason reason, unsigned int phase, uint64_t epoch, std::string_view sessionId,
                         HandoffOperationResult outcome);
        [[nodiscard]] HandoffOperationResult Apply(std::string_view sessionId, uint64_t epoch, AreaControlPhase phase);
        [[nodiscard]] bool LoadState();
        [[nodiscard]] bool SaveState() const;

        std::string m_endpoint;
        std::filesystem::path m_keyFile;
        std::filesystem::path m_epochStateFile;
        std::vector<uint8_t> m_key;
        std::string m_error;
        mutable std::mutex m_errorMutex;
        std::unordered_map<std::string, SessionFence> m_sessions;
        std::unordered_map<uint64_t, int64_t> m_seenNonces;
        mutable std::mutex m_mutex;
        std::array<std::atomic<uint64_t>, static_cast<size_t>(AreaControlAuditReason::Count)> m_auditCounts{};
        std::thread m_thread;
        std::atomic<bool> m_stop{false};
        std::atomic<bool> m_ready{false};
        std::atomic<bool> m_startupComplete{false};
    };

    class LocalGatewayIngressClient
    {
      public:
        explicit LocalGatewayIngressClient(std::string endpoint) : m_endpoint(std::move(endpoint)) {}
        [[nodiscard]] RouteResult Admit(const AdmissionRequest& request) const;

      private:
        std::string m_endpoint;
    };

    /** Owner-local named-pipe ingress; authentication remains in GatewayCoordinator. */
    class LocalGatewayIngressService
    {
      public:
        LocalGatewayIngressService(std::string endpoint, GatewayCoordinator& coordinator);
        ~LocalGatewayIngressService();
        [[nodiscard]] bool Start();
        void Stop();
        [[nodiscard]] bool IsReady() const { return m_ready.load(std::memory_order_acquire); }

      private:
        void Run();
        std::string m_endpoint;
        GatewayCoordinator* m_coordinator = nullptr;
        std::thread m_thread;
        std::atomic<bool> m_stop{false};
        std::atomic<bool> m_ready{false};
    };
} // namespace Spark::Gateway
