/**
 * @file TFSquadSystem.cpp
 * @brief Squads of 6 (W4): server registry + TF_SquadMsg ops + state echoes,
 *        squad-leader spawn rule (30 s cd), client mirror, debug UI.
 *
 * Wire convention (see TFSquadSystem.h): C->S TF_SquadMsg is the op request;
 * S->C reuses the SAME struct as a state echo — op = what happened,
 * targetPlayer = subject player. Roster sync for a joiner is one Accept echo
 * per existing member + a Promote echo naming the leader + the waypoint.
 */
#include "Game/TFSquadSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Net/TFClientNet.h"
#include "Net/TFServerSim.h"
#include "World/TFWorldSetup.h" // W11 squad-v2: TerrainHeightAt for map waypoints
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace Terrafront
{

    namespace
    {

        void AddUnique(std::vector<PlayerId>& v, PlayerId p)
        {
            if (std::find(v.begin(), v.end(), p) == v.end())
                v.push_back(p);
        }

        void Remove(std::vector<PlayerId>& v, PlayerId p)
        {
            v.erase(std::remove(v.begin(), v.end(), p), v.end());
        }

        constexpr float kSweepPeriodSec = 1.0f; // disconnect sweep cadence

    } // namespace

    TFSquadSystem::TFSquadSystem() = default;
    TFSquadSystem::~TFSquadSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFSquadSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFSquadSystem initialized");
        return true;
    }

    void TFSquadSystem::Update(float deltaTime)
    {
        // W11 squad-v2 fix: the clock, disconnect sweep, and request-TTL used to
        // live in FixedUpdate — which Main.cpp's OnFixedUpdate NEVER calls for
        // this system, so m_clock froze at 0: one squad-leader spawn charged a
        // cooldown that never expired, and disconnected players were never swept
        // out of squads. They now run here (Update IS wired). None of this is
        // sim state — cooldown/TTL bookkeeping only — so frame-time cadence is
        // fine and determinism is untouched.
        m_clock += deltaTime;

        // Expire stale leader-side waypoint request pings (client mirror state).
        if (!m_mirror.wpRequests.empty())
            std::erase_if(m_mirror.wpRequests, [this](const WaypointRequest& r) { return r.until <= m_clock; });

        if (m_ctx && m_ctx->IsAuthority())
        {
            m_sweepAccum += deltaTime;
            if (m_sweepAccum >= kSweepPeriodSec)
            {
                m_sweepAccum = 0.0f;
                SweepDisconnected();
            }
        }

#ifdef ENABLE_NETWORKING
        // Client mirror handler lifecycle. Registered AFTER link-up so our real
        // handler wins the single per-type slot over TFClientNet's
        // accepted-but-unrouted no-op (TFRegionSystem pattern).
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_clientHandlers)
            EnsureClientHandlers();
        else if (!clientUp && m_clientHandlers)
        {
            ReleaseClientHandlers();
            ResetMirror();
        }

        // W11 squad-v2: 0x546C rides NetworkManager directly (block id outside
        // the frozen TFMsg enum), so the server route self-registers here — the
        // TFSocialSystem::EnsureServerHandlers pattern, enter-world gate mirrored.
        if (m_ctx && m_ctx->IsAuthority() && ServerNetActive() && !m_serverHandlers)
            EnsureServerHandlers();
#endif
    }

    void TFSquadSystem::FixedUpdate(float fixedDeltaTime)
    {
        // Intentionally empty (W11 squad-v2): Main.cpp's OnFixedUpdate never
        // wired this system, so everything time-based moved to Update() — see
        // the note there. Kept as a no-op for the frozen lifecycle contract.
        (void)fixedDeltaTime;
    }

    void TFSquadSystem::Shutdown()
    {
        if (!m_initialized)
            return;
#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ReleaseClientHandlers();
        if (m_serverHandlers)
            ReleaseServerHandlers();
#endif
        m_squads.clear();
        m_squadOf.clear();
        m_invites.clear();
        m_spawnCdUntil.clear();
        ResetMirror();
        m_initialized = false;
    }

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

    SquadId TFSquadSystem::SquadOf(PlayerId player) const
    {
        if (m_ctx && m_ctx->IsAuthority())
        {
            auto it = m_squadOf.find(player);
            return it == m_squadOf.end() ? kInvalidSquad : it->second;
        }
        if (m_ctx && player == m_ctx->localPlayer)
            return m_mirror.squad;
        return kInvalidSquad;
    }

    bool TFSquadSystem::IsLocalSquadMember(PlayerId player) const
    {
        if (!m_ctx || player == kInvalidPlayer)
            return false;
        if (m_ctx->IsAuthority())
        {
            const SquadId mine = SquadOf(m_ctx->localPlayer);
            return mine != kInvalidSquad && SquadOf(player) == mine;
        }
        if (m_mirror.squad == kInvalidSquad)
            return false;
        for (const PlayerId member : m_mirror.members)
            if (member == player)
                return true;
        return false;
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

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFSquadSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Squads"))
            return;

        // --- local player view (mirror) -----------------------------------------
        if (m_ctx && m_ctx->HasLocalPlayer())
        {
            if (m_mirror.squad == kInvalidSquad)
            {
                ImGui::TextUnformatted("no squad");
                if (ImGui::Button("Create squad"))
                    SendOp(SquadOp::Create, kInvalidPlayer);
                if (m_mirror.inviteSquad != kInvalidSquad)
                {
                    ImGui::Text("invite: squad %u from player %u", m_mirror.inviteSquad, m_mirror.inviteFrom);
                    ImGui::SameLine();
                    if (ImGui::Button("Accept"))
                        SendOp(SquadOp::Accept, kInvalidPlayer);
                }
            }
            else
            {
                ImGui::Text("squad %u  (%zu/%u members)", m_mirror.squad, m_mirror.members.size(), kMaxSquadSize);
                for (const PlayerId member : m_mirror.members)
                    ImGui::Text("  %s p%u%s", member == m_mirror.leader ? "*" : " ", member,
                                member == m_ctx->localPlayer ? " (you)" : "");
                if (m_mirror.hasWaypoint)
                {
                    ImGui::Text("waypoint: (%.0f %.0f %.0f)", m_mirror.wp[0], m_mirror.wp[1], m_mirror.wp[2]);
                    if (IsLocalSquadLeader())
                    {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Clear##wp"))
                            UiClearWaypoint();
                    }
                }

                // W11 squad-v2: leader-side waypoint request pings.
                for (size_t i = 0; i < m_mirror.wpRequests.size(); ++i)
                {
                    const WaypointRequest& rq = m_mirror.wpRequests[i];
                    ImGui::Text("wp request p%u (%.0f %.0f %.0f)", rq.from, rq.wp[0], rq.wp[1], rq.wp[2]);
                    ImGui::SameLine();
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::SmallButton("Set"))
                    {
                        PromoteWaypointRequest(i);
                        ImGui::PopID();
                        break; // vector mutated — bail this frame
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X"))
                    {
                        DismissWaypointRequest(i);
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }

                if (ImGui::Button("Leave"))
                    SendOp(SquadOp::Leave, kInvalidPlayer);

                const bool isLeader = m_mirror.leader == m_ctx->localPlayer;
                if (isLeader)
                {
                    ImGui::InputInt("target player", &m_uiTargetId);
                    const auto target = static_cast<PlayerId>(m_uiTargetId);
                    if (ImGui::Button("Invite"))
                        SendOp(SquadOp::Invite, target);
                    ImGui::SameLine();
                    if (ImGui::Button("Kick"))
                        SendOp(SquadOp::Kick, target);
                    ImGui::SameLine();
                    if (ImGui::Button("Promote"))
                        SendOp(SquadOp::Promote, target);

                    PawnInfo self{};
                    if (m_ctx->players && m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, self) && self.alive)
                    {
                        if (ImGui::Button("Set waypoint here"))
                            SendOp(SquadOp::SetWaypoint, kInvalidPlayer, self.pos);
                    }
                }
            }
            const float cd = SquadSpawnCooldownRemaining(m_ctx->localPlayer);
            if (cd > 0.0f)
                ImGui::Text("squad-spawn cooldown: %.0fs", cd);
        }

        // --- authority registry --------------------------------------------------
        if (m_ctx && m_ctx->IsAuthority())
        {
            ImGui::Separator();
            ImGui::Text("server squads : %zu   invites pending: %zu", m_squads.size(), m_invites.size());
            for (const auto& [id, squad] : m_squads)
            {
                ImGui::Text("squad %u [%s] leader p%u  %zu/%u%s", id, FactionTag(squad.faction), squad.leader,
                            squad.members.size(), kMaxSquadSize, squad.hasWaypoint ? "  wp" : "");
            }
        }
#endif // SPARK_HAS_IMGUI
    }

} // namespace Terrafront
