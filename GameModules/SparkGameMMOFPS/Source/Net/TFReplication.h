/**
 * @file TFReplication.h
 * @brief TF replication channel — server-side pawn create/update/destroy
 *        broadcasting and the client-side RemotePawn store.
 *
 * OWNERSHIP: this header + TFReplication.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 *
 * W1 implementation (channel map in TFRepProtocol.h, ids 0x54F0-0x54F3):
 *  Server (authority roles, NetworkManager role == Server):
 *   - EvPlayerSpawned  -> broadcast kTFRepMsg_Create (TF_RepPawnCreate, reliable)
 *   - EvPlayerKilled   -> broadcast kTFRepMsg_Destroy (reliable); a per-tick
 *     sweep also destroys entities that silently left the alive-pawn set
 *   - late join (GetClients() diff, same poll pattern as TFServerSim because
 *     NetworkManager has no join callback slot) -> full pawn set as Creates
 *   - 20 Hz: one kTFRepMsg_Update batch (TF_RepUpdateHeader + N
 *     TF_RepPawnUpdate) to all clients; per-pawn dirty check against the
 *     last-sent quantized state so unchanged pawns are skipped
 *  Client (NetworkManager role == Client):
 *   - handlers for Create/Update/Destroy/MoveState maintain the RemotePawn
 *     store and the owner-only latest TF_MoveState (shared W1B contract
 *     consumed by TFClientNet / TFPlayerSystem).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFRepProtocol.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace Terrafront {

struct PawnInfo; // TFPlayerSystem.h

/// Client-side view of a replicated pawn (shared W1B contract — consumed by
/// TFClientNet and TFPlayerSystem; layout/fields frozen for W1).
struct RemotePawn {
    EntityId  entity;
    PlayerId  owner;
    FactionId faction;
    ClassId   cls;
    bool      alive;
    float     pos[3];
    float     vel[3];        ///< estimated from successive updates (0 until 2nd)
    float     yaw, pitch;    ///< radians
    float     health, shield;
    double    recvTime;      ///< client clock when the last update arrived
};

class TFReplication {
  public:
    TFReplication();
    ~TFReplication();

    bool Initialize(TFGameContext& ctx, TFEventBus& events);
    void Update(float deltaTime);
    void FixedUpdate(float fixedDeltaTime);
    void Shutdown();
    void RenderDebugUI();

    // --- shared W1B contract (client-side store) ---------------------------
    bool GetRemotePawn(EntityId entity, RemotePawn& out) const;
    void ForEachRemotePawn(const std::function<void(const RemotePawn&)>& fn) const;

    /// Owner-only authoritative move state (kTFRepMsg_MoveState). Returns the
    /// newest received state and clears the 'fresh' flag.
    bool GetLatestMoveState(TF_MoveState& out) const;
    bool HasFreshMoveState() const { return m_freshMoveState; }

    /// Debug panel toggle (hidden by default; wired from tf_* console commands).
    void ToggleDebugUI() { m_showDebug = !m_showDebug; }

  private:
    /// Last state broadcast for a pawn entity (server-side dirty check).
    struct SentState {
        QuantPos pos;
        QuantAim aim;
        uint16_t health = 0;
        uint16_t shield = 0;
        uint8_t  alive  = 0;
        bool operator==(const SentState&) const = default;
    };

    static SentState QuantizePawn(const PawnInfo& p);

    void OnPlayerSpawned(const EvPlayerSpawned& ev);
    void OnPlayerKilled(const EvPlayerKilled& ev);

#ifdef ENABLE_NETWORKING
    // --- server side (TFReplication.cpp) ------------------------------------
    bool ServerActive() const;
    static TF_RepPawnCreate MakeCreate(const PawnInfo& p);
    void ServerPollNewClients();
    void ServerBroadcastUpdates();
    void ServerBroadcastDestroy(EntityId entity);
    /// target == kInvalidPlayer broadcasts to all clients.
    void SendRep(PlayerId target, uint16_t msgId, const void* payload,
                 size_t size, bool reliable);

    // --- client side (TFReplicationClient.cpp) ------------------------------
    bool ClientActive() const;
    void ClientEnsureHandlers();
    void ClientReleaseHandlers();
    void OnRepCreate(const void* data, size_t size);
    void OnRepUpdate(const void* data, size_t size);
    void OnRepDestroy(const void* data, size_t size);
    void OnRepMoveState(const void* data, size_t size);
#endif

    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};

    // server state
    std::unordered_set<PlayerId>             m_knownClients;
    std::unordered_map<EntityId, SentState>  m_lastSent;
    float m_updateAccum{0.0f};

    // client state
    std::unordered_map<EntityId, RemotePawn> m_pawns;
    TF_MoveState m_moveState{};
    bool         m_hasMoveState{false};
    mutable bool m_freshMoveState{false};
    bool         m_clientHandlers{false};
    double       m_clock{0.0};              ///< monotonic client clock (Update)

    // stats (whichever direction this role drives)
    uint32_t m_ctrMsgs{0},  m_ctrRecords{0};
    uint64_t m_ctrBytes{0};
    float    m_msgsPerSec{0.0f}, m_recordsPerSec{0.0f}, m_bytesPerSec{0.0f};
    float    m_rateWindow{0.0f};
    uint32_t m_skippedUnchanged{0};
    uint32_t m_unknownEntityUpdates{0};
    uint32_t m_badPackets{0};
    bool     m_showDebug{false};
};

} // namespace Terrafront
