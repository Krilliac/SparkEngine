/**
 * @file TFTravelSystemNet.cpp
 * @brief TFTravelSystem wire plumbing (own channel, TFVehicleNet precedent):
 *        NetworkManager handler registration/release for the 0x5434-0x5437
 *        travel channel plus the frozen TFMsg::ContinentHopReply id, packet
 *        size validation and the reliable server->client send helper. Split
 *        from TFTravelSystem.cpp (same class, split per the repo file-size
 *        rules — mirrors the TFVehicleSystem split).
 */
#include "World/TFTravelSystem.h"

#include "Net/TFNetProtocol.h" // W13 server-authoritative continent-hop: TF_ContinentHopRequest/Reply

#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <cstring>

#ifdef ENABLE_NETWORKING

namespace Terrafront
{

    bool TFTravelSystem::ServerNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server && m_ctx->IsAuthority();
    }

    bool TFTravelSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Client;
    }

    void TFTravelSystem::ServerEnsureNetHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFTravelMsg_Request), [this](const NetworkMessage& m)
                           { ServerHandleTravelPacket(m.senderID, m.payload.data(), m.payload.size()); });
        nm.RegisterHandler(static_cast<MessageType>(kTFTravelMsg_InfoRequest), [this](const NetworkMessage& m)
                           { ServerHandleInfoRequestPacket(m.senderID, m.payload.data(), m.payload.size()); });
        m_serverHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] travel server net handlers registered");
    }

    void TFTravelSystem::ServerReleaseNetHandlers()
    {
        // No per-type removal in NetworkManager; overwrite with no-ops so no
        // dangling `this` survives shutdown (module-wide pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (uint16_t id : {kTFTravelMsg_Request, kTFTravelMsg_InfoRequest})
            nm.RegisterHandler(static_cast<Spark::Net::MessageType>(id), [](const Spark::Net::NetworkMessage&) {});
        m_serverHandlers = false;
    }

    void TFTravelSystem::ClientEnsureNetHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFTravelMsg_Reply),
                           [this](const NetworkMessage& m) { OnNetTravelReply(m.payload.data(), m.payload.size()); });
        nm.RegisterHandler(static_cast<MessageType>(kTFTravelMsg_Info),
                           [this](const NetworkMessage& m) { OnNetContinentInfo(m.payload.data(), m.payload.size()); });
        // server-authoritative continent-hop follow-up (W13,
        // docs/TERRAFRONT_MULTIMAP.md §2.2): TFMsg::ContinentHopReply is a
        // frozen TFNetProtocol.h id (Net/TFServerSim.cpp answers it
        // server-side) but the client-side reaction — disconnect from this
        // server, connect to the resolved one — belongs to this system's
        // existing connect/disconnect sequence, so it registers its own
        // handler here (this system's own-channel precedent, same as the
        // travel-reply ids above) instead of routing through
        // TFClientNet/TFClientNetHandlers.cpp.
        nm.RegisterHandler(static_cast<MessageType>(static_cast<uint16_t>(TFMsg::ContinentHopReply)),
                           [this](const NetworkMessage& m)
                           { OnNetContinentHopReply(m.payload.data(), m.payload.size()); });
        m_clientHandlers = true;
    }

    void TFTravelSystem::ClientReleaseNetHandlers()
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (uint16_t id : {kTFTravelMsg_Reply, kTFTravelMsg_Info, static_cast<uint16_t>(TFMsg::ContinentHopReply)})
            nm.RegisterHandler(static_cast<Spark::Net::MessageType>(id), [](const Spark::Net::NetworkMessage&) {});
        m_clientHandlers = false;
    }

    void TFTravelSystem::ServerHandleTravelPacket(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_TravelRequest) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_TravelRequest req;
        std::memcpy(&req, data, sizeof(req));
        ServerHandleTravel(sender, req);
    }

    void TFTravelSystem::ServerHandleInfoRequestPacket(PlayerId sender, const void* data, size_t size)
    {
        (void)data;
        if (size != 0 || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_ContinentInfo info;
        ServerBuildContinentInfo(info);
        ServerSendTo(sender, kTFTravelMsg_Info, &info, sizeof(info));
    }

    void TFTravelSystem::ServerSendTo(PlayerId player, uint16_t msgId, const void* payload, size_t size)
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return;
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(msgId);
        msg.channel = Spark::Net::ChannelType::Reliable;
        msg.payload.resize(size);
        if (size > 0)
            std::memcpy(msg.payload.data(), payload, size);
        nm.SendToClient(player, msg);
    }

    void TFTravelSystem::OnNetTravelReply(const void* data, size_t size)
    {
        if (size != sizeof(TF_TravelReply))
        {
            ++m_badPackets;
            return;
        }
        TF_TravelReply rep;
        std::memcpy(&rep, data, sizeof(rep));
        ApplyTravelReply(rep);
    }

    void TFTravelSystem::OnNetContinentInfo(const void* data, size_t size)
    {
        if (size != sizeof(TF_ContinentInfo))
        {
            ++m_badPackets;
            return;
        }
        TF_ContinentInfo info;
        std::memcpy(&info, data, sizeof(info));
        ApplyContinentInfo(info);
    }

    void TFTravelSystem::OnNetContinentHopReply(const void* data, size_t size)
    {
        if (size != sizeof(TF_ContinentHopReply))
        {
            ++m_badPackets;
            return;
        }
        TF_ContinentHopReply rep{};
        std::memcpy(&rep, data, sizeof(rep));
        rep.host[sizeof(rep.host) - 1] = '\0'; // defense-in-depth: guarantee NUL even if the wire string wasn't

        if (m_hopRequestMapId != static_cast<int>(rep.mapId))
            return; // stale/unsolicited reply (already applied, or superseded by a newer request)

        // Deferred apply — see ApplyPendingContinentHop's doc comment: doing
        // the Disconnect()/Connect() here, synchronously inside
        // NetworkManager's message-dispatch loop, would tear down the very
        // socket that loop is iterating.
        m_hopReplyOk = rep.ok != 0;
        m_hopReplyHost = rep.host;
        m_hopReplyPort = rep.port;
        m_hopReplyPending = true;
    }

} // namespace Terrafront

#endif // ENABLE_NETWORKING
