/**
 * @file TFServerSim.h
 * @brief Authoritative fixed-tick simulation (movement validate, hits, capture).
 *
 * OWNERSHIP: this header + TFServerSim.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 *
 * W1 implementation:
 *  - Registers all server-side TFMsg handlers with NetworkManager (lazily, once
 *    the NetworkManager role becomes Server) and routes them:
 *      ClientInput   -> EnqueueInput
 *      SpawnRequest  -> validate + ctx.players->ServerSpawnPawn + TF_SpawnReply
 *      FireEvent     -> ctx.weapons->ServerHandleFire
 *      FactionSelect -> SetPlayerFaction
 *  - 60 Hz fixed tick: per-player input queues -> accel/friction ground movement
 *    (classes.json speeds), gravity/jump/sprint, terrain clamp
 *    (ctx.world->TerrainHeightAt) and world bounds [0,4096]; speed-cap
 *    validation (clamp + throttled log, never kick).
 *  - Writes results into the pawn entity's Transform (the replicated truth;
 *    Transform.position = FEET position, rotation.y = yaw in degrees).
 *  - After each tick RecordSnapshot()s all alive pawns into LagComp() as
 *    capsules (pose.pos = feet, radius 0.4, height 1.8) and advances
 *    ServerTime() by fixedDt.
 *  - Sends TF_MoveState (TFRepProtocol.h) to each connected owner at 20 Hz —
 *    this carries the acked input seq for ClientPrediction reconciliation.
 *  - Sends TF_WorldWelcome on client join (join detection = polling
 *    NetworkManager::GetClients() diff each tick).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFRepProtocol.h"

#include "Engine/Networking/IAreaSimulation.h"
#include "Engine/Networking/LagCompensation.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Terrafront {

class TFServerSim final : public Spark::Net::IAreaSimulation {
  public:
    TFServerSim();
    ~TFServerSim() override;

    bool Initialize(TFGameContext& ctx, TFEventBus& events);
    void Update(float deltaTime);
    void FixedUpdate(float fixedDeltaTime);
    void Shutdown();
    void RenderDebugUI();

    // --- frozen cross-system API (DESIGN.md W1 contract) -------------------
    void EnqueueInput(PlayerId player, const TF_ClientInput& input);
    double ServerTime() const { return m_serverTime; }
    Spark::Net::LagCompensation& LagComp() { return m_lagComp; }

    FactionId GetPlayerFaction(PlayerId player) const;
    void SetPlayerFaction(PlayerId player, FactionId faction);

    // --- engine area-simulation hook ----------------------------------------
    // W1: the module's OnFixedUpdate drives the authoritative tick; OnAreaTick
    // only counts invocations so AreaServer wiring can be observed.
    // TF-W2: move the authoritative tick onto the area thread and make
    // FixedUpdate defer when area-driven.
    void OnAreaTick(float fixedDt) override;

    /// Debug panel toggle (hidden by default; wired from tf_* console commands).
    void ToggleDebugUI() { m_showDebug = !m_showDebug; }

  private:
    // Per-player authoritative movement state (seeded from EvPlayerSpawned).
    struct MoveState {
        EntityId pawn = 0;
        ClassId  cls  = ClassId::COUNT;
        float    pos[3]{0, 0, 0};   // feet
        float    vel[3]{0, 0, 0};
        float    yaw = 0.0f, pitch = 0.0f;
        bool     grounded = true;
        uint32_t lastSeq = 0;
    };

    void TickAuthoritative(float fdt);
    void TickMovement(float fdt);
    void StepPlayer(MoveState& ms, const TF_ClientInput* in, float dt);
    void WritePawnTransform(const MoveState& ms);
    void RecordLagCompSnapshot();

    void OnPlayerSpawned(const EvPlayerSpawned& ev);
    void OnPlayerKilled(const EvPlayerKilled& ev);

#ifdef ENABLE_NETWORKING
    void RegisterNetHandlers();
    void UnregisterNetHandlers();
    void PollClientJoinsLeaves();
    void SendMoveStates();
    void SendToPlayer(PlayerId player, uint16_t msgId, const void* payload,
                      size_t size, bool reliable);

    void HandleClientInput(PlayerId sender, const void* data, size_t size);
    void HandleSpawnRequest(PlayerId sender, const void* data, size_t size);
    void HandleFireEvent(PlayerId sender, const void* data, size_t size);
    void HandleFactionSelect(PlayerId sender, const void* data, size_t size);
    void SendSpawnReply(PlayerId player, const TF_SpawnReply& reply);
    void SendWorldWelcome(PlayerId player);
#endif

    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};

    double m_serverTime{0.0};
    uint64_t m_tickCount{0};
    uint64_t m_areaTickCount{0};

    Spark::Net::LagCompensation m_lagComp;
    bool m_lagCompConfigured{false};

    std::unordered_map<PlayerId, std::deque<TF_ClientInput>> m_inputs;
    std::unordered_map<PlayerId, MoveState> m_move;
    std::unordered_map<PlayerId, FactionId> m_factions;
    std::unordered_map<PlayerId, double>    m_deathTime;   // for the 8s respawn timer

    bool m_handlersRegistered{false};
    std::unordered_set<PlayerId> m_knownClients;

    float m_moveStateAccum{0.0f};

    // validation / debug counters
    uint32_t m_speedClamps{0};
    uint32_t m_badPackets{0};
    double   m_lastViolationLog{0.0};
    bool     m_showDebug{false};
};

} // namespace Terrafront
