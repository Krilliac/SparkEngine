/**
 * @file TFServerSim.cpp
 * @brief Authoritative fixed-tick simulation core: lifecycle, frozen
 *        cross-system API, the authoritative tick driver, spawn/kill event
 *        handlers and the debug panel. Movement integration lives in
 *        TFServerSimMovement.cpp, session/socket plumbing in TFServerSimNet.cpp,
 *        gameplay TFMsg handlers in TFServerSimNetHandlers.cpp and the routing
 *        choke point + W5 onboarding in TFServerSimOnboarding.cpp (same class,
 *        split per repo file-size rules — mirrors the TFClientNet/
 *        TFClientNetHandlers split).
 */
#include "Net/TFServerSim.h"
#include "Net/TFServerSimConstants.h"

#include "Game/TFPlayerSystem.h"
#include "Game/TFServerValidation.h" // W13 anti-cheat lane: movement/fire-origin sanity (see file header)
#include "Game/TFSquadSystem.h"

#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"
#include "Physics/PhysicsSystem.h" // TF vehicle physics: single per-process step
#include "Utils/LogMacros.h"
#include "Utils/TFPerfCounters.h" // TF-W13 server-perf lane

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

namespace Terrafront
{

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
                    TFPerfCounters::ScopedTimer perfPhysics(TFPerfCounters::Phase::PhysicsStep);
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

        // W13 anti-cheat lane: input-rate budget -- TickMovement's
        // kMaxInputsPerTick replay only bounds how much backlog is DRAINED
        // per tick, not how fast the queue can be FILLED. Gate acceptance
        // here so a client flooding TF_ClientInput packets can't sustain a
        // permanent backlog (see TFServerValidation.h file header).
        if (!TFServerValidation::Get().AllowInput(player, m_serverTime))
            return;

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
        // W13 anti-cheat lane: this is an explicit server-authoritative
        // reposition (redeploy / tf_tp), not a validated walk — whitelist the
        // next TickMovement tick's displacement check for this player (see
        // TFServerValidation.h file header for why this is defense-in-depth
        // rather than load-bearing).
        TFServerValidation::Get().NoteExemptTeleport(player);
        ms.pos[0] = x;
        ms.pos[1] = y;
        ms.pos[2] = z;
        ms.vel[0] = ms.vel[1] = ms.vel[2] = 0.0f;
        ms.grounded = false; // settle onto the terrain on the next step
        WritePawnTransform(ms);
    }

    // TF-W13 server-perf lane: replication interest gate (see TFServerSim.h
    // for the contract). Reuses the exact same squad/distance shape as
    // HandleChatMsg's per-recipient TFChatRouteView below — proven pattern in
    // this file, just without the faction/region legs chat needs.
    bool TFServerSim::IsRelevantTo(PlayerId observer, PlayerId subject) const
    {
        if (observer == subject)
            return true;

        if (m_ctx->squads)
        {
            const SquadId a = m_ctx->squads->SquadOf(observer);
            const SquadId b = m_ctx->squads->SquadOf(subject);
            if (a != kInvalidSquad && a == b)
                return true; // squadmates always relevant regardless of distance
        }

        if (!m_ctx->players)
            return true; // fail open: no pawn registry to distance-check against

        PawnInfo observerPawn{};
        PawnInfo subjectPawn{};
        const bool haveObserver = m_ctx->players->GetPawnByPlayer(observer, observerPawn);
        const bool haveSubject = m_ctx->players->GetPawnByPlayer(subject, subjectPawn);
        if (!haveObserver || !haveSubject)
            return true; // fail open: e.g. subject not yet registered — let the Create through

        const float dx = observerPawn.pos[0] - subjectPawn.pos[0];
        const float dy = observerPawn.pos[1] - subjectPawn.pos[1];
        const float dz = observerPawn.pos[2] - subjectPawn.pos[2];
        return (dx * dx + dy * dy + dz * dz) <= (kInterestRadiusM * kInterestRadiusM);
    }

    // ---------------------------------------------------------------------------
    // Authoritative tick
    // ---------------------------------------------------------------------------

    void TFServerSim::TickAuthoritative(float fdt)
    {
        m_serverTime += fdt;
        ++m_tickCount;

        {
            TFPerfCounters::ScopedTimer perfMove(TFPerfCounters::Phase::Movement);
            TickMovement(fdt);
        }

#ifdef ENABLE_NETWORKING
        EnforceAntiCheatKicks();
#endif

        // TF-W13 server-perf lane: lag-comp snapshot + owner move-state send
        // prep is the replication-adjacent work TFServerSim itself performs.
        // The OTHER major replication-build cost — TFReplication's pawn
        // Create/Update quantize-and-diff pass and TFVehicleSystem's vehicle
        // Update/Aim broadcasts — lives in those files (not owned by this
        // lane); see wiringNotes for the one-line ScopedTimer drop-in there.
        {
            TFPerfCounters::ScopedTimer perfRep(TFPerfCounters::Phase::ReplicationBuild);
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
            // TF-W13 server-perf lane: same numbers `tf_perf` prints, inline for
            // the panel. See Utils/TFPerfCounters.h.
            {
                auto& perf = TFPerfCounters::Instance();
                ImGui::Text("perf: move %.3fms  phys %.3fms  rep %.3fms  budget %.3fms",
                            perf.AverageMs(TFPerfCounters::Phase::Movement),
                            perf.AverageMs(TFPerfCounters::Phase::PhysicsStep),
                            perf.AverageMs(TFPerfCounters::Phase::ReplicationBuild), 1000.0 / kServerTickHz);
            }
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
