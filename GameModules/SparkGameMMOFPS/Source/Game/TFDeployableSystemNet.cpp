/**
 * @file TFDeployableSystemNet.cpp
 * @brief TFDeployableSystem 0x54FC-0x54FE replication: server broadcast
 *        (reliable Create/Destroy, health/life Update, late-joiner
 *        GetClients() diff poll) and the client mirror handlers. Split from
 *        TFDeployableSystem.cpp; the whole translation unit compiles away
 *        without ENABLE_NETWORKING (patterns mirror TFReplication.cpp).
 */
#include "Game/TFDeployableSystem.h"

#ifdef ENABLE_NETWORKING

#include "Game/TFDeployableTypes.h"
#include "Net/TFRepProtocol.h"

#include "Engine/Networking/NetworkManager.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Replication — server broadcast (patterns mirror TFReplication.cpp)
    // ---------------------------------------------------------------------------

    bool TFDeployableSystem::ServerNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server && m_ctx->IsAuthority();
    }

    bool TFDeployableSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Client;
    }

    void TFDeployableSystem::ServerPollNewClients()
    {
        // No join callback slot on NetworkManager — diff GetClients() like
        // TFServerSim/TFReplication so late joiners get the current set.
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        const auto& clients = nm.GetClients();

        for (const auto& [id, info] : clients)
        {
            if (info.state != Spark::Net::ConnectionState::Connected || m_knownClients.contains(id))
                continue;
            m_knownClients.insert(id);
            for (const auto& [entity, rec] : m_deployables)
                SendCreate(id, rec.view);
        }
        for (auto it = m_knownClients.begin(); it != m_knownClients.end();)
        {
            if (!clients.contains(*it))
                it = m_knownClients.erase(it);
            else
                ++it;
        }
    }

    void TFDeployableSystem::SendCreate(PlayerId target, const TFDeployableView& view)
    {
        TF_RepDeployCreate c{};
        c.entityId = view.entity;
        c.ownerPlayer = view.owner;
        c.kind = static_cast<uint8_t>(view.kind);
        c.faction = static_cast<uint8_t>(view.faction);
        c.posX = view.pos[0];
        c.posY = view.pos[1];
        c.posZ = view.pos[2];
        c.yaw = view.yaw;
        c.health = view.health;
        c.maxHealth = view.maxHealth;
        c.lifeSec = view.life;
        SendRep(target, kTFRepMsg_DeployCreate, &c, sizeof(c), true);
    }

    void TFDeployableSystem::SendUpdate(const TFDeployableView& view)
    {
        TF_RepDeployUpdate u{};
        u.entityId = view.entity;
        u.health = view.health;
        u.lifeSec = view.life;
        SendRep(kInvalidPlayer, kTFRepMsg_DeployUpdate, &u, sizeof(u), false);
    }

    void TFDeployableSystem::SendDestroy(EntityId entity)
    {
        TF_RepDeployDestroy d{entity};
        SendRep(kInvalidPlayer, kTFRepMsg_DeployDestroy, &d, sizeof(d), true);
    }

    void TFDeployableSystem::SendRep(PlayerId target, uint16_t msgId, const void* payload, size_t size, bool reliable)
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(msgId);
        msg.channel = reliable ? Spark::Net::ChannelType::Reliable : Spark::Net::ChannelType::Unreliable;
        msg.payload.resize(size);
        std::memcpy(msg.payload.data(), payload, size);
        if (target == kInvalidPlayer)
            nm.SendToAll(msg);
        else
            nm.SendToClient(target, msg);
    }

    // ---------------------------------------------------------------------------
    // Replication — client mirror
    // ---------------------------------------------------------------------------

    void TFDeployableSystem::ClientEnsureHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();

        nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_DeployCreate),
                           [this](const NetworkMessage& m) { OnDeployCreate(m.payload.data(), m.payload.size()); });
        nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_DeployUpdate),
                           [this](const NetworkMessage& m) { OnDeployUpdate(m.payload.data(), m.payload.size()); });
        nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_DeployDestroy),
                           [this](const NetworkMessage& m) { OnDeployDestroy(m.payload.data(), m.payload.size()); });
        m_clientHandlers = true;
    }

    void TFDeployableSystem::ClientReleaseHandlers()
    {
        // No per-type removal on NetworkManager — swap in no-ops so no dangling
        // `this` survives shutdown (established TFReplication pattern).
        using Spark::Net::MessageType;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (uint16_t id : {kTFRepMsg_DeployCreate, kTFRepMsg_DeployUpdate, kTFRepMsg_DeployDestroy})
            nm.RegisterHandler(static_cast<MessageType>(id), [](const Spark::Net::NetworkMessage&) {});
        m_clientHandlers = false;
    }

    void TFDeployableSystem::OnDeployCreate(const void* data, size_t size)
    {
        if (size != sizeof(TF_RepDeployCreate))
            return;
        TF_RepDeployCreate c;
        std::memcpy(&c, data, sizeof(c));
        if (c.kind >= kTFDeployKindCount)
            return; // stale/newer server sent a kind this build doesn't know

        Rec& rec = m_deployables[c.entityId]; // upsert (re-create keeps the visual)
        rec.view.entity = c.entityId;
        rec.view.kind = static_cast<DeployableKind>(c.kind);
        rec.view.owner = c.ownerPlayer;
        rec.view.faction = static_cast<FactionId>(c.faction);
        rec.view.pos[0] = c.posX;
        rec.view.pos[1] = c.posY;
        rec.view.pos[2] = c.posZ;
        rec.view.yaw = c.yaw;
        rec.view.health = c.health;
        rec.view.maxHealth = c.maxHealth;
        rec.view.life = c.lifeSec;
        if (rec.local == 0)
            rec.local = CreateDeployableEntity(rec.view);
    }

    void TFDeployableSystem::OnDeployUpdate(const void* data, size_t size)
    {
        if (size != sizeof(TF_RepDeployUpdate))
            return;
        TF_RepDeployUpdate u;
        std::memcpy(&u, data, sizeof(u));
        auto it = m_deployables.find(u.entityId);
        if (it == m_deployables.end())
            return; // unreliable update raced the reliable Create — skip
        it->second.view.health = u.health;
        it->second.view.life = u.lifeSec;
    }

    void TFDeployableSystem::OnDeployDestroy(const void* data, size_t size)
    {
        if (size != sizeof(TF_RepDeployDestroy))
            return;
        TF_RepDeployDestroy d;
        std::memcpy(&d, data, sizeof(d));
        auto it = m_deployables.find(d.entityId);
        if (it == m_deployables.end())
            return;
        DestroyLocalEntity(it->second.local);
        m_deployables.erase(it);
    }

} // namespace Terrafront

#endif // ENABLE_NETWORKING
