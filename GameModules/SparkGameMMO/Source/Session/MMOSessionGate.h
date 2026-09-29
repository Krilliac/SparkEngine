/**
 * @file MMOSessionGate.h
 * @brief Authenticated MMO character admission and authoritative world commands.
 *
 * Thread affinity: Initialize/Update/Shutdown/Send/Get* run on the game thread. The
 * NetworkManager handlers may run on whichever thread calls NetworkManager::Update (the
 * DedicatedServer tick thread inside SparkServer), so they only copy the datagram into a
 * mutex-guarded, fixed-capacity inbox; every decode and all session/world mutation happen
 * when Update() drains that inbox on the game thread. The module owns this gate; the
 * borrowed network/account/character/player services must outlive Shutdown().
 * Session, client-view and inbox storage are bounded (64 sessions, 128 queued datagrams,
 * each at most MaxPacketBytes, allocated once in Initialize); datagrams arriving while the
 * inbox is full are dropped. Admission allocates strings and characters; movement uses
 * existing player storage. Encoded outbound messages allocate through NetworkManager.
 * Once-per-second disconnect maintenance obtains a bounded connection snapshot. This is a
 * small-area development slice, not a fleet.
 */
#pragma once

#ifdef ENABLE_NETWORKING
#include "MMOSessionGateProtocol.h"
#include "Engine/Networking/NetworkManager.h"

#include <array>
#include <memory>
#include <mutex>
#include <string>

namespace MMO
{
    class MMOAccountSystem;
    class MMOCharacterSystem;
    class MMOPlayerSystem;

    class MMOSessionGate
    {
      public:
        static constexpr size_t MaxSessions = 64;
        static constexpr size_t InboxCapacity = 128;
        static constexpr auto RequestType = static_cast<Spark::Net::MessageType>(1320);
        static constexpr auto ReplyType = static_cast<Spark::Net::MessageType>(1321);

        MMOSessionGate();
        ~MMOSessionGate();
        MMOSessionGate(const MMOSessionGate&) = delete;
        MMOSessionGate& operator=(const MMOSessionGate&) = delete;

        /// @brief Bind the real services and install directional, sensitive network handlers.
        bool Initialize(Spark::Net::NetworkManager& network, MMOAccountSystem& accounts, MMOCharacterSystem& characters,
                        MMOPlayerSystem& players);
        /// @brief Replenish server-time budgets and revoke disconnected/expired sessions.
        void Update(float deltaTime);
        /// @brief Remove handlers and authenticated players before the borrowed services die.
        void Shutdown();
        /// @brief Submit a client command. Zero requestId assigns the next monotonically increasing ID.
        bool Send(const SessionGateWire::Packet& packet);
        const SessionGateWire::Packet& GetLastReply() const { return m_lastReply; }
        const SessionGateWire::Packet* GetState(uint32_t characterId) const;

      private:
        struct AuthenticationState;
        /// One received datagram awaiting game-thread processing.
        struct Inbound
        {
            uint32_t senderId = 0;
            uint32_t length = 0;
            bool reply = false;
            std::array<uint8_t, SessionGateWire::MaxPacketBytes> bytes{};
        };
        using Inbox = std::array<Inbound, InboxCapacity>;
        struct Session
        {
            uint32_t clientId = 0;
            uint32_t accountId = 0;
            uint32_t characterId = 0;
            uint32_t lastRequest = 0;
            std::string token;
            float moveCredit = 0.0f;
            float interactCooldown = 0.0f;
        };

        Session* FindSession(uint32_t clientId);
        Session* AcquireSession(uint32_t clientId);
        void Revoke(Session& session);
        void Enqueue(const Spark::Net::NetworkMessage& message, bool reply);
        void DrainInbox();
        void ClearInbox();
        void ReceiveRequest(uint32_t senderId, std::span<const uint8_t> payload);
        void ReceiveReply(std::span<const uint8_t> payload);
        SessionGateWire::Status Authenticate(Session& session, const SessionGateWire::Packet& request);
        SessionGateWire::Status Apply(Session& session, const SessionGateWire::Packet& request);
        SessionGateWire::Packet Snapshot(const Session& session) const;
        void SendTo(uint32_t clientId, const SessionGateWire::Packet& packet);
        void Publish(const Session& session);

        Spark::Net::NetworkManager* m_network = nullptr;
        MMOAccountSystem* m_accounts = nullptr;
        MMOCharacterSystem* m_characters = nullptr;
        MMOPlayerSystem* m_players = nullptr;
        std::unique_ptr<AuthenticationState> m_authentication;
        std::mutex m_inboxMutex;
        std::unique_ptr<Inbox> m_inbox;      ///< Filled by network handlers under m_inboxMutex.
        std::unique_ptr<Inbox> m_processing; ///< Swapped out of m_inbox and drained on the game thread.
        size_t m_inboxCount = 0;
        std::array<Session, MaxSessions> m_sessions{};
        std::array<SessionGateWire::Packet, MaxSessions> m_states{};
        SessionGateWire::Packet m_lastReply{};
        uint32_t m_nextRequest = 0;
        float m_authCooldown = 0.0f;
        float m_connectionCheck = 0.0f;
    };
} // namespace MMO
#endif
