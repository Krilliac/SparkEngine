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

    /// True once this player has an active authoritative MoveState (i.e. is
    /// spawned/alive). Mirrors the guard TFServerSim::HandleFactionSelect
    /// applies to networked FactionSelect packets (can't switch faction while
    /// alive) so callers outside the socket path -- namely the listen-host /
    /// standalone loopback router -- can enforce the same rule before calling
    /// SetPlayerFaction directly.
    bool IsPlayerAlive(PlayerId player) const { return m_move.contains(player); }

    /// Admin teleport (tf_tp): moves the authoritative MoveState and syncs
    /// the pawn Transform. Writing only the Transform is NOT enough — the
    /// movement tick overwrites it from MoveState one fixed step later.
    void TeleportPawn(PlayerId player, float x, float y, float z);

#ifdef ENABLE_NETWORKING
    /// W5 onboarding (Task 4): dispatches one of the onboarding client->server
    /// messages (Login/Register/CharList/CharCreate/CharDelete/EnterWorld) to
    /// its handler. Shared by the socket route (RegisterNetHandlers) and the
    /// listen-host/standalone loopback router (TFClientNet::RouteLoopback) so
    /// both paths run the exact same authoritative logic (DESIGN.md W5).
    void RouteClientMessage(PlayerId sender, TFMsg id, const void* data, size_t size);
#endif

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
    // TF-W3 (vehicles agent): TFMsg::VehicleEnter/VehicleExit/AegisDeploy
    // routing into TFVehicleSystem (was accepted-but-unrouted in W1/W2).
    void HandleVehicleSeatOp(PlayerId sender, const void* data, size_t size, bool enter);
    void HandleAegisDeploy(PlayerId sender, const void* data, size_t size);
    void SendSpawnReply(PlayerId player, const TF_SpawnReply& reply);
    void SendWorldWelcome(PlayerId player);

    // W5 onboarding (Task 4). Each guards `if (m_ctx->account)` /
    // `if (m_ctx->characters)` since those context pointers are null until
    // Task 6's boot wiring constructs and publishes the real systems — until
    // then the messages are accepted (no "unknown message" warning) but
    // answered with TFAuthErr::ServerError / TFCharErr::ServerError.
    void HandleLogin(PlayerId sender, const void* data, size_t size);
    void HandleRegister(PlayerId sender, const void* data, size_t size);
    void HandleCharList(PlayerId sender, const void* data, size_t size);
    void HandleCharCreate(PlayerId sender, const void* data, size_t size);
    void HandleCharDelete(PlayerId sender, const void* data, size_t size);
    // On success: binds the session's authoritative faction, marks the client
    // entered-world, and ONLY THEN sends TF_WorldWelcome (the gate).
    void HandleEnterWorld(PlayerId sender, const void* data, size_t size);
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
    std::unordered_set<PlayerId> m_enteredWorld;   // W5 onboarding: clients past the EnterWorld gate

    float m_moveStateAccum{0.0f};

    // validation / debug counters
    uint32_t m_speedClamps{0};
    uint32_t m_badPackets{0};
    double   m_lastViolationLog{0.0};
    bool     m_showDebug{false};
};

} // namespace Terrafront
