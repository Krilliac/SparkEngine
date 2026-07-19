/**
 * @file TFSquadSystemClient.cpp
 * @brief TFSquadSystem client half: the local squad mirror (TF_SquadMsg state
 *        echoes + W11 TF_SquadWaypoint), the map/HUD waypoint UI entries, the
 *        wire echo/send helpers (with listen-host loopback) and the
 *        self-registering net handler lifecycle. Split from TFSquadSystem.cpp;
 *        the shared roster helpers live in TFSquadSystemInternal.h.
 */
#include "Game/TFSquadSystem.h"

#include "Game/TFSquadSystemInternal.h"
#include "Net/TFClientNet.h"
#include "Net/TFServerSim.h"
#include "World/TFWorldSetup.h" // W11 squad-v2: TerrainHeightAt for map waypoints
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <cstddef>
#include <cstring>

namespace Terrafront
{

    using namespace SquadDetail;

    void TFSquadSystem::UiMapWaypointClick(float x, float z, RegionId region)
    {
        if (!m_ctx || m_mirror.squad == kInvalidSquad)
            return;

        float wp[3] = {x, 0.0f, z};
        if (m_ctx->world)
            wp[1] = m_ctx->world->TerrainHeightAt(x, z);

        if (IsLocalSquadLeader())
        {
            // Ctrl-clicking the hex that already holds the waypoint clears it;
            // any other hex replaces it.
            if (m_mirror.hasWaypoint && region != kInvalidRegion && m_mirror.wpRegion == region)
                SendWaypointOp(SquadWpOp::Clear, nullptr, kInvalidRegion);
            else
                SendWaypointOp(SquadWpOp::Set, wp, region);
        }
        else
        {
            SendWaypointOp(SquadWpOp::Request, wp, region);
        }
    }

    void TFSquadSystem::UiClearWaypoint()
    {
        if (IsLocalSquadLeader() && m_mirror.hasWaypoint)
            SendWaypointOp(SquadWpOp::Clear, nullptr, kInvalidRegion);
    }

    void TFSquadSystem::PromoteWaypointRequest(size_t index)
    {
        if (!IsLocalSquadLeader() || index >= m_mirror.wpRequests.size())
            return;
        const WaypointRequest req = m_mirror.wpRequests[index]; // copy before erase
        m_mirror.wpRequests.erase(m_mirror.wpRequests.begin() + static_cast<ptrdiff_t>(index));
        SendWaypointOp(SquadWpOp::Set, req.wp, req.region);
    }

    void TFSquadSystem::DismissWaypointRequest(size_t index)
    {
        if (index < m_mirror.wpRequests.size())
            m_mirror.wpRequests.erase(m_mirror.wpRequests.begin() + static_cast<ptrdiff_t>(index));
    }

    // ---------------------------------------------------------------------------
    // Wire helpers
    // ---------------------------------------------------------------------------

    TF_SquadMsg TFSquadSystem::MakeEcho(SquadOp op, SquadId squad, PlayerId target)
    {
        TF_SquadMsg m{};
        m.op = static_cast<uint8_t>(op);
        m.squadId = squad;
        m.targetPlayer = target;
        return m;
    }

    void TFSquadSystem::SendEchoTo(PlayerId target, const TF_SquadMsg& echo)
    {
        if (target == kInvalidPlayer)
            return;
        // Listen host / standalone: the local player is not a network client —
        // feed the mirror directly (server + mirror live in this same instance).
        if (m_ctx->HasLocalPlayer() && target == m_ctx->localPlayer && m_ctx->role != NetRole::Client)
        {
            ClientHandleEcho(echo);
            return;
        }
#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized())
            return;
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(static_cast<uint16_t>(TFMsg::SquadMsg));
        msg.channel = Spark::Net::ChannelType::Reliable;
        msg.payload.resize(sizeof(echo));
        std::memcpy(msg.payload.data(), &echo, sizeof(echo));
        nm.SendToClient(target, msg);
#endif
    }

    void TFSquadSystem::EchoToMembers(const Squad& squad, const TF_SquadMsg& echo)
    {
        for (const PlayerId member : squad.members)
            SendEchoTo(member, echo);
    }

    // W11 squad-v2: TF_SquadWaypoint twins of SendEchoTo/EchoToMembers (same
    // local-loopback rule, same reliable channel, block id 0x546C).
    void TFSquadSystem::SendWaypointEchoTo(PlayerId target, const TF_SquadWaypoint& echo)
    {
        if (target == kInvalidPlayer)
            return;
        if (m_ctx->HasLocalPlayer() && target == m_ctx->localPlayer && m_ctx->role != NetRole::Client)
        {
            ClientHandleWaypoint(echo);
            return;
        }
#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized())
            return;
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(kTFMsgSquadWaypoint);
        msg.channel = Spark::Net::ChannelType::Reliable;
        msg.payload.resize(sizeof(echo));
        std::memcpy(msg.payload.data(), &echo, sizeof(echo));
        nm.SendToClient(target, msg);
#endif
    }

    void TFSquadSystem::EchoWaypointToMembers(const Squad& squad, const TF_SquadWaypoint& echo)
    {
        for (const PlayerId member : squad.members)
            SendWaypointEchoTo(member, echo);
    }

    // ---------------------------------------------------------------------------
    // Client mirror
    // ---------------------------------------------------------------------------

    void TFSquadSystem::ClientHandleEcho(const TF_SquadMsg& msg)
    {
        const PlayerId me = m_ctx ? m_ctx->localPlayer : kInvalidPlayer;

        switch (static_cast<SquadOp>(msg.op))
        {
        case SquadOp::Create:
            m_mirror = Mirror{};
            m_mirror.squad = msg.squadId;
            m_mirror.leader = msg.targetPlayer;
            m_mirror.members = {msg.targetPlayer};
            break;

        case SquadOp::Invite:
            m_mirror.inviteSquad = msg.squadId;
            m_mirror.inviteFrom = msg.targetPlayer;
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] squad invite from player %u (squad %u)", msg.targetPlayer,
                           msg.squadId);
            break;

        case SquadOp::Accept:
            if (m_mirror.squad == kInvalidSquad)
            {
                m_mirror.squad = msg.squadId; // our own join confirmation
                m_mirror.inviteSquad = kInvalidSquad;
                m_mirror.inviteFrom = kInvalidPlayer;
            }
            if (msg.squadId == m_mirror.squad)
                AddUnique(m_mirror.members, msg.targetPlayer);
            break;

        case SquadOp::Leave:
        case SquadOp::Kick:
            if (msg.squadId != m_mirror.squad)
                break;
            if (msg.targetPlayer == me)
            {
                ResetMirror(); // we left / were kicked
            }
            else
            {
                Remove(m_mirror.members, msg.targetPlayer);
                if (m_mirror.leader == msg.targetPlayer)
                    m_mirror.leader = kInvalidPlayer; // Promote echo follows
            }
            break;

        case SquadOp::Promote:
            if (msg.squadId == m_mirror.squad)
            {
                m_mirror.leader = msg.targetPlayer;
                AddUnique(m_mirror.members, msg.targetPlayer);
            }
            break;

        case SquadOp::SetWaypoint:
            if (msg.squadId == m_mirror.squad)
            {
                m_mirror.hasWaypoint = true;
                m_mirror.wp[0] = msg.wpX;
                m_mirror.wp[1] = msg.wpY;
                m_mirror.wp[2] = msg.wpZ;
            }
            break;

        default:
            break;
        }
    }

    // W11 squad-v2: mirror side of TF_SquadWaypoint. Set/Clear reach every
    // member; Request reaches the leader only (server forwards it nowhere else).
    void TFSquadSystem::ClientHandleWaypoint(const TF_SquadWaypoint& msg)
    {
        if (m_mirror.squad == kInvalidSquad)
            return; // not squadded: stale/foreign echo

        switch (static_cast<SquadWpOp>(msg.op))
        {
        case SquadWpOp::Set:
            m_mirror.hasWaypoint = true;
            m_mirror.wp[0] = msg.wpX;
            m_mirror.wp[1] = msg.wpY;
            m_mirror.wp[2] = msg.wpZ;
            m_mirror.wpRegion = msg.regionId;
            break;

        case SquadWpOp::Clear:
            m_mirror.hasWaypoint = false;
            m_mirror.wpRegion = kInvalidRegion;
            break;

        case SquadWpOp::Request:
        {
            // Leader-side ping. One live ping per requester (latest wins).
            std::erase_if(m_mirror.wpRequests, [&msg](const WaypointRequest& r) { return r.from == msg.fromPlayer; });
            WaypointRequest req;
            req.from = msg.fromPlayer;
            req.region = msg.regionId;
            req.wp[0] = msg.wpX;
            req.wp[1] = msg.wpY;
            req.wp[2] = msg.wpZ;
            req.until = m_clock + kSquadWpRequestTTLSec;
            m_mirror.wpRequests.push_back(req);
            break;
        }

        default:
            break;
        }
    }

    void TFSquadSystem::SendWaypointOp(SquadWpOp op, const float wp[3], RegionId region)
    {
        if (!m_ctx || m_ctx->localPlayer == kInvalidPlayer)
            return;

        TF_SquadWaypoint m{};
        m.op = static_cast<uint8_t>(op);
        m.regionId = region;
        m.fromPlayer = m_ctx->localPlayer; // server overwrites; loopback uses it
        if (wp)
        {
            m.wpX = wp[0];
            m.wpY = wp[1];
            m.wpZ = wp[2];
        }

        if (m_ctx->IsAuthority())
        {
            // Same enter-world defense-in-depth as SendOp (see the note there).
#ifdef ENABLE_NETWORKING
            if (!m_ctx->serverSim || !m_ctx->serverSim->IsEnteredWorld(m_ctx->localPlayer))
                return;
#endif
            ServerHandleSquadWaypoint(m_ctx->localPlayer, m); // listen host / standalone
        }
        else if (m_ctx->clientNet)
            m_ctx->clientNet->SendMsg(static_cast<TFMsg>(kTFMsgSquadWaypoint), &m, sizeof(m));
    }

    void TFSquadSystem::SendOp(SquadOp op, PlayerId target, const float* wp)
    {
        if (!m_ctx || m_ctx->localPlayer == kInvalidPlayer)
            return;

        TF_SquadMsg m{};
        m.op = static_cast<uint8_t>(op);
        m.squadId = m_mirror.squad != kInvalidSquad ? m_mirror.squad : m_mirror.inviteSquad;
        m.targetPlayer = target;
        if (wp)
        {
            m.wpX = wp[0];
            m.wpY = wp[1];
            m.wpZ = wp[2];
        }

        if (m_ctx->IsAuthority())
        {
            // final-review #3 follow-up (gate defense-in-depth): this authority-
            // path call goes straight into ServerHandleSquadMsg, bypassing the
            // RouteClientMessage enter-world gate networked SquadMsg traffic is
            // held to. Mirror that gate so an un-entered-world local host can't
            // spam squad ops either. The gate only exists under ENABLE_NETWORKING
            // (RouteClientMessage/onboarding are ifdef'd out otherwise, and
            // m_enteredWorld would never be populated), so don't apply it to
            // builds without networking.
#ifdef ENABLE_NETWORKING
            if (!m_ctx->serverSim || !m_ctx->serverSim->IsEnteredWorld(m_ctx->localPlayer))
                return;
#endif
            ServerHandleSquadMsg(m_ctx->localPlayer, m); // listen host / standalone
        }
        else if (m_ctx->clientNet)
            m_ctx->clientNet->SendMsg(TFMsg::SquadMsg, &m, sizeof(m));
    }

#ifdef ENABLE_NETWORKING

    bool TFSquadSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    bool TFSquadSystem::ServerNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server;
    }

    void TFSquadSystem::EnsureClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();

        nm.RegisterHandler(static_cast<MessageType>(static_cast<uint16_t>(TFMsg::SquadMsg)),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_SquadMsg))
                                   return;
                               TF_SquadMsg msg;
                               std::memcpy(&msg, m.payload.data(), sizeof(msg));
                               ClientHandleEcho(msg);
                           });

        // W11 squad-v2: waypoint echoes (0x546C).
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgSquadWaypoint),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_SquadWaypoint))
                                   return;
                               TF_SquadWaypoint msg;
                               std::memcpy(&msg, m.payload.data(), sizeof(msg));
                               ClientHandleWaypoint(msg);
                           });

        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] squad mirror handlers registered");
    }

    void TFSquadSystem::ReleaseClientHandlers()
    {
        // NetworkManager has no per-type removal; replace our handlers with no-ops
        // so no dangling `this` survives module shutdown (TFServerSim pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (uint16_t id : {static_cast<uint16_t>(TFMsg::SquadMsg), kTFMsgSquadWaypoint})
        {
            nm.RegisterHandler(static_cast<Spark::Net::MessageType>(id), [](const Spark::Net::NetworkMessage&) {});
        }
        m_clientHandlers = false;
    }

    void TFSquadSystem::EnsureServerHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();

        // 0x546C is outside the frozen TFMsg enum, so it can't ride
        // TFServerSim::RouteClientMessage — mirror its enter-world gate here
        // instead (TFSocialSystem::EnsureServerHandlers precedent).
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgSquadWaypoint),
                           [this](const NetworkMessage& m)
                           {
                               if (!m_ctx || !m_ctx->serverSim || !m_ctx->serverSim->IsEnteredWorld(m.senderID))
                                   return;
                               ServerHandleSquadWaypointRaw(m.senderID, m.payload.data(), m.payload.size());
                           });

        m_serverHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] squad waypoint server handler registered");
    }

    void TFSquadSystem::ReleaseServerHandlers()
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFMsgSquadWaypoint),
                           [](const Spark::Net::NetworkMessage&) {});
        m_serverHandlers = false;
    }

#endif // ENABLE_NETWORKING

} // namespace Terrafront
