/**
 * @file CollaborationService.h
 * @brief Headless collaborative-editing session broker hosted by SparkDaemon.
 */

#pragma once

#include "CollaborationProtocol.h"
#include "ServiceBase.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace Spark::Daemon
{
    struct CollaborationConfig
    {
        size_t maximumSessions = 32;
        size_t maximumPeersPerSession = 32;
        size_t maximumLocksPerSession = 4096;
        size_t maximumEditHistory = 8192;
        std::chrono::seconds peerTimeout{120};
    };

    /**
     * @brief Authoritative in-memory presence, lock, and edit-history broker.
     *
     * This is intentionally UI- and scene-model-independent. Editors translate
     * their existing CollaborativeEditSession concepts at the client boundary.
     * Every operation is serialized under one service mutex; returned snapshots
     * are value copies. Peer capabilities are opaque 256-bit tokens and node
     * locks are released when a peer leaves or expires.
     */
    class CollaborationService final : public ServiceBase
    {
      public:
        explicit CollaborationService(CollaborationConfig config = {});

        [[nodiscard]] ServiceId GetServiceId() const noexcept override { return ServiceId::Collab; }
        [[nodiscard]] const char* GetName() const noexcept override { return "collaboration"; }
        std::optional<ServiceResponse> HandleMessage(uint16_t messageType,
                                                     const std::vector<uint8_t>& payload) override;

        [[nodiscard]] size_t GetSessionCount() const;

      private:
        struct PeerRecord
        {
            CollaborationPeer peer;
            std::string token;
            std::chrono::steady_clock::time_point lastSeen;
        };

        struct SessionRecord
        {
            std::string id;
            std::string administrationToken;
            uint32_t nextPeerId = 1;
            uint64_t nextSequence = 1;
            std::unordered_map<uint32_t, PeerRecord> peers;
            std::unordered_map<std::string, uint32_t> locks;
            std::deque<CollaborationEdit> edits;
        };

        ServiceResponse Create(const std::vector<uint8_t>& payload);
        ServiceResponse Delete(const std::vector<uint8_t>& payload);
        ServiceResponse Join(const std::vector<uint8_t>& payload);
        ServiceResponse Leave(const std::vector<uint8_t>& payload);
        ServiceResponse Presence(const std::vector<uint8_t>& payload);
        ServiceResponse AcquireLock(const std::vector<uint8_t>& payload);
        ServiceResponse ReleaseLock(const std::vector<uint8_t>& payload);
        ServiceResponse SubmitEdit(const std::vector<uint8_t>& payload);
        ServiceResponse Snapshot(const std::vector<uint8_t>& payload);
        ServiceResponse MakeError(std::string message) const;
        ServiceResponse MakeAck(CollaborationMessage type) const;

        SessionRecord* AuthenticateLocked(const CollaborationAuth& auth);
        void RemovePeerLocked(SessionRecord& session, uint32_t peerId);
        void PruneExpiredLocked(SessionRecord& session, std::chrono::steady_clock::time_point now);
        static std::string GenerateToken();
        static bool SecureEquals(std::string_view lhs, std::string_view rhs) noexcept;
        static bool IsValidIdentifier(std::string_view id);

        CollaborationConfig m_config;
        mutable std::mutex m_mutex;
        std::unordered_map<std::string, SessionRecord> m_sessions;
    };
} // namespace Spark::Daemon
