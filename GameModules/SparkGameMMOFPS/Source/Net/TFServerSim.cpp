/**
 * @file TFServerSim.cpp
 * @brief Authoritative fixed-tick simulation: input consume, movement validate,
 *        lag-comp snapshots, TFMsg server-side routing (W1).
 */
#include "Net/TFServerSim.h"

#include "Account/TFAccountSystem.h"   // W5 onboarding (Task 4)
#include "Account/TFCharacterSystem.h" // W5 onboarding (Task 4)
#include "Persistence/TFDatabase.h"
#include "Net/TFClientNet.h" // W5 onboarding (Task 7): local-player reply loopback
#include "Net/TFChatRules.h"
#include "Data/TFDataTables.h"
#include "World/TFRegionSystem.h"
#include "World/TFWorldSetup.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h" // W5 onboarding (Task 6): disconnect progress flush
#include "Game/TFDirectiveSystem.h"   // W6 directives: disconnect progress sweep
#include "Game/TFVehicleSystem.h"     // TF-W3: seat routing + seated-pawn ride sync
#include "Game/TFWeaponSystem.h"
#include "Game/TFOutfitSystem.h"  // Outfits lane: OutfitRequest routing + session hooks
#include "Game/TFAbilitySystem.h" // class-abilities lane (W9): AbilityRequest routing
#include "Game/TFRedeployRules.h" // W7 ui-map-keys: redeploy validation
#include "Net/TFRedeployProtocol.h"

#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"
#include "Physics/PhysicsSystem.h" // TF vehicle physics: single per-process step
#include "Game/TFComponents.h"
#include "Game/TFMovementModel.h"
#include "Game/TFSquadSystem.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Terrafront
{

    namespace
    {

        // Engine-side movement limits. Balance numbers (run/sprint speed) come from
        // classes.json via TFDataTables; these are simulation constants.
        constexpr float kWorldMin = 0.0f;
        constexpr float kWorldMax = 4096.0f;
        constexpr float kPitchLimitRad = 1.55f;
        constexpr float kRespawnDelaySec = 8.0f; // DESIGN.md §4 default
        constexpr float kSpeedTolerance = 1.25f; // sprint * this == hard cap
        constexpr int kMaxInputsPerTick = 3;     // catch-up bound per fixed tick
        constexpr size_t kMaxQueuedInputs = 32;
        constexpr float kDefaultRunSpeed = 5.2f; // fallback if classes.json missing
        constexpr float kDefaultSprintSpeed = 7.2f;
        constexpr float kRadToDeg = 57.2957795f;

    } // namespace

    TFServerSim::TFServerSim() = default;
    TFServerSim::~TFServerSim()
    {
        if (m_initialized)
            Shutdown();
    }

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
        const bool serverUp =
            nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server && m_ctx->IsAuthority();
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
        {
            TickAuthoritative(fixedDeltaTime);

            // TF vehicle physics: the SINGLE per-process Jolt step (module-driven
            // stepping contract, Physics/PhysicsSystem.h). Runs after movement so
            // TFVehicleSystem::FixedUpdate — next in Main.cpp fixed-update order —
            // reads back a fresh hull pose and queues forces for the next step.
            // StepFixed no-ops (returns 0) when Jolt is unavailable. NEVER add a
            // second Update()/StepFixed() caller in this process (double-stepping).
            if (m_ctx->engine)
            {
                if (auto* physics = m_ctx->engine->GetPhysics())
                {
                    physics->SetTimeStep(fixedDeltaTime);
                    physics->StepFixed(1, 1.0f);
                }
            }
        }
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
        m_enteredWorld.clear();
        m_activeCharacter.clear();
        m_chatNextAt.clear();
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
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] Player %u joined %s", player, FactionName(faction));
    }

    uint64_t TFServerSim::ActiveCharacterOf(PlayerId player) const
    {
        auto it = m_activeCharacter.find(player);
        return it == m_activeCharacter.end() ? 0 : it->second;
    }

    void TFServerSim::TeleportPawn(PlayerId player, float x, float y, float z)
    {
        auto it = m_move.find(player);
        if (it == m_move.end())
            return;
        MoveState& ms = it->second;
        ms.pos[0] = x;
        ms.pos[1] = y;
        ms.pos[2] = z;
        ms.vel[0] = ms.vel[1] = ms.vel[2] = 0.0f;
        ms.grounded = false; // settle onto the terrain on the next step
        WritePawnTransform(ms);
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
            // TF-W3 (vehicles agent): seated pawns don't walk. Their inputs drive
            // the VEHICLE (driver throttle/steer; gunners aim/fire via the weapon
            // path) and the pawn rides at the seat position — SyncSeatedPawn pulls
            // that pose (or the one-shot exit placement) into MoveState so the
            // Transform truth, replication, lag comp and TF_MoveState all stay
            // coherent without touching the walking model.
            if (m_ctx->vehicles && m_ctx->vehicles->IsSeated(player))
            {
                if (auto vit = m_inputs.find(player); vit != m_inputs.end())
                {
                    auto& vq = vit->second;
                    int consumed = 0;
                    while (!vq.empty() && consumed < kMaxInputsPerTick)
                    {
                        const TF_ClientInput in = vq.front();
                        vq.pop_front();
                        m_ctx->vehicles->ServerHandleSeatedInput(player, in, fdt);
                        ms.lastSeq = in.seq;
                        // Sibling of StepPlayer's guard (a5f95e7d): reject non-finite
                        // view angles before WrapPi/clamp here too -- WrapPi's
                        // while-loop never terminates on +-Inf (subtracting a finite
                        // step from infinity stays infinite), and a NaN survives
                        // clamp unchanged, poisoning ms.yaw/ms.pitch for every future
                        // tick and every TF_MoveState broadcast to all connected
                        // clients. The seated/vehicle-occupant input path took client
                        // view angles straight into WrapPi/clamp with no check.
                        if (std::isfinite(in.viewYaw) && std::isfinite(in.viewPitch))
                        {
                            ms.yaw = QuantAim::WrapPi(in.viewYaw);
                            ms.pitch = std::clamp(in.viewPitch, -kPitchLimitRad, kPitchLimitRad);
                        }
                        else if (m_serverTime - m_lastViolationLog > 5.0)
                        {
                            m_lastViolationLog = m_serverTime;
                            SPARK_LOG_WARN(Spark::LogCategory::Game,
                                           "[TF] movement validation: rejected non-finite view angle (seated, seq=%u)",
                                           in.seq);
                        }
                        ++consumed;
                    }
                }
                m_ctx->vehicles->SyncSeatedPawn(player, ms.pos, ms.vel);
                ms.grounded = true;
                WritePawnTransform(ms);
                continue;
            }

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
                                   "[TF] movement validation: clamped over-unit move input (%u total)", m_speedClamps);
                }
            }
            // Reject non-finite view angles before they reach WrapPi/clamp: WrapPi's
            // while-loop never terminates on +-Inf (subtracting a finite step from
            // infinity stays infinite), and a NaN survives clamp unchanged, poisoning
            // ms.yaw/ms.pitch for every future tick and every TF_MoveState broadcast
            // to all connected clients.
            if (std::isfinite(in->viewYaw) && std::isfinite(in->viewPitch))
            {
                ms.yaw = QuantAim::WrapPi(in->viewYaw);
                ms.pitch = std::clamp(in->viewPitch, -kPitchLimitRad, kPitchLimitRad);
            }
            else if (m_serverTime - m_lastViolationLog > 5.0)
            {
                m_lastViolationLog = m_serverTime;
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "[TF] movement validation: rejected non-finite view angle (seq=%u)", in->seq);
            }
            buttons = in->buttons;
        }

        // --- shared TF movement model v1 (Game/TFMovementModel.h) ---
        // MUST stay byte-identical in behavior to the client's ClientPrediction
        // simulator (TFClientNet::SimulateMove); both call the same TFMoveStep.
        TFMoveState mstate;
        mstate.pos[0] = ms.pos[0];
        mstate.pos[1] = ms.pos[1];
        mstate.pos[2] = ms.pos[2];
        mstate.vel[0] = ms.vel[0];
        mstate.vel[1] = ms.vel[1];
        mstate.vel[2] = ms.vel[2];
        mstate.grounded = ms.grounded;

        TFMoveInput minput;
        minput.moveX = mx;
        minput.moveY = my;
        minput.yaw = ms.yaw;
        minput.jump = (buttons & TFB_Jump) != 0;
        minput.sprint = (buttons & TFB_Sprint) != 0 && my > 0.0f;
        minput.crouch = (buttons & TFB_Crouch) != 0;

        // class-abilities lane (W9): per-pawn ability movement modifiers
        // (Striker jets / Bulwark Field slow / Colossus lockdown root).
        // MUST stay the exact mirror of TFClientNet::SimulateMove's snippet
        // (prediction symmetry — both-or-neither).
        TFAbilityMoveMods abilityMods;
        if (m_ctx->abilities)
            abilityMods = m_ctx->abilities->MoveModsForPawn(ms.pawn);
        if (abilityMods.speedMult <= 0.0f)
            minput.jump = false; // rooted: no jump either

        TFMoveStep(mstate, minput, runSpeed * abilityMods.speedMult, sprintSpeed * abilityMods.speedMult, dt,
                   [this](float x, float z) { return m_ctx->world ? m_ctx->world->TerrainHeightAt(x, z) : 0.0f; });

        // 2026-07-10 collision wave: block/slide against the static scene
        // bodies + terrain re-clamp at the resolved column. MUST stay the
        // exact mirror of TFClientNet::SimulateMove (same shared resolver,
        // TFWorldSetup::ResolveMoveCollision) or prediction rubber-bands.
        if (m_ctx->world)
        {
            const float prevPos[3] = {ms.pos[0], ms.pos[1], ms.pos[2]};
            m_ctx->world->ResolveMoveCollision(prevPos, mstate.pos, mstate.vel, &mstate.grounded);
        }

        // class-abilities lane (W9): jet thrust after step+collision resolve —
        // same order as the client mirror (see Game/TFAbilitySystem.h).
        if (abilityMods.jetThrust)
            TFApplyJetThrust(mstate, dt);

        ms.pos[0] = mstate.pos[0];
        ms.pos[1] = mstate.pos[1];
        ms.pos[2] = mstate.pos[2];
        ms.vel[0] = mstate.vel[0];
        ms.vel[1] = mstate.vel[1];
        ms.vel[2] = mstate.vel[2];
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
        m_ctx->players->ForEachAlivePawn(
            [&poses](const auto& p)
            {
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

        auto route = [&nm](TFMsg id, auto&& fn)
        { nm.RegisterHandler(static_cast<MessageType>(static_cast<uint16_t>(id)), std::forward<decltype(fn)>(fn)); };

        // W5 T6 (T4-review #1 security fix): gameplay ids are now routed through
        // RouteClientMessage — the SAME single choke point the onboarding ids use
        // below and the listen-host/standalone loopback path uses
        // (TFClientNet::RouteLoopback) — so the enter-world gate added there
        // applies uniformly to every client-originated gameplay message,
        // regardless of transport.
        // final-review #3 (gate defense-in-depth): VehicleEnter/VehicleExit/
        // AegisDeploy/SquadMsg used to be direct routes straight into their
        // handlers below, bypassing the RouteClientMessage enter-world gate
        // entirely -- an unauthenticated/pre-enter-world client could seat a
        // vehicle, toggle Aegis, or spam squad ops. They now go through the same
        // choke point as the other gameplay ids.
        for (TFMsg id :
             {TFMsg::ClientInput, TFMsg::SpawnRequest, TFMsg::FireEvent, TFMsg::FactionSelect, TFMsg::VehicleEnter,
              TFMsg::VehicleExit, TFMsg::AegisDeploy, TFMsg::SquadMsg, TFMsg::RedeployRequest})
        {
            route(id, [this, id](const NetworkMessage& m)
                  { RouteClientMessage(m.senderID, id, m.payload.data(), m.payload.size()); });
        }

        // W6 progression: loadout persistence + unlock-tree purchases now route
        // through the same enter-world-gated choke point as the gameplay ids.
        for (TFMsg id : {TFMsg::LoadoutChange, TFMsg::UnlockRequest})
        {
            route(id, [this, id](const NetworkMessage& m)
                  { RouteClientMessage(m.senderID, id, m.payload.data(), m.payload.size()); });
        }

        // Outfits lane: enter-world-gated like the other gameplay ids.
        route(TFMsg::OutfitRequest, [this](const NetworkMessage& m)
              { RouteClientMessage(m.senderID, TFMsg::OutfitRequest, m.payload.data(), m.payload.size()); });

        // class-abilities lane (W9): enter-world-gated like the other gameplay ids.
        route(TFMsg::AbilityRequest, [this](const NetworkMessage& m)
              { RouteClientMessage(m.senderID, TFMsg::AbilityRequest, m.payload.data(), m.payload.size()); });

        route(TFMsg::ChatMsg, [this](const NetworkMessage& m)
              { RouteClientMessage(m.senderID, TFMsg::ChatMsg, m.payload.data(), m.payload.size()); });

        // W5 onboarding (Task 4): login -> char-select/create/delete -> enter-world.
        // Routed through RouteClientMessage so the socket path and the listen-host/
        // standalone loopback path (TFClientNet::RouteLoopback) share one dispatch.
        for (TFMsg id : {TFMsg::LoginRequest, TFMsg::RegisterRequest, TFMsg::CharListRequest, TFMsg::CharCreateReq,
                         TFMsg::CharDeleteReq, TFMsg::EnterWorldReq})
        {
            route(id, [this, id](const NetworkMessage& m)
                  { RouteClientMessage(m.senderID, id, m.payload.data(), m.payload.size()); });
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
        for (TFMsg id : {TFMsg::ClientInput,     TFMsg::SpawnRequest,    TFMsg::FireEvent,     TFMsg::FactionSelect,
                         TFMsg::LoadoutChange,   TFMsg::UnlockRequest,   TFMsg::SquadMsg,      TFMsg::ChatMsg,
                         TFMsg::VehicleEnter,    TFMsg::VehicleExit,     TFMsg::AegisDeploy,   TFMsg::LoginRequest,
                         TFMsg::RegisterRequest, TFMsg::CharListRequest, TFMsg::CharCreateReq, TFMsg::CharDeleteReq,
                         TFMsg::EnterWorldReq,   TFMsg::RedeployRequest, TFMsg::OutfitRequest, TFMsg::AbilityRequest})
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
                // W5 onboarding gate: TF_WorldWelcome is NO LONGER sent on connect.
                // It is now sent only from HandleEnterWorld, after a successful
                // login + character enter-world. See DESIGN.md W5.
                m_knownClients.insert(id);
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] client %u joined — awaiting login/enter-world", id);
            }
        }

        for (auto it = m_knownClients.begin(); it != m_knownClients.end();)
        {
            const PlayerId id = *it;
            const auto clientIt = clients.find(id);
            if (clientIt == clients.end() || clientIt->second.state != Spark::Net::ConnectionState::Connected)
            {
                CleanupPlayerSession(id);
                it = m_knownClients.erase(it);
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] client %u left — pawn cleaned up", id);
            }
            else
            {
                ++it;
            }
        }
    }

    void TFServerSim::CleanupPlayerSession(PlayerId id)
    {
        auto mv = m_move.find(id);
        if (mv != m_move.end() && m_ctx->players)
            m_ctx->players->ServerKillPawn(mv->second.pawn, kInvalidPlayer, kInvalidWeapon, false);
        m_move.erase(id);
        m_inputs.erase(id);
        m_factions.erase(id);
        m_deathTime.erase(id);
        m_enteredWorld.erase(id);
        m_chatNextAt.erase(id);
        m_redeployNextAt.erase(id); // W7 ui-map-keys
        // W5 onboarding (Task 6): flush the active character's progress
        // one last time before dropping the session (DESIGN.md W5 "Error
        // handling": "On disconnect, flush the active character's
        // progress") — the periodic TFProgressionSystem::SaveNow debounce
        // could otherwise miss a few seconds of the final session.
        if (auto cIt = m_activeCharacter.find(id); cIt != m_activeCharacter.end())
        {
            if (m_ctx->characters && m_ctx->progression)
                m_ctx->characters->PersistProgress(cIt->second, m_ctx->progression->XPOf(id),
                                                   m_ctx->progression->RankOf(id), m_ctx->progression->FluxOf(id));
            m_activeCharacter.erase(cIt);
        }
        // final-review #2 (leak): drop the runtime progression record for
        // this PlayerId AFTER the final flush-to-character above. Without
        // this, a recycled PlayerId (a new client reusing a freed slot)
        // would inherit the prior occupant's xp/rank/flux and leak them
        // onto a different account's character.
        if (m_ctx->progression)
            m_ctx->progression->ClearPlayer(id);
        // W6 directives: same recycled-PlayerId hygiene for directive progress.
        if (m_ctx->directives)
            m_ctx->directives->ClearPlayer(id);
        // Outfits lane: drop the player->charId binding + roster/tag interest.
        if (m_ctx->outfits)
            m_ctx->outfits->ServerOnPlayerLeft(id);
        if (m_ctx->account)
            m_ctx->account->ClearSession(id);
    }

    void TFServerSim::DebugSimulateDisconnect(PlayerId player)
    {
        // Test-only hook (final-review #1/#2 regression proof): the listen-host/
        // standalone loopback player never appears in m_knownClients (see
        // SendToPlayer above), so PollClientJoinsLeaves's real-socket-drop
        // detection never fires for it. Run the identical cleanup directly so
        // tf_selftest_onboarding can prove a disconnect -> re-login -> re-enter
        // round-trip without tearing down the whole NetworkManager/session.
        CleanupPlayerSession(player);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] simulated disconnect cleanup for player %u (test hook)", player);
    }

    void TFServerSim::SendToPlayer(PlayerId player, uint16_t msgId, const void* payload, size_t size, bool reliable)
    {
        // W5 T7 (acceptance-harness fix): the listen-host/standalone local player
        // (m_ctx->localPlayer == kTFLocalHostPlayer) never establishes a real
        // NetworkManager socket -- TFClientNet::RouteLoopback bypasses the socket
        // entirely for the C->S direction, calling RouteClientMessage directly --
        // so below, nm.SendToClient() would look this id up in m_clientAddresses,
        // find nothing, and silently drop the reply (see NetworkConnection.cpp
        // SendToClient). Every onboarding client-state transition (logged-in,
        // character list, in-world) is driven purely by these S->C replies; there
        // is no ECS ground truth for "logged in"/"in world" the local player
        // could read directly the way movement/spawn state is (TFClientNet reads
        // the authoritative Transform for those). That silent drop broke the
        // whole login->world flow for local/standalone play. Mirror the reply
        // straight into the in-process client for that one player instead.
        if (player == m_ctx->localPlayer && m_ctx->clientNet && !m_knownClients.contains(player))
        {
            m_ctx->clientNet->DeliverLoopbackReply(static_cast<TFMsg>(msgId), payload, size);
            return;
        }

        auto& nm = Spark::Net::NetworkManager::GetInstance();
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(msgId);
        msg.channel = reliable ? Spark::Net::ChannelType::Reliable : Spark::Net::ChannelType::Unreliable;
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
            st.posX = ms.pos[0];
            st.posY = ms.pos[1];
            st.posZ = ms.pos[2];
            st.velX = ms.vel[0];
            st.velY = ms.vel[1];
            st.velZ = ms.vel[2];
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

        // TF-W3 (vehicles agent): Aegis mobile-spawn requests (spawnKind==2) are
        // validated end-to-end by TFPlayerSystem (owns respawn records + the
        // GetAegisSpawnPos check) which also sends the TF_SpawnReply; the pawn it
        // spawns re-enters this sim via EvPlayerSpawned exactly like any other.
        // Region (1), Aegis (2) and squad-leader (3) spawns are validated
        // end-to-end by TFPlayerSystem (owns respawn records + point checks).
        if ((req.spawnKind == 1 || req.spawnKind == 2 || req.spawnKind == 3) && m_ctx->players)
        {
            m_ctx->players->ServerHandleSpawnRequest(sender, req);
            return;
        }

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

                const EntityId ent =
                    m_ctx->players->ServerSpawnPawn(sender, faction, static_cast<ClassId>(req.classId), pos, yaw);
                if (ent != 0)
                {
                    // Seed movement immediately (EvPlayerSpawned will re-seed identically).
                    MoveState ms;
                    ms.pawn = ent;
                    ms.cls = static_cast<ClassId>(req.classId);
                    ms.pos[0] = x;
                    ms.pos[1] = y;
                    ms.pos[2] = z;
                    ms.yaw = yaw;
                    m_move[sender] = ms;
                    m_deathTime.erase(sender);

                    rep.accepted = 1;
                    rep.reason = 0;
                    rep.entityId = ent;
                    rep.posX = x;
                    rep.posY = y;
                    rep.posZ = z;
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

    // TF-W3 (vehicles agent): thin size-validated routes into TFVehicleSystem.
    void TFServerSim::HandleVehicleSeatOp(PlayerId sender, const void* data, size_t size, bool enter)
    {
        if (size != sizeof(TF_VehicleSeatOp) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        if (!m_ctx->vehicles)
            return;
        TF_VehicleSeatOp op;
        std::memcpy(&op, data, sizeof(op));
        m_ctx->vehicles->ServerHandleSeatOp(sender, op, enter);
    }

    void TFServerSim::HandleAegisDeploy(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_AegisDeploy) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        if (!m_ctx->vehicles)
            return;
        TF_AegisDeploy msg;
        std::memcpy(&msg, data, sizeof(msg));
        m_ctx->vehicles->ServerHandleAegisDeploy(sender, msg);
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
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] player %u tried to switch faction while alive — ignored",
                           sender);
            return;
        }
        SetPlayerFaction(sender, static_cast<FactionId>(sel.faction));
    }

    // W7 ui-map-keys: alive-pawn redeploy — server-authoritative teleport to a
    // friendly, non-contested, lattice-linked region's spawn point. Countdown
    // is client UX only; the anti-abuse control here is the per-player interval.
    void TFServerSim::HandleRedeployRequest(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_RedeployRequest) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_RedeployRequest rq;
        std::memcpy(&rq, data, sizeof(rq));

        TF_RedeployReply rep{};
        rep.regionId = rq.regionId;

        const FactionId faction = GetPlayerFaction(sender);
        rep.reason = TFRedeployRules::CanRedeploy(*m_ctx, sender, faction, rq.regionId);

        if (rep.reason == kTFRedeployOk)
        {
            if (auto it = m_redeployNextAt.find(sender); it != m_redeployNextAt.end() && m_serverTime < it->second)
            {
                rep.reason = kTFRedeployCooldown;
                rep.cooldownSec = static_cast<float>(it->second - m_serverTime);
            }
        }

        float pos[3]{};
        float yaw = 0.0f;
        if (rep.reason == kTFRedeployOk && !TFRedeployRules::ResolveSpawnPos(*m_ctx, rq.regionId, faction, pos, yaw))
            rep.reason = kTFRedeployBadRegion;

        if (rep.reason == kTFRedeployOk)
        {
            TeleportPawn(sender, pos[0], pos[1], pos[2]);
            m_redeployNextAt[sender] = m_serverTime + TFRedeployRules::kTFRedeployIntervalSec;
            rep.accepted = 1;
            rep.posX = pos[0];
            rep.posY = pos[1];
            rep.posZ = pos[2];
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] redeploy: p%u -> region %u", sender,
                           static_cast<unsigned>(rq.regionId));
        }
        SendToPlayer(sender, static_cast<uint16_t>(TFMsg::RedeployReply), &rep, sizeof(rep), true);
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
        w.territoryHash = m_ctx->regions ? m_ctx->regions->TerritoryHash() : 0;
        w.serverTimeMs = static_cast<uint32_t>(m_serverTime * 1000.0);
        SendToPlayer(player, static_cast<uint16_t>(TFMsg::WorldWelcome), &w, sizeof(w), true);
    }

    // ---------------------------------------------------------------------------
    // W5 onboarding (Task 4): login / register / char CRUD / enter-world.
    // ---------------------------------------------------------------------------

    void TFServerSim::RouteClientMessage(PlayerId sender, TFMsg id, const void* data, size_t size)
    {
        // W5 T6 (T4-review #1 security fix): CRITICAL security gate. Before this
        // fix, the enter-world gate only withheld TF_WorldWelcome — the gameplay
        // handlers themselves never verified the sender had actually logged in
        // and entered the world, so a modified client could send SpawnRequest/
        // ClientInput/FireEvent/FactionSelect directly and play as an
        // unauthenticated "ghost". Every client-originated gameplay message now
        // requires `sender` to already be in m_enteredWorld (set exactly once, by
        // a successful HandleEnterWorld below) — this is the SAME dispatcher both
        // the socket path (RegisterNetHandlers) and the listen-host/standalone
        // loopback path (TFClientNet::RouteLoopback) call through, so local play
        // is gated identically to networked play: the local host player
        // (kTFLocalHostPlayer) must complete login -> character select/create ->
        // enter-world via TFLoginFlow exactly like a networked client before it
        // can move, spawn, fire, or switch factions. Bot-driven spawns/inputs
        // (TFBotSystem) never call through here — they call
        // TFPlayerSystem::ServerHandleSpawnRequest / EnqueueInput /
        // TFWeaponSystem::ServerHandleFire directly — so bots are unaffected.
        // Onboarding ids (login/char CRUD/enter-world itself) are never gated
        // here: they are how a session GETS into m_enteredWorld in the first
        // place.
        switch (id)
        {
        case TFMsg::ClientInput:
        case TFMsg::SpawnRequest:
        case TFMsg::FireEvent:
        case TFMsg::FactionSelect:
        case TFMsg::VehicleEnter:
        case TFMsg::VehicleExit:
        case TFMsg::AegisDeploy:
        case TFMsg::SquadMsg:
        case TFMsg::ChatMsg:
        case TFMsg::LoadoutChange:
        case TFMsg::UnlockRequest:
        case TFMsg::RedeployRequest: // W7 ui-map-keys: MUST be gated
        case TFMsg::OutfitRequest:   // Outfits lane: gated like the other gameplay ids
        case TFMsg::AbilityRequest:  // class-abilities lane (W9): gated
            if (!m_enteredWorld.contains(sender))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "[TF] gameplay message 0x%04X from non-entered-world client %u rejected",
                               static_cast<unsigned>(id), sender);
                return;
            }
            break;
        default:
            break;
        }

        switch (id)
        {
        case TFMsg::LoginRequest:
            HandleLogin(sender, data, size);
            break;
        case TFMsg::RegisterRequest:
            HandleRegister(sender, data, size);
            break;
        case TFMsg::CharListRequest:
            HandleCharList(sender, data, size);
            break;
        case TFMsg::CharCreateReq:
            HandleCharCreate(sender, data, size);
            break;
        case TFMsg::CharDeleteReq:
            HandleCharDelete(sender, data, size);
            break;
        case TFMsg::EnterWorldReq:
            HandleEnterWorld(sender, data, size);
            break;
        case TFMsg::ClientInput:
            HandleClientInput(sender, data, size);
            break;
        case TFMsg::SpawnRequest:
            HandleSpawnRequest(sender, data, size);
            break;
        case TFMsg::FireEvent:
            HandleFireEvent(sender, data, size);
            break;
        case TFMsg::FactionSelect:
            HandleFactionSelect(sender, data, size);
            break;
        // W7 ui-map-keys: server-validated map redeploy.
        case TFMsg::RedeployRequest:
            HandleRedeployRequest(sender, data, size);
            break;
        // Outfits lane: TFOutfitSystem size-validates, applies rank policy and replies.
        case TFMsg::OutfitRequest:
            if (m_ctx->outfits)
                m_ctx->outfits->ServerHandleOutfitMsgRaw(sender, data, size);
            break;
        // class-abilities lane (W9): TFAbilitySystem size-validates and replies.
        case TFMsg::AbilityRequest:
            if (m_ctx->abilities)
                m_ctx->abilities->ServerHandleAbilityMsgRaw(sender, data, size);
            break;
        // final-review #3: vehicle/squad verbs, now gated the same as the
        // other gameplay ids above.
        case TFMsg::VehicleEnter:
            HandleVehicleSeatOp(sender, data, size, true);
            break;
        case TFMsg::VehicleExit:
            HandleVehicleSeatOp(sender, data, size, false);
            break;
        case TFMsg::AegisDeploy:
            HandleAegisDeploy(sender, data, size);
            break;
        case TFMsg::SquadMsg:
            if (m_ctx->squads)
                m_ctx->squads->ServerHandleSquadMsgRaw(sender, data, size);
            break;
        case TFMsg::ChatMsg:
            HandleChatMsg(sender, data, size);
            break;
        // W6 progression: persist the player's saved loadout preference
        // (validated inside ServerSetLoadout; false == rejected, no change).
        case TFMsg::LoadoutChange:
        {
            if (size < sizeof(TF_LoadoutChange) || !m_ctx->progression || !m_ctx->data || !m_ctx->data->IsLoaded())
                break;
            TF_LoadoutChange lc{};
            std::memcpy(&lc, data, sizeof(lc));
            auto keyOf = [&](uint16_t wid) -> std::string
            {
                if (wid == kInvalidWeapon)
                    return {};
                const WeaponDef* def = m_ctx->data->GetWeapon(static_cast<WeaponId>(wid));
                return def ? def->key : std::string{};
            };
            TFLoadout lo;
            lo.primary = keyOf(lc.primary);
            lo.secondary = keyOf(lc.secondary);
            lo.tool = keyOf(lc.tool);
            m_ctx->progression->ServerSetLoadout(sender, lo);
            break;
        }
        // W6 progression: unlock-tree purchase (Persistence/TFUnlockTree.h).
        case TFMsg::UnlockRequest:
        {
            if (size < sizeof(TF_UnlockRequest) || !m_ctx->progression)
                break;
            TF_UnlockRequest req{};
            std::memcpy(&req, data, sizeof(req));
            req.unlockKey[sizeof(req.unlockKey) - 1] = '\0';
            const TFUnlockResult r = m_ctx->progression->ServerTryUnlock(sender, req.unlockKey);
            TF_UnlockReply rep{};
            rep.result = static_cast<uint8_t>(r);
            std::memcpy(rep.unlockKey, req.unlockKey, sizeof(rep.unlockKey));
            SendToPlayer(sender, static_cast<uint16_t>(TFMsg::UnlockReply), &rep, sizeof(rep), true);
            break;
        }
        default:
            break;
        }
    }

    RegionId TFServerSim::RegionOfPlayer(PlayerId player) const
    {
        if (!m_ctx || !m_ctx->players || !m_ctx->data || !m_ctx->data->IsLoaded())
            return kInvalidRegion;
        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(player, pawn))
            return kInvalidRegion;

        RegionId nearest = kInvalidRegion;
        float bestSq = std::numeric_limits<float>::max();
        for (const RegionDef& region : m_ctx->data->GetContinent().regions)
        {
            const float dx = pawn.pos[0] - region.centerX;
            const float dz = pawn.pos[2] - region.centerZ;
            const float d2 = dx * dx + dz * dz;
            if (d2 < bestSq)
            {
                bestSq = d2;
                nearest = region.id;
            }
        }
        return nearest;
    }

    void TFServerSim::HandleChatMsg(PlayerId sender, const void* data, size_t size)
    {
        if (data == nullptr || size != sizeof(TF_ChatMsg) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }

        TF_ChatMsg incoming{};
        std::memcpy(&incoming, data, sizeof(incoming));
        if (!IsValidChatChannel(incoming.channel))
        {
            ++m_badPackets;
            return;
        }

        const double nextAllowed = m_chatNextAt[sender];
        if (m_serverTime < nextAllowed)
            return;
        // Consume the cooldown before text normalization so empty/control-only
        // packets cannot bypass the limiter and hammer the server parser.
        m_chatNextAt[sender] = m_serverTime + 0.5;

        TF_ChatMsg outgoing{};
        outgoing.fromPlayer = sender;
        outgoing.channel = incoming.channel;
        if (!NormalizeChatText(incoming.text, sizeof(incoming.text), outgoing.text, sizeof(outgoing.text)))
            return;

        const auto channel = static_cast<ChatChannel>(outgoing.channel);
        const FactionId senderFaction = GetPlayerFaction(sender);
        const SquadId senderSquad = m_ctx->squads ? m_ctx->squads->SquadOf(sender) : kInvalidSquad;
        const RegionId senderRegion = RegionOfPlayer(sender);
        // W8 ui-polish: outfit channel membership (authority registry truth).
        const uint32_t senderOutfit = m_ctx->outfits ? m_ctx->outfits->OutfitIdOf(sender) : 0u;
        PawnInfo senderPawn{};
        const bool senderHasPawn = m_ctx->players && m_ctx->players->GetPawnByPlayer(sender, senderPawn);

        size_t recipients = 0;
        for (const PlayerId recipient : m_enteredWorld)
        {
            PawnInfo recipientPawn{};
            const bool recipientHasPawn = m_ctx->players && m_ctx->players->GetPawnByPlayer(recipient, recipientPawn);
            float distanceSq = 0.0f;
            if (senderHasPawn && recipientHasPawn)
            {
                const float dx = senderPawn.pos[0] - recipientPawn.pos[0];
                const float dy = senderPawn.pos[1] - recipientPawn.pos[1];
                const float dz = senderPawn.pos[2] - recipientPawn.pos[2];
                distanceSq = dx * dx + dy * dy + dz * dz;
            }

            const FactionId recipientFaction = GetPlayerFaction(recipient);
            const SquadId recipientSquad = m_ctx->squads ? m_ctx->squads->SquadOf(recipient) : kInvalidSquad;
            const RegionId recipientRegion = RegionOfPlayer(recipient);
            const TFChatRouteView view{
                recipient == sender,
                senderRegion != kInvalidRegion && recipientRegion == senderRegion,
                senderFaction != FactionId::None && recipientFaction == senderFaction,
                senderSquad != kInvalidSquad && recipientSquad == senderSquad,
                senderHasPawn && recipientHasPawn,
                distanceSq,
                senderOutfit != 0u && m_ctx->outfits->OutfitIdOf(recipient) == senderOutfit,
            };
            if (!ShouldReceiveChat(channel, view))
                continue;
            SendToPlayer(recipient, static_cast<uint16_t>(TFMsg::ChatMsg), &outgoing, sizeof(outgoing), true);
            ++recipients;
        }

        SPARK_LOG_TRACE(Spark::LogCategory::Game, "[TF] chat %s from p%u routed to %zu player(s)",
                        ChatChannelName(channel), sender, recipients);
    }

    void TFServerSim::HandleLogin(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_AuthRequest) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_AuthRequest req;
        std::memcpy(&req, data, sizeof(req));
        const std::string user(req.user, strnlen(req.user, sizeof(req.user)));
        const std::string pass(req.pass, strnlen(req.pass, sizeof(req.pass)));

        TF_AuthReply rep{};
        if (!m_ctx->account || !EnsureAuthorityDatabaseOpen())
        {
            rep.err = static_cast<uint8_t>(TFAuthErr::ServerError);
        }
        else
        {
            const TFAuthResult r = m_ctx->account->Login(user, pass);
            rep.ok = r.ok ? 1 : 0;
            rep.err = static_cast<uint8_t>(r.err);
            rep.accountId = r.accountId;
            if (r.ok)
                m_ctx->account->BindSession(sender, r.accountId);
        }
        SendToPlayer(sender, static_cast<uint16_t>(TFMsg::LoginReply), &rep, sizeof(rep), true);
    }

    void TFServerSim::HandleRegister(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_AuthRequest) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_AuthRequest req;
        std::memcpy(&req, data, sizeof(req));
        const std::string user(req.user, strnlen(req.user, sizeof(req.user)));
        const std::string pass(req.pass, strnlen(req.pass, sizeof(req.pass)));

        TF_AuthReply rep{};
        if (!m_ctx->account || !EnsureAuthorityDatabaseOpen())
        {
            rep.err = static_cast<uint8_t>(TFAuthErr::ServerError);
        }
        else
        {
            const TFAuthResult r = m_ctx->account->Register(user, pass);
            rep.ok = r.ok ? 1 : 0;
            rep.err = static_cast<uint8_t>(r.err);
            rep.accountId = r.accountId;
            // Registration does NOT auto-login; the client sends LoginRequest next
            // (mirrors MMOAccountSystem's register-then-login flow).
        }
        SendToPlayer(sender, static_cast<uint16_t>(TFMsg::RegisterReply), &rep, sizeof(rep), true);
    }

    bool TFServerSim::EnsureAuthorityDatabaseOpen()
    {
        if (!m_ctx || !m_ctx->IsAuthority() || !m_ctx->db)
            return false;
        if (m_ctx->db->IsOpen())
            return true;
        if (m_ctx->db->Open("Saves/terrafront.db"))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] authority opened account database");
            return true;
        }
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] authority failed to open Saves/terrafront.db");
        return false;
    }

    void TFServerSim::HandleCharList(PlayerId sender, const void* data, size_t size)
    {
        (void)data;
        if (size != 0 || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_CharListReply rep{};
        if (m_ctx->account && m_ctx->characters)
        {
            const uint64_t acctId = m_ctx->account->AccountForClient(sender);
            if (acctId != 0)
            {
                const std::vector<TFCharacterRecord> list = m_ctx->characters->List(acctId);
                const uint8_t n = static_cast<uint8_t>(std::min<size_t>(list.size(), 5));
                rep.count = n;
                for (uint8_t i = 0; i < n; ++i)
                {
                    TF_CharBrief& b = rep.chars[i];
                    b.id = list[i].id;
                    std::strncpy(b.name, list[i].name.c_str(), sizeof(b.name) - 1);
                    b.name[sizeof(b.name) - 1] = '\0';
                    b.faction = static_cast<uint8_t>(list[i].faction);
                    b.rank = list[i].rank;
                }
            }
        }
        SendToPlayer(sender, static_cast<uint16_t>(TFMsg::CharListReply), &rep, sizeof(rep), true);
    }

    void TFServerSim::HandleCharCreate(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_CharCreateRequest) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_CharCreateRequest req;
        std::memcpy(&req, data, sizeof(req));
        const std::string name(req.name, strnlen(req.name, sizeof(req.name)));

        TF_CharOpReply rep{};
        if (!m_ctx->account || !m_ctx->characters)
        {
            rep.err = static_cast<uint8_t>(TFCharErr::ServerError);
        }
        else
        {
            const uint64_t acctId = m_ctx->account->AccountForClient(sender);
            if (acctId == 0)
            {
                rep.err = static_cast<uint8_t>(TFCharErr::NotLoggedIn);
            }
            else
            {
                const TFCharCreateResult r =
                    m_ctx->characters->Create(acctId, name, static_cast<FactionId>(req.faction));
                rep.ok = r.ok ? 1 : 0;
                rep.err = static_cast<uint8_t>(r.err);
                rep.charId = r.charId;
            }
        }
        SendToPlayer(sender, static_cast<uint16_t>(TFMsg::CharCreateReply), &rep, sizeof(rep), true);
    }

    void TFServerSim::HandleCharDelete(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_CharDeleteRequest) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_CharDeleteRequest req;
        std::memcpy(&req, data, sizeof(req));

        TF_CharOpReply rep{};
        rep.charId = req.charId;
        if (!m_ctx->account || !m_ctx->characters)
        {
            rep.err = static_cast<uint8_t>(TFCharErr::ServerError);
        }
        else
        {
            const uint64_t acctId = m_ctx->account->AccountForClient(sender);
            if (acctId == 0)
            {
                rep.err = static_cast<uint8_t>(TFCharErr::NotLoggedIn);
            }
            else
            {
                const TFCharErr err = m_ctx->characters->Delete(acctId, req.charId);
                rep.ok = (err == TFCharErr::Ok) ? 1 : 0;
                rep.err = static_cast<uint8_t>(err);
            }
        }
        SendToPlayer(sender, static_cast<uint16_t>(TFMsg::CharDeleteReply), &rep, sizeof(rep), true);
    }

    void TFServerSim::HandleEnterWorld(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_EnterWorldRequest) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_EnterWorldRequest req;
        std::memcpy(&req, data, sizeof(req));

        // Idempotency guard (mirrors HandleFactionSelect's alive-guard): a
        // duplicate EnterWorldReq, or one sent while a second character is being
        // entered mid-session, must NOT re-bind the session's faction out from
        // under an already-alive/already-entered pawn (DESIGN.md W5 "Error
        // handling": duplicate/already-in-world is idempotent, server ignores).
        if (m_move.contains(sender) || m_enteredWorld.contains(sender))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] player %u sent duplicate/late EnterWorldReq while already in world — ignored", sender);
            return;
        }

        // Server-authoritative gate: the client cannot self-report auth/enter-world
        // state. Without a bound, logged-in session AND ownership-verified
        // character, TF_WorldWelcome is never sent — the client stays parked on
        // the login/char-select screen (fails silently; no reply message exists
        // for a rejected EnterWorldReq by design, mirroring how an unauthenticated
        // socket gets no gameplay traffic at all).
        if (!m_ctx->account || !m_ctx->characters)
            return; // T6 boot wiring not present yet — cannot authoritatively enter world

        const uint64_t acctId = m_ctx->account->AccountForClient(sender);
        if (acctId == 0)
            return; // not logged in

        TFCharacterRecord rec;
        if (!m_ctx->characters->EnterWorld(acctId, req.charId, rec))
            return; // unknown character or not owned by this account

        SetPlayerFaction(sender, rec.faction);
        m_enteredWorld.insert(sender);
        m_activeCharacter[sender] = rec.id; // W5 onboarding (Task 6): progression re-key target
        // final-review #1 (data loss): seed this session's runtime progression from
        // the durable character record BEFORE any spawn/save can run. Without this,
        // the runtime record starts at the OnPlayerSpawned default (rank 1/0 xp/0
        // flux) and the next SaveNow/disconnect-flush overwrites the durable record
        // back to that default -- silent data loss for a returning character.
        if (m_ctx->progression)
            m_ctx->progression->ServerLoadCharacter(sender, rec.xp, rec.rank, rec.flux);
        // Outfits lane: bind player->character so tag/roster delivery happens at
        // enter-world instead of waiting for the first-spawn fallback.
        if (m_ctx->outfits)
            m_ctx->outfits->ServerOnCharacterEntered(sender, rec.id, rec.name);
        SendWorldWelcome(sender);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u entered world as character %llu (%s)", sender,
                       static_cast<unsigned long long>(rec.id), FactionName(rec.faction));
    }

#endif // ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFServerSim::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
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
                ImGui::Text("p%u pawn=%u pos=(%.1f %.1f %.1f) seq=%u %s", pid, ms.pawn, ms.pos[0], ms.pos[1], ms.pos[2],
                            ms.lastSeq, ms.grounded ? "ground" : "air");
            }
        }
        ImGui::End();
#endif // SPARK_HAS_IMGUI
    }

} // namespace Terrafront
