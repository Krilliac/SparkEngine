/**
 * @file TFSquadSystemServer.cpp
 * @brief TFSquadSystem server registry: TF_SquadMsg op handlers
 *        (Create/Invite/Accept/Kick/Leave/Promote/SetWaypoint), the W11
 *        TF_SquadWaypoint (0x546C) Set/Clear/Request handler, the
 *        squad-leader spawn rule and the disconnect sweep. Split from
 *        TFSquadSystem.cpp; the shared roster helpers live in
 *        TFSquadSystemInternal.h.
 */
#include "Game/TFSquadSystem.h"

#include "Game/TFPlayerSystem.h"
#include "Game/TFSquadSystemInternal.h"
#include "Net/TFServerSim.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace Terrafront
{

    using namespace SquadDetail;

    // ---------------------------------------------------------------------------
    // W4 cross-system API
    // ---------------------------------------------------------------------------

    void TFSquadSystem::ServerHandleSquadMsgRaw(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_SquadMsg) || data == nullptr)
            return;
        TF_SquadMsg msg;
        std::memcpy(&msg, data, sizeof(msg));
        ServerHandleSquadMsg(sender, msg);
    }

    void TFSquadSystem::ServerHandleSquadMsg(PlayerId sender, const TF_SquadMsg& msg)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || sender == kInvalidPlayer)
            return;

        switch (static_cast<SquadOp>(msg.op))
        {
        case SquadOp::Create:
            ServerCreate(sender);
            break;
        case SquadOp::Invite:
            ServerInvite(sender, msg.targetPlayer);
            break;
        case SquadOp::Accept:
            ServerAccept(sender);
            break;
        case SquadOp::Leave:
            ServerKickOrLeave(sender, sender, SquadOp::Leave);
            break;
        case SquadOp::Kick:
            ServerKickOrLeave(sender, msg.targetPlayer, SquadOp::Kick);
            break;
        case SquadOp::Promote:
            ServerPromote(sender, msg.targetPlayer);
            break;
        case SquadOp::SetWaypoint:
            ServerSetWaypoint(sender, msg);
            break;
        default:
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] squads: player %u sent unknown SquadOp %u", sender, msg.op);
            break;
        }
    }

    bool TFSquadSystem::GetSquadLeaderSpawn(PlayerId requester, float outPos[3])
    {
        if (!m_ctx || !m_ctx->IsAuthority() || !m_ctx->players || outPos == nullptr)
            return false;

        auto sit = m_squadOf.find(requester);
        if (sit == m_squadOf.end())
            return false;
        auto qit = m_squads.find(sit->second);
        if (qit == m_squads.end())
            return false;
        const Squad& squad = qit->second;
        if (squad.leader == requester || squad.leader == kInvalidPlayer)
            return false; // leaders don't spawn on themselves

        if (SquadSpawnCooldownRemaining(requester) > 0.0f)
            return false;

        PawnInfo leaderPawn{};
        if (!m_ctx->players->GetPawnByPlayer(squad.leader, leaderPawn) || !leaderPawn.alive)
            return false;

        outPos[0] = leaderPawn.pos[0];
        outPos[1] = leaderPawn.pos[1];
        outPos[2] = leaderPawn.pos[2];
        m_spawnCdUntil[requester] = m_clock + kSquadSpawnCooldownSec; // charge on success
        return true;
    }

    float TFSquadSystem::SquadSpawnCooldownRemaining(PlayerId requester) const
    {
        auto it = m_spawnCdUntil.find(requester);
        if (it == m_spawnCdUntil.end() || it->second <= m_clock)
            return 0.0f;
        return static_cast<float>(it->second - m_clock);
    }

    // ---------------------------------------------------------------------------
    // Server op handlers
    // ---------------------------------------------------------------------------

    TFSquadSystem::Squad* TFSquadSystem::FindSquad(SquadId id)
    {
        auto it = m_squads.find(id);
        return it == m_squads.end() ? nullptr : &it->second;
    }

    FactionId TFSquadSystem::FactionOfPlayer(PlayerId player) const
    {
        FactionId f = FactionId::None;
        if (m_ctx->players)
            f = m_ctx->players->FactionOf(player);
        if (f == FactionId::None && m_ctx->serverSim)
            f = m_ctx->serverSim->GetPlayerFaction(player);
        return f;
    }

    SquadId TFSquadSystem::AllocSquadId()
    {
        // uint16 wrap-safe: skip 0 and ids still in use.
        for (uint32_t tries = 0; tries <= 0xFFFFu; ++tries)
        {
            const SquadId id = m_nextSquadId++;
            if (m_nextSquadId == kInvalidSquad)
                m_nextSquadId = 1;
            if (id != kInvalidSquad && !m_squads.contains(id))
                return id;
        }
        return kInvalidSquad; // 65535 live squads: not happening at 64 players
    }

    void TFSquadSystem::ServerCreate(PlayerId sender)
    {
        if (m_squadOf.contains(sender))
            return; // already squadded
        const FactionId faction = FactionOfPlayer(sender);
        if (faction == FactionId::None)
            return; // must pick a faction first
        const SquadId id = AllocSquadId();
        if (id == kInvalidSquad)
            return;

        Squad squad;
        squad.id = id;
        squad.faction = faction;
        squad.leader = sender;
        squad.members.push_back(sender);
        m_squads[id] = squad;
        m_squadOf[sender] = id;
        m_invites.erase(sender);

        SendEchoTo(sender, MakeEcho(SquadOp::Create, id, sender));
        if (m_events)
            m_events->Fire(EvSquadChanged{id, sender, true});
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] squad %u created by player %u (%s)", id, sender,
                       FactionTag(faction));
    }

    void TFSquadSystem::ServerInvite(PlayerId sender, PlayerId target)
    {
        auto sit = m_squadOf.find(sender);
        if (sit == m_squadOf.end())
            return;
        Squad* squad = FindSquad(sit->second);
        if (!squad || squad->leader != sender)
            return; // leader-only
        if (target == kInvalidPlayer || target == sender || m_squadOf.contains(target))
            return;
        if (squad->members.size() >= kMaxSquadSize)
            return;
        if (FactionOfPlayer(target) != squad->faction)
            return; // per-faction squads

        m_invites[target] = squad->id; // latest invite wins
        SendEchoTo(target, MakeEcho(SquadOp::Invite, squad->id, sender));
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] squad %u: player %u invited %u", squad->id, sender, target);
    }

    void TFSquadSystem::ServerAccept(PlayerId sender)
    {
        auto iit = m_invites.find(sender);
        if (iit == m_invites.end())
            return;
        const SquadId id = iit->second;
        m_invites.erase(iit); // consume either way

        Squad* squad = FindSquad(id);
        if (!squad || m_squadOf.contains(sender) || squad->members.size() >= kMaxSquadSize ||
            FactionOfPlayer(sender) != squad->faction)
            return;

        squad->members.push_back(sender);
        m_squadOf[sender] = id;

        // Broadcast the join (the joiner's own mirror initializes from this too).
        EchoToMembers(*squad, MakeEcho(SquadOp::Accept, id, sender));

        // Roster sync for the joiner: existing members + leader + waypoint.
        for (const PlayerId member : squad->members)
            if (member != sender)
                SendEchoTo(sender, MakeEcho(SquadOp::Accept, id, member));
        SendEchoTo(sender, MakeEcho(SquadOp::Promote, id, squad->leader));
        if (squad->hasWaypoint)
        {
            TF_SquadMsg wp = MakeEcho(SquadOp::SetWaypoint, id, squad->leader);
            wp.wpX = squad->wp[0];
            wp.wpY = squad->wp[1];
            wp.wpZ = squad->wp[2];
            SendEchoTo(sender, wp);
        }

        if (m_events)
            m_events->Fire(EvSquadChanged{id, sender, true});
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] squad %u: player %u joined (%zu/%u)", id, sender,
                       squad->members.size(), kMaxSquadSize);
    }

    void TFSquadSystem::ServerKickOrLeave(PlayerId sender, PlayerId target, SquadOp op)
    {
        auto sit = m_squadOf.find(sender);
        if (sit == m_squadOf.end())
            return;
        if (op == SquadOp::Kick)
        {
            Squad* squad = FindSquad(sit->second);
            if (!squad || squad->leader != sender)
                return; // leader-only
            if (target == sender ||
                std::find(squad->members.begin(), squad->members.end(), target) == squad->members.end())
                return;
        }
        RemoveFromSquad(target, op);
    }

    void TFSquadSystem::ServerPromote(PlayerId sender, PlayerId target)
    {
        auto sit = m_squadOf.find(sender);
        if (sit == m_squadOf.end())
            return;
        Squad* squad = FindSquad(sit->second);
        if (!squad || squad->leader != sender || target == sender)
            return;
        if (std::find(squad->members.begin(), squad->members.end(), target) == squad->members.end())
            return;

        squad->leader = target;
        EchoToMembers(*squad, MakeEcho(SquadOp::Promote, squad->id, target));
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] squad %u: player %u promoted to leader", squad->id, target);
    }

    void TFSquadSystem::ServerSetWaypoint(PlayerId sender, const TF_SquadMsg& msg)
    {
        auto sit = m_squadOf.find(sender);
        if (sit == m_squadOf.end())
            return;
        Squad* squad = FindSquad(sit->second);
        if (!squad || squad->leader != sender)
            return; // leader-only

        squad->hasWaypoint = true;
        squad->wp[0] = msg.wpX;
        squad->wp[1] = msg.wpY;
        squad->wp[2] = msg.wpZ;
        squad->wpRegion = kInvalidRegion; // legacy path: no map hex hint

        TF_SquadMsg echo = MakeEcho(SquadOp::SetWaypoint, squad->id, sender);
        echo.wpX = msg.wpX;
        echo.wpY = msg.wpY;
        echo.wpZ = msg.wpZ;
        EchoToMembers(*squad, echo);
    }

    // ---------------------------------------------------------------------------
    // W11 squad-v2: TF_SquadWaypoint (0x546C) — leader Set/Clear + member Request
    // ---------------------------------------------------------------------------

    void TFSquadSystem::ServerHandleSquadWaypointRaw(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_SquadWaypoint) || data == nullptr)
            return;
        TF_SquadWaypoint msg;
        std::memcpy(&msg, data, sizeof(msg));
        ServerHandleSquadWaypoint(sender, msg);
    }

    void TFSquadSystem::ServerHandleSquadWaypoint(PlayerId sender, const TF_SquadWaypoint& msg)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || sender == kInvalidPlayer)
            return;

        auto sit = m_squadOf.find(sender);
        if (sit == m_squadOf.end())
            return;
        Squad* squad = FindSquad(sit->second);
        if (!squad)
            return;

        // Sanitize coordinates once for both Set and Request (TFServerSim world
        // bounds [0, 4096]; Y stays whatever terrain height the client sampled).
        TF_SquadWaypoint echo = msg;
        echo.fromPlayer = sender; // server-filled: clients never self-identify
        echo.wpX = std::clamp(echo.wpX, 0.0f, 4096.0f);
        echo.wpZ = std::clamp(echo.wpZ, 0.0f, 4096.0f);
        echo.wpY = std::clamp(echo.wpY, -1000.0f, 10000.0f);

        switch (static_cast<SquadWpOp>(msg.op))
        {
        case SquadWpOp::Set:
            if (squad->leader != sender)
                return; // leader-only
            squad->hasWaypoint = true;
            squad->wp[0] = echo.wpX;
            squad->wp[1] = echo.wpY;
            squad->wp[2] = echo.wpZ;
            squad->wpRegion = echo.regionId;
            EchoWaypointToMembers(*squad, echo);
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] squad %u: waypoint set (%.0f %.0f %.0f) by player %u",
                           squad->id, echo.wpX, echo.wpY, echo.wpZ, sender);
            break;

        case SquadWpOp::Clear:
            if (squad->leader != sender || !squad->hasWaypoint)
                return; // leader-only; Clear with no waypoint is a no-op
            squad->hasWaypoint = false;
            squad->wpRegion = kInvalidRegion;
            EchoWaypointToMembers(*squad, echo);
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] squad %u: waypoint cleared by player %u", squad->id, sender);
            break;

        case SquadWpOp::Request:
            // Non-leader suggestion: forwarded to the LEADER ONLY (the ping
            // never reaches other members; the leader promotes it via Set).
            if (squad->leader == sender || squad->leader == kInvalidPlayer)
                return;
            SendWaypointEchoTo(squad->leader, echo);
            break;

        default:
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] squads: player %u sent unknown SquadWpOp %u", sender,
                           msg.op);
            break;
        }
    }

    void TFSquadSystem::RemoveFromSquad(PlayerId player, SquadOp echoOp)
    {
        auto sit = m_squadOf.find(player);
        if (sit == m_squadOf.end())
            return;
        const SquadId id = sit->second;
        m_squadOf.erase(sit);

        Squad* squad = FindSquad(id);
        if (!squad)
            return;
        Remove(squad->members, player);

        // Tell everyone (including the removed player, so their mirror clears).
        const TF_SquadMsg echo = MakeEcho(echoOp, id, player);
        EchoToMembers(*squad, echo);
        SendEchoTo(player, echo);

        if (squad->members.empty())
        {
            m_squads.erase(id);
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] squad %u disbanded", id);
        }
        else if (squad->leader == player)
        {
            squad->leader = squad->members.front(); // auto-promote oldest member
            EchoToMembers(*squad, MakeEcho(SquadOp::Promote, id, squad->leader));
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] squad %u: leader left — player %u promoted", id,
                           squad->leader);
        }

        if (m_events)
            m_events->Fire(EvSquadChanged{id, player, false});
    }

    void TFSquadSystem::SweepDisconnected()
    {
#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return;
        const auto& clients = nm.GetClients();

        std::vector<PlayerId> gone;
        for (const auto& [player, squad] : m_squadOf)
        {
            (void)squad;
            if (player != m_ctx->localPlayer && !clients.contains(player))
                gone.push_back(player);
        }
        for (const PlayerId player : gone)
        {
            RemoveFromSquad(player, SquadOp::Leave);
            m_spawnCdUntil.erase(player);
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] squads: player %u disconnected — removed", player);
        }
        std::erase_if(m_invites, [&clients, this](const auto& kv)
                      { return kv.first != m_ctx->localPlayer && !clients.contains(kv.first); });
#endif
    }

} // namespace Terrafront
