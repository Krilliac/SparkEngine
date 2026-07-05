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
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstring>

namespace Terrafront {

namespace {

void AddUnique(std::vector<PlayerId>& v, PlayerId p)
{
    if (std::find(v.begin(), v.end(), p) == v.end())
        v.push_back(p);
}

void Remove(std::vector<PlayerId>& v, PlayerId p)
{
    v.erase(std::remove(v.begin(), v.end(), p), v.end());
}

constexpr float kSweepPeriodSec = 1.0f;   // disconnect sweep cadence

} // namespace

TFSquadSystem::TFSquadSystem() = default;
TFSquadSystem::~TFSquadSystem() { if (m_initialized) Shutdown(); }

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
    (void)deltaTime;
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
#endif
}

void TFSquadSystem::FixedUpdate(float fixedDeltaTime)
{
    m_clock += fixedDeltaTime;

    if (!m_ctx || !m_ctx->IsAuthority())
        return;
    m_sweepAccum += fixedDeltaTime;
    if (m_sweepAccum >= kSweepPeriodSec)
    {
        m_sweepAccum = 0.0f;
        SweepDisconnected();
    }
}

void TFSquadSystem::Shutdown()
{
    if (!m_initialized)
        return;
#ifdef ENABLE_NETWORKING
    if (m_clientHandlers)
        ReleaseClientHandlers();
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
        case SquadOp::Create:      ServerCreate(sender);                                   break;
        case SquadOp::Invite:      ServerInvite(sender, msg.targetPlayer);                 break;
        case SquadOp::Accept:      ServerAccept(sender);                                   break;
        case SquadOp::Leave:       ServerKickOrLeave(sender, sender, SquadOp::Leave);      break;
        case SquadOp::Kick:        ServerKickOrLeave(sender, msg.targetPlayer, SquadOp::Kick); break;
        case SquadOp::Promote:     ServerPromote(sender, msg.targetPlayer);                break;
        case SquadOp::SetWaypoint: ServerSetWaypoint(sender, msg);                         break;
        default:
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] squads: player %u sent unknown SquadOp %u", sender, msg.op);
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
        return false;   // leaders don't spawn on themselves

    if (SquadSpawnCooldownRemaining(requester) > 0.0f)
        return false;

    PawnInfo leaderPawn{};
    if (!m_ctx->players->GetPawnByPlayer(squad.leader, leaderPawn) || !leaderPawn.alive)
        return false;

    outPos[0] = leaderPawn.pos[0];
    outPos[1] = leaderPawn.pos[1];
    outPos[2] = leaderPawn.pos[2];
    m_spawnCdUntil[requester] = m_clock + kSquadSpawnCooldownSec;   // charge on success
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
    return kInvalidSquad;   // 65535 live squads: not happening at 64 players
}

void TFSquadSystem::ServerCreate(PlayerId sender)
{
    if (m_squadOf.contains(sender))
        return;                                     // already squadded
    const FactionId faction = FactionOfPlayer(sender);
    if (faction == FactionId::None)
        return;                                     // must pick a faction first
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
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] squad %u created by player %u (%s)",
                   id, sender, FactionTag(faction));
}

void TFSquadSystem::ServerInvite(PlayerId sender, PlayerId target)
{
    auto sit = m_squadOf.find(sender);
    if (sit == m_squadOf.end())
        return;
    Squad* squad = FindSquad(sit->second);
    if (!squad || squad->leader != sender)
        return;                                     // leader-only
    if (target == kInvalidPlayer || target == sender || m_squadOf.contains(target))
        return;
    if (squad->members.size() >= kMaxSquadSize)
        return;
    if (FactionOfPlayer(target) != squad->faction)
        return;                                     // per-faction squads

    m_invites[target] = squad->id;                  // latest invite wins
    SendEchoTo(target, MakeEcho(SquadOp::Invite, squad->id, sender));
    SPARK_LOG_INFO(Spark::LogCategory::Game,
                   "[TF] squad %u: player %u invited %u", squad->id, sender, target);
}

void TFSquadSystem::ServerAccept(PlayerId sender)
{
    auto iit = m_invites.find(sender);
    if (iit == m_invites.end())
        return;
    const SquadId id = iit->second;
    m_invites.erase(iit);                           // consume either way

    Squad* squad = FindSquad(id);
    if (!squad || m_squadOf.contains(sender) ||
        squad->members.size() >= kMaxSquadSize ||
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
        wp.wpX = squad->wp[0]; wp.wpY = squad->wp[1]; wp.wpZ = squad->wp[2];
        SendEchoTo(sender, wp);
    }

    if (m_events)
        m_events->Fire(EvSquadChanged{id, sender, true});
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] squad %u: player %u joined (%zu/%u)",
                   id, sender, squad->members.size(), kMaxSquadSize);
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
            return;                                 // leader-only
        if (target == sender ||
            std::find(squad->members.begin(), squad->members.end(), target) ==
                squad->members.end())
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
    if (std::find(squad->members.begin(), squad->members.end(), target) ==
        squad->members.end())
        return;

    squad->leader = target;
    EchoToMembers(*squad, MakeEcho(SquadOp::Promote, squad->id, target));
    SPARK_LOG_INFO(Spark::LogCategory::Game,
                   "[TF] squad %u: player %u promoted to leader", squad->id, target);
}

void TFSquadSystem::ServerSetWaypoint(PlayerId sender, const TF_SquadMsg& msg)
{
    auto sit = m_squadOf.find(sender);
    if (sit == m_squadOf.end())
        return;
    Squad* squad = FindSquad(sit->second);
    if (!squad || squad->leader != sender)
        return;                                     // leader-only

    squad->hasWaypoint = true;
    squad->wp[0] = msg.wpX;
    squad->wp[1] = msg.wpY;
    squad->wp[2] = msg.wpZ;

    TF_SquadMsg echo = MakeEcho(SquadOp::SetWaypoint, squad->id, sender);
    echo.wpX = msg.wpX; echo.wpY = msg.wpY; echo.wpZ = msg.wpZ;
    EchoToMembers(*squad, echo);
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
        squad->leader = squad->members.front();     // auto-promote oldest member
        EchoToMembers(*squad, MakeEcho(SquadOp::Promote, id, squad->leader));
        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "[TF] squad %u: leader left — player %u promoted", id, squad->leader);
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
        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "[TF] squads: player %u disconnected — removed", player);
    }
    std::erase_if(m_invites, [&clients, this](const auto& kv) {
        return kv.first != m_ctx->localPlayer && !clients.contains(kv.first);
    });
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
    if (m_ctx->HasLocalPlayer() && target == m_ctx->localPlayer &&
        m_ctx->role != NetRole::Client)
    {
        ClientHandleEcho(echo);
        return;
    }
#ifdef ENABLE_NETWORKING
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    if (!nm.IsInitialized())
        return;
    Spark::Net::NetworkMessage msg;
    msg.type = static_cast<Spark::Net::MessageType>(
        static_cast<uint16_t>(TFMsg::SquadMsg));
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
            SPARK_LOG_INFO(Spark::LogCategory::Game,
                           "[TF] squad invite from player %u (squad %u)",
                           msg.targetPlayer, msg.squadId);
            break;

        case SquadOp::Accept:
            if (m_mirror.squad == kInvalidSquad)
            {
                m_mirror.squad = msg.squadId;       // our own join confirmation
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
                ResetMirror();                      // we left / were kicked
            }
            else
            {
                Remove(m_mirror.members, msg.targetPlayer);
                if (m_mirror.leader == msg.targetPlayer)
                    m_mirror.leader = kInvalidPlayer;   // Promote echo follows
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
        ServerHandleSquadMsg(m_ctx->localPlayer, m);    // listen host / standalone
    else if (m_ctx->clientNet)
        m_ctx->clientNet->SendMsg(TFMsg::SquadMsg, &m, sizeof(m));
}

#ifdef ENABLE_NETWORKING

bool TFSquadSystem::ClientNetActive() const
{
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    return m_ctx->role == NetRole::Client &&
           nm.IsInitialized() &&
           nm.GetRole() == Spark::Net::NetworkRole::Client &&
           nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
}

void TFSquadSystem::EnsureClientHandlers()
{
    using Spark::Net::MessageType;
    using Spark::Net::NetworkMessage;
    auto& nm = Spark::Net::NetworkManager::GetInstance();

    nm.RegisterHandler(
        static_cast<MessageType>(static_cast<uint16_t>(TFMsg::SquadMsg)),
        [this](const NetworkMessage& m) {
            if (m.payload.size() != sizeof(TF_SquadMsg))
                return;
            TF_SquadMsg msg;
            std::memcpy(&msg, m.payload.data(), sizeof(msg));
            ClientHandleEcho(msg);
        });

    m_clientHandlers = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] squad mirror handlers registered");
}

void TFSquadSystem::ReleaseClientHandlers()
{
    // NetworkManager has no per-type removal; replace our handler with a no-op
    // so no dangling `this` survives module shutdown (TFServerSim pattern).
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    nm.RegisterHandler(
        static_cast<Spark::Net::MessageType>(static_cast<uint16_t>(TFMsg::SquadMsg)),
        [](const Spark::Net::NetworkMessage&) {});
    m_clientHandlers = false;
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
                ImGui::Text("invite: squad %u from player %u",
                            m_mirror.inviteSquad, m_mirror.inviteFrom);
                ImGui::SameLine();
                if (ImGui::Button("Accept"))
                    SendOp(SquadOp::Accept, kInvalidPlayer);
            }
        }
        else
        {
            ImGui::Text("squad %u  (%zu/%u members)", m_mirror.squad,
                        m_mirror.members.size(), kMaxSquadSize);
            for (const PlayerId member : m_mirror.members)
                ImGui::Text("  %s p%u%s", member == m_mirror.leader ? "*" : " ",
                            member, member == m_ctx->localPlayer ? " (you)" : "");
            if (m_mirror.hasWaypoint)
                ImGui::Text("waypoint: (%.0f %.0f %.0f)",
                            m_mirror.wp[0], m_mirror.wp[1], m_mirror.wp[2]);

            if (ImGui::Button("Leave"))
                SendOp(SquadOp::Leave, kInvalidPlayer);

            const bool isLeader = m_mirror.leader == m_ctx->localPlayer;
            if (isLeader)
            {
                ImGui::InputInt("target player", &m_uiTargetId);
                const auto target = static_cast<PlayerId>(m_uiTargetId);
                if (ImGui::Button("Invite"))  SendOp(SquadOp::Invite, target);
                ImGui::SameLine();
                if (ImGui::Button("Kick"))    SendOp(SquadOp::Kick, target);
                ImGui::SameLine();
                if (ImGui::Button("Promote")) SendOp(SquadOp::Promote, target);

                PawnInfo self{};
                if (m_ctx->players &&
                    m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, self) && self.alive)
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
        ImGui::Text("server squads : %zu   invites pending: %zu",
                    m_squads.size(), m_invites.size());
        for (const auto& [id, squad] : m_squads)
        {
            ImGui::Text("squad %u [%s] leader p%u  %zu/%u%s", id,
                        FactionTag(squad.faction), squad.leader,
                        squad.members.size(), kMaxSquadSize,
                        squad.hasWaypoint ? "  wp" : "");
        }
    }
#endif // SPARK_HAS_IMGUI
}

} // namespace Terrafront
