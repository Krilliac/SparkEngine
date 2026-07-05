/**
 * @file TFPlayerSystem.h
 * @brief Spawn/death/respawn, class loadouts, pawn registry (both sides).
 *
 * OWNERSHIP: this header + TFPlayerSystem*.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 *
 * W1: server spawn pipeline (TF_SpawnRequest/Reply), pawn entity creation with
 * TF components (Game/TFComponents.h), class stats from classes.json, kill /
 * respawn flow, and the shared PawnInfo registry every other system reads.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFNetProtocol.h"

#include <functional>
#include <unordered_map>

namespace Terrafront {

/// Snapshot view of a pawn (FROZEN cross-system contract, DESIGN.md W1).
/// `entity` is the NETWORK entity id (== server-side ECS id); on pure clients
/// use TFPlayerSystem::ResolveEntity to get the local entt id.
struct PawnInfo {
    EntityId  entity;
    PlayerId  owner;
    FactionId faction;
    ClassId   cls;
    bool      alive;
    float     pos[3];
    float     vel[3];
    float     yaw, pitch;
    float     health, shield;
};

class TFPlayerSystem {
  public:
    /// Both `Terrafront::PawnInfo` and `TFPlayerSystem::PawnInfo` name the same type.
    using PawnInfo = ::Terrafront::PawnInfo;

    TFPlayerSystem();
    ~TFPlayerSystem();

    bool Initialize(TFGameContext& ctx, TFEventBus& events);
    void Update(float deltaTime);
    void FixedUpdate(float fixedDeltaTime);
    void Shutdown();
    void RenderDebugUI();

    // --- FROZEN cross-system API (W1) ---
    bool GetPawnByEntity(EntityId entity, PawnInfo& out) const;
    bool GetPawnByPlayer(PlayerId player, PawnInfo& out) const;
    void ForEachAlivePawn(const std::function<void(const PawnInfo&)>& fn) const;
    EntityId ServerSpawnPawn(PlayerId player, FactionId faction, ClassId cls,
                             const float pos[3], float yaw);
    void ServerKillPawn(EntityId victim, PlayerId killerPlayer, WeaponId weapon, bool headshot);
    void ServerSetPawnHealth(EntityId entity, float health, float shield);

    // --- W1 additions beyond the frozen surface (documented in wave report) ---

    /// Server: TFServerSim routes TFMsg::FactionSelect here.
    void ServerHandleFactionSelect(PlayerId player, FactionId faction);

    /// Server: TFServerSim routes TFMsg::SpawnRequest here (validates timer /
    /// class / spawn point, spawns, and sends TF_SpawnReply back).
    void ServerHandleSpawnRequest(PlayerId player, const TF_SpawnRequest& req);

    /// Server: call when a client drops so its pawn is removed.
    void ServerHandlePlayerDisconnect(PlayerId player);

    /// Client: TFClientNet routes TFMsg::SpawnReply here (associates the local
    /// player with its replicated pawn entity). No-op on the authority.
    void ClientOnSpawnReply(const TF_SpawnReply& reply);

    /// Client: TFClientNet sniffs outgoing TF_SpawnRequest so the local record
    /// knows which class was asked for (SpawnReply does not echo it).
    void ClientNoteRequestedClass(ClassId cls) { m_lastRequestedClass = cls; }

    /// Faction a player has picked (FactionId::None if not yet).
    FactionId FactionOf(PlayerId player) const;

    /// Map a network entity id to the local entt entity id (identity on the
    /// authority; replication-created entity on pure clients).
    bool ResolveEntity(EntityId netEntity, uint32_t& outLocalEntity) const;

  private:
    struct PlayerRec {
        FactionId faction = FactionId::None;
        ClassId   cls     = ClassId::Ghost;
        EntityId  pawn    = 0;      ///< network entity id (0 = none)
        uint32_t  local   = 0;      ///< local entt id
        bool      hasPawn = false;
        bool      alive   = false;
        double    nextRespawnAt = 0.0;
        double    despawnAt     = 0.0;
    };

    bool  FillPawnInfo(PlayerId player, const PlayerRec& rec, PawnInfo& out) const;
    void  SendSpawnReply(PlayerId player, const TF_SpawnReply& reply);
    WeaponId PickDefaultWeapon(FactionId faction, ClassId cls) const;
    bool  FindSkyanchorSpawn(FactionId faction, float outPos[3], float& outYaw) const;
    bool  FindRegionSpawn(RegionId region, FactionId faction, float outPos[3],
                          float& outYaw) const;
    void  DestroyPawn(PlayerRec& rec);
    void  SyncClientRecords();      ///< pure client: discover replicated pawns
    void  AttachPawnVisual(uint32_t localEntity, FactionId faction);

    // W1 private helpers (ECS-backed implementation)
    double   NowSec() const;        ///< serverSim time on authority, local clock otherwise
    uint32_t CreatePawnEntity(PlayerId player, FactionId faction, ClassId cls,
                              const float pos[3], float yaw); ///< 0 if no ECS world
    void     UpdateAuthorityLifecycle();   ///< corpse despawn sweep (kTFDespawnDelaySec)

    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};
    double         m_clock{0.0};    ///< fallback time base when serverSim is absent

    std::unordered_map<PlayerId, PlayerRec> m_players;
    std::unordered_map<EntityId, PlayerId>  m_pawnOwner;   ///< net entity -> player
    EntityId m_pendingLocalPawn{0};           ///< client: SpawnReply entity not yet replicated
    ClassId  m_lastRequestedClass{ClassId::Ghost};
};

/// Respawn rules (DESIGN §4). kTFDespawnDelaySec = corpse-hide window before
/// the dead pawn entity is destroyed (W1 "hide + respawn"; TF-W4: ragdoll).
constexpr float kTFRespawnDelaySec = 8.0f;
constexpr float kTFDespawnDelaySec = 5.0f;

} // namespace Terrafront
