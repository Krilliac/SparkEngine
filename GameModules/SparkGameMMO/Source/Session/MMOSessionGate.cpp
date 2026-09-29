/** @file MMOSessionGate.cpp
 * @brief Connection-bound account admission over the engine's secure transport.
 */
#include "MMOSessionGate.h"

#ifdef ENABLE_NETWORKING
#include "Account/MMOAccountSystem.h"
#include "Character/MMOCharacterSystem.h"
#include "Player/MMOPlayerSystem.h"
#include "Engine/Networking/GatewayAuthenticator.h"
#include "Utils/SecureMemory.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace MMO
{
    using namespace SessionGateWire;

    struct MMOSessionGate::AuthenticationState final : Spark::Gateway::IGatewayAuthenticator
    {
        explicit AuthenticationState(MMOAccountSystem& accounts) : m_accounts(accounts), m_guard(*this) {}
        Spark::Gateway::AuthenticationResult Authenticate(const Spark::Gateway::AdmissionRequest& request) override
        {
            m_result = m_accounts.Login(request.playerName, request.credential);
            return {m_result.success, m_result.success ? std::to_string(m_result.accountId) : std::string{},
                    m_result.success ? "" : "Invalid username or password"};
        }
        bool IsReady() const override { return true; }
        MMOAccountSystem& m_accounts;
        Spark::Gateway::GuardedGatewayAuthenticator m_guard;
        AuthResult m_result;
    };

    MMOSessionGate::MMOSessionGate() = default;
    MMOSessionGate::~MMOSessionGate()
    {
        Shutdown();
    }

    bool MMOSessionGate::Initialize(Spark::Net::NetworkManager& network, MMOAccountSystem& accounts,
                                    MMOCharacterSystem& characters, MMOPlayerSystem& players)
    {
        Shutdown();
        m_network = &network;
        m_accounts = &accounts;
        m_characters = &characters;
        m_players = &players;
        m_authentication = std::make_unique<AuthenticationState>(accounts);
        m_inbox = std::make_unique<Inbox>();
        m_processing = std::make_unique<Inbox>();
        auto& validator = network.GetPacketValidator();
        validator.RegisterSchema(RequestType, {.minPayloadSize = 1,
                                               .maxPayloadSize = MaxPacketBytes,
                                               .requiresAuth = true,
                                               .allowedFromClient = true,
                                               .allowedFromServer = false});
        validator.RegisterSchema(ReplyType, {.minPayloadSize = 1,
                                             .maxPayloadSize = MaxPacketBytes,
                                             .requiresAuth = true,
                                             .allowedFromClient = false,
                                             .allowedFromServer = true});
        network.RegisterSensitiveHandler(RequestType, [this](const Spark::Net::NetworkMessage& message)
                                         { Enqueue(message, false); });
        network.RegisterHandler(ReplyType,
                                [this](const Spark::Net::NetworkMessage& message) { Enqueue(message, true); });
        return true;
    }

    void MMOSessionGate::Enqueue(const Spark::Net::NetworkMessage& message, bool reply)
    {
        // Any thread: copy only. The schema already bounds the payload; recheck before copying.
        if (message.payload.empty() || message.payload.size() > MaxPacketBytes)
        {
            return;
        }
        std::lock_guard lock(m_inboxMutex);
        if (!m_inbox || m_inboxCount >= InboxCapacity)
        {
            return;
        }
        Inbound& slot = (*m_inbox)[m_inboxCount++];
        slot.senderId = message.senderID;
        slot.length = static_cast<uint32_t>(message.payload.size());
        slot.reply = reply;
        std::copy(message.payload.begin(), message.payload.end(), slot.bytes.begin());
    }

    void MMOSessionGate::DrainInbox()
    {
        size_t count = 0;
        {
            std::lock_guard lock(m_inboxMutex);
            if (!m_inbox || !m_processing)
            {
                return;
            }
            std::swap(m_inbox, m_processing);
            count = m_inboxCount;
            m_inboxCount = 0;
        }
        for (size_t index = 0; index < count && m_network; ++index)
        {
            Inbound& item = (*m_processing)[index];
            const std::span<const uint8_t> payload(item.bytes.data(), item.length);
            if (item.reply)
            {
                ReceiveReply(payload);
            }
            else
            {
                ReceiveRequest(item.senderId, payload);
            }
            // Requests may carry credentials; never leave them in a reusable slot.
            Spark::SecureErase(item.bytes.data(), item.length);
            item.length = 0;
        }
    }

    void MMOSessionGate::ClearInbox()
    {
        std::lock_guard lock(m_inboxMutex);
        for (auto* inbox : {m_inbox.get(), m_processing.get()})
        {
            if (inbox)
            {
                for (auto& item : *inbox)
                {
                    Spark::SecureErase(item.bytes.data(), item.bytes.size());
                    item.length = 0;
                }
            }
        }
        m_inboxCount = 0;
    }

    MMOSessionGate::Session* MMOSessionGate::FindSession(uint32_t clientId)
    {
        for (auto& session : m_sessions)
        {
            if (clientId != 0 && session.clientId == clientId)
            {
                return &session;
            }
        }
        return nullptr;
    }

    MMOSessionGate::Session* MMOSessionGate::AcquireSession(uint32_t clientId)
    {
        if (auto* session = FindSession(clientId))
        {
            return session;
        }
        for (auto& session : m_sessions)
        {
            if (session.clientId == 0)
            {
                session.clientId = clientId;
                return &session;
            }
        }
        return nullptr;
    }

    void MMOSessionGate::Revoke(Session& session)
    {
        if (session.characterId != 0)
        {
            auto state = Snapshot(session);
            state.status = Status::Rejected;
            for (const auto& observer : m_sessions)
            {
                if (observer.characterId != 0 && observer.clientId != session.clientId)
                {
                    SendTo(observer.clientId, state);
                }
            }
            m_players->RemovePlayer(session.clientId);
        }
        if (!session.token.empty())
        {
            m_accounts->Logout(session.token);
            Spark::SecureClear(session.token);
        }
        session = {};
    }

    void MMOSessionGate::Shutdown()
    {
        if (m_network)
        {
            m_network->UnregisterHandler(RequestType);
            m_network->UnregisterHandler(ReplyType);
            ClearInbox();
            for (auto& session : m_sessions)
            {
                Revoke(session);
            }
            for (const auto& state : m_states)
            {
                if (state.characterId != 0)
                {
                    m_players->RemovePlayer(state.requestId);
                }
            }
        }
        m_authentication.reset();
        m_network = nullptr;
        m_accounts = nullptr;
        m_characters = nullptr;
        m_players = nullptr;
        m_states = {};
        m_lastReply = {};
        m_nextRequest = 0;
        m_authCooldown = 0.0f;
        m_connectionCheck = 0.0f;
    }

    void MMOSessionGate::Update(float deltaTime)
    {
        if (!m_network)
        {
            return;
        }
        DrainInbox();
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
        {
            return;
        }
        if (m_network->GetRole() != Spark::Net::NetworkRole::Server)
        {
            if (m_network->GetConnectionState() != Spark::Net::ConnectionState::Connected)
            {
                for (const auto& state : m_states)
                {
                    if (state.characterId != 0)
                    {
                        m_players->RemovePlayer(state.requestId);
                    }
                }
                m_states = {};
            }
            return;
        }
        const float elapsed = (std::min)(deltaTime, 0.1f);
        m_authCooldown = (std::max)(0.0f, m_authCooldown - elapsed);
        for (auto& session : m_sessions)
        {
            session.moveCredit = (std::min)(0.1f, session.moveCredit + elapsed);
            session.interactCooldown = (std::max)(0.0f, session.interactCooldown - elapsed);
        }
        m_connectionCheck += elapsed;
        if (m_connectionCheck < 1.0f)
        {
            return;
        }
        m_connectionCheck = 0.0f;
        const auto clients = m_network->GetClients();
        for (auto& session : m_sessions)
        {
            if (session.clientId != 0 && (!clients.contains(session.clientId) ||
                                          (session.accountId != 0 && !m_accounts->ValidateSession(session.token))))
            {
                Revoke(session);
            }
        }
    }

    Status MMOSessionGate::Authenticate(Session& session, const Packet& request)
    {
        if (session.accountId != 0)
        {
            return Status::Rejected;
        }
        if (m_authCooldown > 0.0f)
        {
            return Status::RateLimited;
        }
        // A global admission budget bounds KDF work even when one peer changes usernames.
        m_authCooldown = 0.25f;
        if (request.operation == Operation::Register)
        {
            if (m_accounts->GetAccountCount() >= 1024)
            {
                return Status::Rejected;
            }
            return m_accounts->Register(request.username.data(), request.password.data()).success ? Status::Ok
                                                                                                  : Status::Rejected;
        }
        Spark::Gateway::AdmissionRequest admission;
        admission.clientId = session.clientId;
        admission.playerName = request.username.data();
        admission.credential = request.password.data();
        m_authentication->m_result = {};
        const auto result = m_authentication->m_guard.Authenticate(admission);
        Spark::SecureClear(admission.credential);
        auto& login = m_authentication->m_result;
        if (!result.accepted)
        {
            if (login.success)
            {
                m_accounts->Logout(login.sessionToken);
            }
            Spark::SecureClear(login.sessionToken);
            return Status::Rejected;
        }
        // Login replaces the account's old token. Revoke its former world connection immediately.
        for (auto& previous : m_sessions)
        {
            if (&previous != &session && previous.accountId == login.accountId)
            {
                Revoke(previous);
            }
        }
        session.accountId = login.accountId;
        session.token = std::move(login.sessionToken);
        return Status::Ok;
    }

    void MMOSessionGate::ReceiveRequest(uint32_t senderId, std::span<const uint8_t> payload)
    {
        if (!m_network || m_network->GetRole() != Spark::Net::NetworkRole::Server || senderId == 0)
        {
            return;
        }
        Packet request;
        const bool decoded = Decode(payload, request);
        if (!decoded || request.response || request.operation == Operation::State)
        {
            Spark::SecureErase(request.password.data(), request.password.size());
            return;
        }
        auto* session = AcquireSession(senderId);
        if (!session)
        {
            Spark::SecureErase(request.password.data(), request.password.size());
            return;
        }
        Status status = Status::Invalid;
        if (request.requestId > session->lastRequest)
        {
            session->lastRequest = request.requestId;
            status = Apply(*session, request);
        }
        Spark::SecureErase(request.password.data(), request.password.size());
        auto reply = Snapshot(*session);
        reply.operation = request.operation;
        reply.requestId = request.requestId;
        reply.status = status;
        SendTo(senderId, reply);
    }

    void MMOSessionGate::SendTo(uint32_t clientId, const Packet& packet)
    {
        Spark::Net::NetworkMessage message;
        message.type = ReplyType;
        message.channel = Spark::Net::ChannelType::Reliable;
        message.payload = Encode(packet);
        m_network->SendToClient(clientId, message);
    }

    bool MMOSessionGate::Send(const Packet& packet)
    {
        if (!m_network || m_network->GetRole() != Spark::Net::NetworkRole::Client || packet.response ||
            m_network->GetConnectionState() != Spark::Net::ConnectionState::Connected ||
            m_nextRequest == std::numeric_limits<uint32_t>::max())
        {
            return false;
        }
        Packet request = packet;
        request.requestId = packet.requestId == 0 ? m_nextRequest + 1 : packet.requestId;
        m_nextRequest = (std::max)(m_nextRequest, request.requestId);
        Spark::Net::NetworkMessage message;
        message.type = RequestType;
        message.channel = Spark::Net::ChannelType::Reliable;
        message.sensitive = true;
        message.payload = Encode(request);
        Spark::SecureErase(request.password.data(), request.password.size());
        if (message.payload.empty())
        {
            return false;
        }
        m_network->SendMessage(message);
        return true;
    }

    const Packet* MMOSessionGate::GetState(uint32_t characterId) const
    {
        for (const auto& state : m_states)
        {
            if (characterId != 0 && state.characterId == characterId && state.status == Status::Ok)
            {
                return &state;
            }
        }
        return nullptr;
    }

    void MMOSessionGate::ReceiveReply(std::span<const uint8_t> payload)
    {
        Packet packet;
        if (!m_network || m_network->GetRole() != Spark::Net::NetworkRole::Client || !Decode(payload, packet) ||
            !packet.response)
        {
            return;
        }
        if (packet.operation != Operation::State)
        {
            m_lastReply = packet;
            return;
        }
        if (packet.characterId == 0 || packet.requestId == 0)
        {
            return;
        }
        // Reliable delivery does not preserve order, so a late snapshot must never
        // overwrite a newer one (or resurrect a revoked character).
        Packet* slot = nullptr;
        Packet* freeSlot = nullptr;
        for (auto& state : m_states)
        {
            if (state.characterId == packet.characterId)
            {
                slot = &state;
                break;
            }
            if (!freeSlot && (state.characterId == 0 || state.status != Status::Ok))
            {
                freeSlot = &state;
            }
        }
        if (slot && packet.stateSequence <= slot->stateSequence)
        {
            return;
        }
        if (!slot)
        {
            slot = packet.status == Status::Ok ? freeSlot : nullptr;
        }
        if (slot)
        {
            *slot = packet;
        }
        if (packet.status != Status::Ok)
        {
            m_players->RemovePlayer(packet.requestId);
            return;
        }
        if (!slot)
        {
            return;
        }
        MMOPlayer player;
        player.clientId = packet.requestId;
        player.characterId = packet.characterId;
        player.currentAreaId = packet.areaId;
        player.posX = player.targetPosX = packet.x;
        player.posY = player.targetPosY = packet.y;
        player.posZ = player.targetPosZ = packet.z;
        player.health = player.maxHealth = packet.health;
        player.lastInteractionTarget = packet.targetId;
        player.interactionCount = packet.interactionCount;
        (void)m_players->ApplySessionState(packet.requestId, player);
    }
} // namespace MMO
#endif
