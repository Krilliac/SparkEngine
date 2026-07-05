/**
 * @file TFServerSim.cpp
 * @brief Authoritative fixed-tick simulation: input consume, movement validate,
 *        lag-comp snapshots, TFMsg server-side routing (W1).
 */
#include "Net/TFServerSim.h"

#include "Data/TFDataTables.h"
#include "World/TFWorldSetup.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponSystem.h"

#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"
#include "Game/TFComponents.h"
#include "Game/TFMovementModel.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Terrafront {

namespace {

// Engine-side movement limits. Balance numbers (run/sprint speed) come from
// classes.json via TFDataTables; these are simulation constants.
constexpr float kWorldMin         = 0.0f;
constexpr float kWorldMax         = 4096.0f;
constexpr float kPitchLimitRad    = 1.55f;
constexpr float kRespawnDelaySec  = 8.0f;   // DESIGN.md §4 default
constexpr float kSpeedTolerance   = 1.25f;  // sprint * this == hard cap
constexpr int   kMaxInputsPerTick = 3;      // catch-up bound per fixed tick
constexpr size_t kMaxQueuedInputs = 32;
constexpr float kDefaultRunSpeed    = 5.2f; // fallback if classes.json missing
constexpr float kDefaultSprintSpeed = 7.2f;
constexpr float kRadToDeg = 57.2957795f;

} // namespace

TFServerSim::TFServerSim() = default;
TFServerSim::~TFServerSim() { if (m_initialized) Shutdown(); }

bool TFServerSim::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;

    m_lagComp.Configure(kLagCompWindowSec, kServerTickHz);
    m_lagCompConfigured = true;

    events.Subscribe<EvPlayerSpawned>([this](const EvPlayerSpawned& ev) { OnPlayerSpawned(ev); });
    events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnPlayerKilled(ev); });

    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFServerSim initialized");
    return true;
}

void TFServerSim::Update(float deltaTime)
{
    (void)deltaTime; // authoritative work runs on the fixed step only
}

void TFServerSim::FixedUpdate(float fixedDeltaTime)
{
    if (!m_initialized || !m_ctx)
        return;

#ifdef ENABLE_NETWORKING
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    const bool serverUp = nm.IsInitialized() &&
                          nm.GetRole() == Spark::Net::NetworkRole::Server &&
                          m_ctx->IsAuthority();
    if (serverUp && !m_handlersRegistered)
        RegisterNetHandlers();
    else if (!serverUp && m_handlersRegistered)
    {
        UnregisterNetHandlers();
        m_knownClients.clear();
    }
    if (serverUp)
        PollClientJoinsLeaves();
#endif

    if (m_ctx->IsAuthority())
        TickAuthoritative(fixedDeltaTime);
}

void TFServerSim::OnAreaTick(float fixedDt)
{
    // W1: the module's OnFixedUpdate is the single authoritative driver; this
    // hook only proves AreaServer::SetSimulation wiring is alive.
    // TF-W2: move the tick here for dedicated multi-area topology.
    (void)fixedDt;
    ++m_areaTickCount;
}

void TFServerSim::Shutdown()
{
    if (!m_initialized)
        return;
#ifdef ENABLE_NETWORKING
    if (m_handlersRegistered)
        UnregisterNetHandlers();
#endif
    m_inputs.clear();
    m_move.clear();
    m_factions.clear();
    m_deathTime.clear();
    m_knownClients.clear();
    m_lagComp.Clear();
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// Frozen cross-system API
// ---------------------------------------------------------------------------

void TFServerSim::EnqueueInput(PlayerId player, const TF_ClientInput& input)
{
    auto mv = m_move.find(player);
    if (mv == m_move.end())
        return; // no alive pawn — nothing to move

    if (input.seq != 0 && input.seq <= mv->second.lastSeq)
        return; // stale / duplicate

    auto& q = m_inputs[player];
    if (q.size() >= kMaxQueuedInputs)
        q.pop_front(); // drop oldest under flood; never grow unbounded
    q.push_back(input);
}

FactionId TFServerSim::GetPlayerFaction(PlayerId player) const
{
    auto it = m_factions.find(player);
    return it == m_factions.end() ? FactionId::None : it->second;
}

void TFServerSim::SetPlayerFaction(PlayerId player, FactionId faction)
{
    if (faction != FactionId::MRA && faction != FactionId::AUC && faction != FactionId::HLX)
        return;
    m_factions[player] = faction;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] Player %u joined %s",
                   player, FactionName(faction));
}

// ---------------------------------------------------------------------------
// Authoritative tick
// ---------------------------------------------------------------------------

void TFServerSim::TickAuthoritative(float fdt)
{
    m_serverTime += fdt;
    ++m_tickCount;

    TickMovement(fdt);
    RecordLagCompSnapshot();

#ifdef ENABLE_NETWORKING
    m_moveStateAccum += fdt;
    if (m_moveStateAccum >= 1.0f / kReplicationHz)
    {
        m_moveStateAccum = 0.0f;
        SendMoveStates();
    }
#endif
}

void TFServerSim::TickMovement(float fdt)
{
    for (auto& [player, ms] : m_move)
    {
        auto qit = m_inputs.find(player);
        int applied = 0;
        if (qit != m_inputs.end())
        {
            auto& q = qit->second;
            while (!q.empty() && applied < kMaxInputsPerTick)
            {
                const TF_ClientInput in = q.front();
                q.pop_front();
                StepPlayer(ms, &in, fdt);
                ms.lastSeq = in.seq;
                ++applied;
            }
        }
        if (applied == 0)
            StepPlayer(ms, nullptr, fdt); // input starvation: friction + gravity only

        WritePawnTransform(ms);
    }
}

void TFServerSim::StepPlayer(MoveState& ms, const TF_ClientInput* in, float dt)
{
    // --- class speed caps (data-driven) ---
    float runSpeed = kDefaultRunSpeed;
    float sprintSpeed = kDefaultSprintSpeed;
    if (m_ctx->data)
    {
        if (const ClassDef* cd = m_ctx->data->GetClass(ms.cls))
        {
            runSpeed = cd->runSpeed;
            sprintSpeed = cd->sprintSpeed;
        }
    }

    // --- decode + validate input ---
    float mx = 0.0f, my = 0.0f;
    uint16_t buttons = 0;
    if (in)
    {
        mx = static_cast<float>(in->moveX) / 127.0f;
        my = static_cast<float>(in->moveY) / 127.0f;
        const float mag = std::sqrt(mx * mx + my * my);
        if (mag > 1.0f)
        {
            // client asked for more than a unit wish vector: clamp, log, keep playing
            mx /= mag;
            my /= mag;
            ++m_speedClamps;
            if (m_serverTime - m_lastViolationLog > 5.0)
            {
                m_lastViolationLog = m_serverTime;
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "[TF] movement validation: clamped over-unit move input (%u total)",
                               m_speedClamps);
            }
        }
        ms.yaw = QuantAim::WrapPi(in->viewYaw);
        ms.pitch = std::clamp(in->viewPitch, -kPitchLimitRad, kPitchLimitRad);
        buttons = in->buttons;
    }

    // --- shared TF movement model v1 (Game/TFMovementModel.h) ---
    // MUST stay byte-identical in behavior to the client's ClientPrediction
    // simulator (TFClientNet::SimulateMove); both call the same TFMoveStep.
    TFMoveState mstate;
    mstate.pos[0] = ms.pos[0]; mstate.pos[1] = ms.pos[1]; mstate.pos[2] = ms.pos[2];
    mstate.vel[0] = ms.vel[0]; mstate.vel[1] = ms.vel[1]; mstate.vel[2] = ms.vel[2];
    mstate.grounded = ms.grounded;

    TFMoveInput minput;
    minput.moveX  = mx;
    minput.moveY  = my;
    minput.yaw    = ms.yaw;
    minput.jump   = (buttons & TFB_Jump) != 0;
    minput.sprint = (buttons & TFB_Sprint) != 0 && my > 0.0f;
    minput.crouch = (buttons & TFB_Crouch) != 0;

    TFMoveStep(mstate, minput, runSpeed, sprintSpeed, dt,
               [this](float x, float z)
               { return m_ctx->world ? m_ctx->world->TerrainHeightAt(x, z) : 0.0f; });

    ms.pos[0] = mstate.pos[0]; ms.pos[1] = mstate.pos[1]; ms.pos[2] = mstate.pos[2];
    ms.vel[0] = mstate.vel[0]; ms.vel[1] = mstate.vel[1]; ms.vel[2] = mstate.vel[2];
    ms.grounded = mstate.grounded;

    // --- hard speed cap (server-side validation backstop) ---
    const float hSpeed = std::sqrt(ms.vel[0] * ms.vel[0] + ms.vel[2] * ms.vel[2]);
    const float hardCap = sprintSpeed * kSpeedTolerance;
    if (hSpeed > hardCap && hSpeed > 0.0f)
    {
        const float scale = hardCap / hSpeed;
        ms.vel[0] *= scale;
        ms.vel[2] *= scale;
        ++m_speedClamps;
    }

    // --- world bounds [0, 4096] ---
    ms.pos[0] = std::clamp(ms.pos[0], kWorldMin, kWorldMax);
    ms.pos[2] = std::clamp(ms.pos[2], kWorldMin, kWorldMax);
}

void TFServerSim::WritePawnTransform(const MoveState& ms)
{
    // The pawn entity's Transform is the replicated truth.
    // Convention: Transform.position == FEET position, rotation.y == yaw (degrees).
    World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
    if (!world)
        return;
    const auto e = static_cast<EntityID>(ms.pawn);
    if (!world->GetRegistry().valid(e))
        return;
    if (Transform* t = world->GetComponent<Transform>(e))
    {
        t->position = {ms.pos[0], ms.pos[1], ms.pos[2]};
        t->rotation.y = ms.yaw * kRadToDeg;
    }
    if (TFPawnMoveComp* mv = world->GetComponent<TFPawnMoveComp>(e))
    {
        mv->vel[0] = ms.vel[0];
        mv->vel[1] = ms.vel[1];
        mv->vel[2] = ms.vel[2];
        mv->yaw = ms.yaw;
        mv->pitch = ms.pitch;
        mv->grounded = ms.grounded;
    }
}

void TFServerSim::RecordLagCompSnapshot()
{
    if (!m_ctx->players)
        return;

    std::vector<Spark::Net::RewindPose> poses;
    poses.reserve(m_move.size());
    m_ctx->players->ForEachAlivePawn([&poses](const auto& p) {
        Spark::Net::RewindPose pose{};
        pose.entityId = p.entity;
        pose.pos[0] = p.pos[0];
        pose.pos[1] = p.pos[1]; // feet; capsule spans [y, y + height]
        pose.pos[2] = p.pos[2];
        pose.radius = 0.4f;
        pose.height = 1.8f;
        poses.push_back(pose);
    });
    m_lagComp.RecordSnapshot(m_serverTime, poses);
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

void TFServerSim::OnPlayerSpawned(const EvPlayerSpawned& ev)
{
    if (!m_ctx->IsAuthority())
        return;

    MoveState ms;
    ms.pawn = ev.pawn;
    ms.cls = ev.cls;

    World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
    if (world)
    {
        const auto e = static_cast<EntityID>(ev.pawn);
        if (world->GetRegistry().valid(e))
        {
            if (const Transform* t = world->GetComponent<Transform>(e))
            {
                ms.pos[0] = t->position.x;
                ms.pos[1] = t->position.y;
                ms.pos[2] = t->position.z;
                ms.yaw = t->rotation.y / kRadToDeg;
            }
        }
    }

    m_move[ev.player] = ms;
    m_inputs[ev.player].clear();
    m_deathTime.erase(ev.player);
}

void TFServerSim::OnPlayerKilled(const EvPlayerKilled& ev)
{
    if (!m_ctx->IsAuthority())
        return;
    m_deathTime[ev.victim] = m_serverTime;
    m_move.erase(ev.victim);
    m_inputs.erase(ev.victim);
}

// ---------------------------------------------------------------------------
// Networking (server-side TFMsg routing)
// ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

void TFServerSim::RegisterNetHandlers()
{
    using Spark::Net::MessageType;
    using Spark::Net::NetworkMessage;
    auto& nm = Spark::Net::NetworkManager::GetInstance();

    auto route = [&nm](TFMsg id, auto&& fn) {
        nm.RegisterHandler(static_cast<MessageType>(static_cast<uint16_t>(id)),
                           std::forward<decltype(fn)>(fn));
    };

    route(TFMsg::ClientInput, [this](const NetworkMessage& m) {
        HandleClientInput(m.senderID, m.payload.data(), m.payload.size());
    });
    route(TFMsg::SpawnRequest, [this](const NetworkMessage& m) {
        HandleSpawnRequest(m.senderID, m.payload.data(), m.payload.size());
    });
    route(TFMsg::FireEvent, [this](const NetworkMessage& m) {
        HandleFireEvent(m.senderID, m.payload.data(), m.payload.size());
    });
    route(TFMsg::FactionSelect, [this](const NetworkMessage& m) {
        HandleFactionSelect(m.senderID, m.payload.data(), m.payload.size());
    });

    // Accepted-but-unrouted W1 stubs (registered so NetworkManager does not
    // warn "unknown message type" if an eager client sends them):
    // TF-W2: LoadoutChange -> players/weapons, SquadMsg -> squads, ChatMsg -> chat relay
    // TF-W3: VehicleEnter/VehicleExit -> vehicles, AegisDeploy -> vehicles
    for (TFMsg id : {TFMsg::LoadoutChange, TFMsg::SquadMsg, TFMsg::ChatMsg,
                     TFMsg::VehicleEnter, TFMsg::VehicleExit, TFMsg::AegisDeploy})
    {
        route(id, [](const NetworkMessage&) {});
    }

    m_handlersRegistered = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] server TFMsg handlers registered");
}

void TFServerSim::UnregisterNetHandlers()
{
    // NetworkManager has no per-type removal; replace our handlers with no-ops
    // so no dangling `this` survives module shutdown.
    using Spark::Net::MessageType;
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    for (TFMsg id : {TFMsg::ClientInput, TFMsg::SpawnRequest, TFMsg::FireEvent,
                     TFMsg::FactionSelect, TFMsg::LoadoutChange, TFMsg::SquadMsg,
                     TFMsg::ChatMsg, TFMsg::VehicleEnter, TFMsg::VehicleExit,
                     TFMsg::AegisDeploy})
    {
        nm.RegisterHandler(static_cast<MessageType>(static_cast<uint16_t>(id)),
                           [](const Spark::Net::NetworkMessage&) {});
    }
    m_handlersRegistered = false;
}

void TFServerSim::PollClientJoinsLeaves()
{
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    const auto& clients = nm.GetClients();

    for (const auto& [id, info] : clients)
    {
        if (info.state == Spark::Net::ConnectionState::Connected && !m_knownClients.contains(id))
        {
            m_knownClients.insert(id);
            SendWorldWelcome(id);
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] client %u joined — welcome sent", id);
        }
    }

    for (auto it = m_knownClients.begin(); it != m_knownClients.end();)
    {
        const PlayerId id = *it;
        if (!clients.contains(id))
        {
            auto mv = m_move.find(id);
            if (mv != m_move.end() && m_ctx->players)
                m_ctx->players->ServerKillPawn(mv->second.pawn, kInvalidPlayer, kInvalidWeapon, false);
            m_move.erase(id);
            m_inputs.erase(id);
            m_factions.erase(id);
            m_deathTime.erase(id);
            it = m_knownClients.erase(it);
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] client %u left — pawn cleaned up", id);
        }
        else
        {
            ++it;
        }
    }
}

void TFServerSim::SendToPlayer(PlayerId player, uint16_t msgId, const void* payload,
                               size_t size, bool reliable)
{
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    Spark::Net::NetworkMessage msg;
    msg.type = static_cast<Spark::Net::MessageType>(msgId);
    msg.channel = reliable ? Spark::Net::ChannelType::Reliable
                           : Spark::Net::ChannelType::Unreliable;
    msg.payload.resize(size);
    std::memcpy(msg.payload.data(), payload, size);
    nm.SendToClient(player, msg);
}

void TFServerSim::SendMoveStates()
{
    for (const auto& [player, ms] : m_move)
    {
        if (!m_knownClients.contains(player))
            continue; // e.g. listen-host local player is not a network client

        TF_MoveState st{};
        st.lastAckedSeq = ms.lastSeq;
        st.posX = ms.pos[0]; st.posY = ms.pos[1]; st.posZ = ms.pos[2];
        st.velX = ms.vel[0]; st.velY = ms.vel[1]; st.velZ = ms.vel[2];
        st.yaw = ms.yaw;
        st.pitch = ms.pitch;
        st.grounded = ms.grounded ? 1 : 0;
        SendToPlayer(player, kTFRepMsg_MoveState, &st, sizeof(st), false);
    }
}

void TFServerSim::HandleClientInput(PlayerId sender, const void* data, size_t size)
{
    if (size != sizeof(TF_ClientInput) || sender == Spark::Net::INVALID_CLIENT)
    {
        ++m_badPackets;
        return;
    }
    TF_ClientInput in;
    std::memcpy(&in, data, sizeof(in));
    EnqueueInput(sender, in);
}

void TFServerSim::HandleSpawnRequest(PlayerId sender, const void* data, size_t size)
{
    if (size != sizeof(TF_SpawnRequest) || sender == Spark::Net::INVALID_CLIENT)
    {
        ++m_badPackets;
        return;
    }
    TF_SpawnRequest req;
    std::memcpy(&req, data, sizeof(req));

    TF_SpawnReply rep{};
    rep.accepted = 0;
    rep.reason = 1; // bad-point until proven otherwise

    const FactionId faction = GetPlayerFaction(sender);

    if (faction == FactionId::None)
    {
        rep.reason = 1; // must FactionSelect first
    }
    else if (req.classId >= static_cast<uint8_t>(ClassId::COUNT) ||
             req.classId == static_cast<uint8_t>(ClassId::Colossus))
    {
        rep.reason = 4; // class-locked (Colossus is terminal-purchased, DESIGN §1)
    }
    else if (req.spawnKind != 0)
    {
        rep.reason = 1; // TF-W2: region spawns; TF-W3: aegis + squad-leader spawns
    }
    else if (m_move.contains(sender))
    {
        rep.reason = 1; // already alive
    }
    else if (auto dt = m_deathTime.find(sender);
             dt != m_deathTime.end() && m_serverTime < dt->second + kRespawnDelaySec)
    {
        rep.reason = 2;
        rep.respawnDelay = static_cast<float>(dt->second + kRespawnDelaySec - m_serverTime);
    }
    else if (m_ctx->data && m_ctx->players)
    {
        // W1: spawnKind==0 → faction skyanchor home region from regions.json
        const RegionDef* home = nullptr;
        for (const RegionDef& r : m_ctx->data->GetContinent().regions)
        {
            if (r.tier == "skyanchor" && r.homeFaction == faction)
            {
                home = &r;
                break;
            }
        }
        if (home)
        {
            const float x = home->spawns.empty() ? home->centerX : (*home->spawns.begin())[0];
            const float z = home->spawns.empty() ? home->centerZ : (*home->spawns.begin())[1];
            const float y = m_ctx->world ? m_ctx->world->TerrainHeightAt(x, z) : 0.0f;
            const float yaw = std::atan2(kWorldMax * 0.5f - x, kWorldMax * 0.5f - z); // face map center
            const float pos[3]{x, y, z};

            const EntityId ent = m_ctx->players->ServerSpawnPawn(
                sender, faction, static_cast<ClassId>(req.classId), pos, yaw);
            if (ent != 0)
            {
                // Seed movement immediately (EvPlayerSpawned will re-seed identically).
                MoveState ms;
                ms.pawn = ent;
                ms.cls = static_cast<ClassId>(req.classId);
                ms.pos[0] = x; ms.pos[1] = y; ms.pos[2] = z;
                ms.yaw = yaw;
                m_move[sender] = ms;
                m_deathTime.erase(sender);

                rep.accepted = 1;
                rep.reason = 0;
                rep.entityId = ent;
                rep.posX = x; rep.posY = y; rep.posZ = z;
            }
        }
    }

    SendSpawnReply(sender, rep);
}

void TFServerSim::HandleFireEvent(PlayerId sender, const void* data, size_t size)
{
    if (size != sizeof(TF_FireEvent) || sender == Spark::Net::INVALID_CLIENT)
    {
        ++m_badPackets;
        return;
    }
    if (!m_ctx->weapons)
        return;
    TF_FireEvent ev;
    std::memcpy(&ev, data, sizeof(ev));
    m_ctx->weapons->ServerHandleFire(sender, ev);
}

void TFServerSim::HandleFactionSelect(PlayerId sender, const void* data, size_t size)
{
    if (size != sizeof(TF_FactionSelect) || sender == Spark::Net::INVALID_CLIENT)
    {
        ++m_badPackets;
        return;
    }
    TF_FactionSelect sel;
    std::memcpy(&sel, data, sizeof(sel));

    if (m_move.contains(sender))
    {
        SPARK_LOG_WARN(Spark::LogCategory::Game,
                       "[TF] player %u tried to switch faction while alive — ignored", sender);
        return;
    }
    SetPlayerFaction(sender, static_cast<FactionId>(sel.faction));
}

void TFServerSim::SendSpawnReply(PlayerId player, const TF_SpawnReply& reply)
{
    SendToPlayer(player, static_cast<uint16_t>(TFMsg::SpawnReply), &reply, sizeof(reply), true);
}

void TFServerSim::SendWorldWelcome(PlayerId player)
{
    TF_WorldWelcome w{};
    w.yourPlayerId = player;
    w.yourFaction = static_cast<uint8_t>(GetPlayerFaction(player));
    if (m_ctx->data && m_ctx->data->IsLoaded())
    {
        const size_t n = m_ctx->data->GetContinent().regions.size();
        w.regionCount = static_cast<uint8_t>(std::min<size_t>(n, 255));
    }
    w.territoryHash = 0; // TF-W2: real hash from TFRegionSystem ownership state
    w.serverTimeMs = static_cast<uint32_t>(m_serverTime * 1000.0);
    SendToPlayer(player, static_cast<uint16_t>(TFMsg::WorldWelcome), &w, sizeof(w), true);
}

#endif // ENABLE_NETWORKING

// ---------------------------------------------------------------------------
// Debug UI
// ---------------------------------------------------------------------------

void TFServerSim::RenderDebugUI()
{
#ifdef ENABLE_EDITOR
    if (!m_showDebug)
        return;
    if (ImGui::Begin("TF Server Sim", &m_showDebug))
    {
        ImGui::Text("server time  : %.2f s (tick %llu)", m_serverTime,
                    static_cast<unsigned long long>(m_tickCount));
        ImGui::Text("area ticks   : %llu", static_cast<unsigned long long>(m_areaTickCount));
        ImGui::Text("clients      : %zu", m_knownClients.size());
        ImGui::Text("moving pawns : %zu", m_move.size());
        ImGui::Text("factions set : %zu", m_factions.size());
        ImGui::Text("speed clamps : %u   bad packets: %u", m_speedClamps, m_badPackets);
        ImGui::Separator();
        for (const auto& [pid, ms] : m_move)
        {
            ImGui::Text("p%u pawn=%u pos=(%.1f %.1f %.1f) seq=%u %s", pid, ms.pawn,
                        ms.pos[0], ms.pos[1], ms.pos[2], ms.lastSeq,
                        ms.grounded ? "ground" : "air");
        }
    }
    ImGui::End();
#endif // ENABLE_EDITOR
}

} // namespace Terrafront
