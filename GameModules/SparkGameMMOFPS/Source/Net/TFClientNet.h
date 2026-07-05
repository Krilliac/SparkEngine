/**
 * @file TFClientNet.h
 * @brief Client connection, ClientPrediction wiring, interpolation buffers.
 *
 * OWNERSHIP: this header + TFClientNet.cpp belong to ONE implementation agent.
 * The lifecycle + the FROZEN cross-system API below are the module contract.
 *
 * W1 full implementation:
 *  - Connection observe: TFWorldSetup::Connect boots NetworkManager in client
 *    role; this system detects the live connection in Update, registers the
 *    client-side TFMsg handlers (WorldWelcome/SpawnReply/HitConfirm/
 *    DamageEvent/KillEvent/XPEvent) and runs the TF handshake.
 *  - Input pump: samples InputManager at up to 60 Hz fixed steps, builds
 *    TF_ClientInput (seq/buttons/move axes/view angles) and feeds the SAME
 *    input to Spark::ClientPrediction with a TFMoveStep simulator
 *    (Game/TFMovementModel.h — the shared client/server movement contract).
 *  - Reconcile: consumes TFReplication's owner-only TF_MoveState feed
 *    (HasFreshMoveState/GetLatestMoveState) and calls ClientPrediction::
 *    Reconcile with the acked input sequence.
 *  - Remote pawns: 100 ms interpolation-delay buffers over the RemotePawn
 *    store; writes the smoothed pose into the local visual entities that
 *    TFPlayerSystem::SyncClientRecords creates.
 *  - First-person camera: mouse look drives the engine camera (FPS-module
 *    convention); the camera is positioned at the predicted pawn eye
 *    (TFWeaponMath::kEyeHeightM above the feet).
 *  - Listen-host / standalone loopback: when the local process is the
 *    authority, SendMsg/SendInput skip the socket entirely and route straight
 *    into TFServerSim / TFPlayerSystem / TFWeaponSystem, so a single process
 *    plays the full loop with no network.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFRepProtocol.h"

#include "Engine/Networking/ClientPrediction.h"
#include "Engine/Networking/InterpolationBuffer.h"

#include <string>
#include <unordered_map>

namespace Terrafront {

/// PlayerId used for the in-process local player on authority roles
/// (listen host / standalone). Deliberately far above NetworkManager's
/// incrementing client ids and distinct from kInvalidPlayer.
constexpr PlayerId kTFLocalHostPlayer = 0xFFFFFF01u;

class TFClientNet {
  public:
    TFClientNet();
    ~TFClientNet();

    bool Initialize(TFGameContext& ctx, TFEventBus& events);
    void Update(float deltaTime);
    void FixedUpdate(float fixedDeltaTime);
    void Shutdown();
    void RenderDebugUI();

    // --- FROZEN cross-system API (W1) ---
    bool     IsConnected() const;
    PlayerId LocalPlayerId() const;
    void     SendInput(const TF_ClientInput& input);
    void     SendMsg(TFMsg id, const void* payload, size_t size);

    /// Connect to a remote host (called via TFWorldSetup::Connect).
    bool Connect(const std::string& ip, uint16_t port);
    void Disconnect();

    // --- W1 additions beyond the frozen surface (documented in wave report) ---

    /// Pure client: the locally-predicted state of the OWN pawn (feet position,
    /// velocity, view angles). Returns false on authority roles / when no
    /// predicted pawn exists; TFPlayerSystem uses it to serve PawnInfo for the
    /// local player without waiting a replication round-trip.
    bool GetPredictedLocalState(float outPos[3], float outVel[3],
                                float& outYaw, float& outPitch) const;

    /// Debug panel toggle (hidden by default; wired from tf_* console commands).
    void ToggleDebugUI() { m_showDebug = !m_showDebug; }

  private:
    // Per-remote-entity interpolation buffers (100 ms render delay).
    struct InterpEntry {
        Spark::Net::InterpolationBuffer<DirectX::XMFLOAT3> pos;
        Spark::Net::InterpolationBuffer<float>             yaw;
        float  lastYaw{0.0f};      ///< unwrapped yaw of the newest sample
        double lastRecvTime{-1.0}; ///< RemotePawn.recvTime of the newest sample
        bool   has{false};
    };

    bool LocalLoopback() const;    ///< authority role with a local player
    void EnsureLocalHostIdentity();
    void UpdateConnectionState();
    void RouteLoopback(TFMsg id, const void* payload, size_t size);

    void PumpInput(float dt, bool aliveLocalPawn);
    void SendOneInput(float moveX, float moveY, uint16_t buttons);
    void ReconcileFromServer();
    void UpdateRemotePawns();
    void DriveFirstPersonCamera(bool aliveLocalPawn, const float feetPos[3]);
    void SeedPredictionAt(const float pos[3]);
    void SimulateMove(Spark::PredictedState& s, const Spark::PredictedInput& in, float dt) const;
    void RefreshClassSpeeds(ClassId cls);

    void OnBusPlayerKilled(const EvPlayerKilled& ev);
    void OnBusPlayerDamaged(const EvPlayerDamaged& ev);
    void PushKillfeedEntry(PlayerId killer, PlayerId victim, WeaponId weapon,
                           FactionId killerF, FactionId victimF);

#ifdef ENABLE_NETWORKING
    void RegisterClientHandlers();
    void ReleaseClientHandlers();
    void OnWorldWelcome(const void* data, size_t size);
    void OnSpawnReply(const void* data, size_t size);
    void OnHitConfirm(const void* data, size_t size);
    void OnDamageEvent(const void* data, size_t size);
    void OnKillEvent(const void* data, size_t size);
    void OnXPEvent(const void* data, size_t size);
#endif

    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};
    bool           m_connected{false};
    PlayerId       m_localPlayer{kInvalidPlayer};
    uint32_t       m_inputSeq{0};          ///< loopback-only sequence counter

    double m_clock{0.0};                   ///< monotonic client clock (Update)
    float  m_inputAccum{0.0f};             ///< 60 Hz input pacing accumulator
    bool   m_handlersRegistered{false};
    bool   m_wasAlive{false};

    // View angles (camera convention, radians; see TFMovementModel.h basis).
    float m_viewYaw{0.0f};
    float m_viewPitch{0.0f};

    // Prediction (pure client only)
    Spark::ClientPrediction m_prediction;
    Spark::PredictedState   m_predState{};
    bool                    m_predActive{false};
    float m_runSpeed{5.2f};
    float m_sprintSpeed{7.2f};

    // Remote pawn interpolation
    std::unordered_map<EntityId, InterpEntry> m_interp;

    // Stats / debug
    uint32_t m_inputsSent{0};
    uint32_t m_reconciles{0};
    uint16_t m_lastRank{1};
    uint32_t m_lastXPTotal{0};
    bool     m_showDebug{false};
};

} // namespace Terrafront
