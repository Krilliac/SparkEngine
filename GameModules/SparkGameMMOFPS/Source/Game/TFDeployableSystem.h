/**
 * @file TFDeployableSystem.h
 * @brief Fabricator turret / ammo pack + Medtech beacon (W3).
 *
 * OWNERSHIP: this header + TFDeployableSystem.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 *
 * W3 implementation:
 *  Server (authority):
 *   - ServerTryPlaceDeployable(player, kind): class-gated (FabTurret/FabAmmoPack
 *     need Fabricator, MedBeacon needs Medtech), placed 2 m in front of the
 *     pawn on the terrain, max ONE per player per kind (replacing destroys the
 *     old one). Deployables are ECS entities: Transform + TFDeployableComp +
 *     TFFactionComp + NetworkIdentity + prop visual (crate_a / antenna_a with
 *     faction-tinted material).
 *   - Mechanics (per-Update tick; Main.cpp does not drive FixedUpdate here):
 *       FabTurret  300 hp, 60 s: auto-acquires the nearest enemy pawn within
 *                  40 m with terrain LoS, hitscan 95 dmg @ 450 rpm through
 *                  ctx.damage with OWNER kill credit.
 *       FabAmmoPack 150 hp, 60 s: field resupply crate — heals pawns within
 *                  5 m by 20 hp every 5 s. TF-W4: real mag/reserve refill
 *                  (needs a resupply hook on TFWeaponSystem's server
 *                  ShooterState + a client ammo-sync message; no clean seam
 *                  exists this wave).
 *       MedBeacon  150 hp, 45 s: heals same-faction pawns within 6 m at
 *                  30 hp/s (health only, never revives).
 *   - Damage: ServerDamageDeployable / ServerSplashDamageDeployables (the
 *     latter is called from TFWeaponServer's ExplodeAt under the W3 shared-
 *     edit grant, same linear falloff as pawn splash). Direct hitscan vs
 *     deployables is TF-W4 (needs deployable capsules in the lag-comp set).
 *   - Replication: compact packed structs on 0x54FC-0x54FE (TFRepProtocol.h,
 *     additive block) — reliable Create/Destroy, health/life Update on damage
 *     + 1 Hz keepalive, late-joiner Create burst (GetClients() diff poll, the
 *     established TFReplication pattern).
 *  Client:
 *   - Mirror store + local ECS prop visuals driven by the 0x54FC-0x54FE
 *     handlers; lifetime decays locally between keepalives.
 *
 * Trigger wiring: mechanics are complete behind ServerTryPlaceDeployable; the
 * orchestrator wires a tf_place console command + TFB_Ability routing in W4.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace Terrafront {

/// Result of a ServerTryPlaceDeployable attempt.
enum class TFDeployResult : uint8_t {
    Ok = 0,
    NotAuthority,   ///< called on a pure client
    DataMissing,    ///< data tables not loaded
    NoPawn,         ///< player has no pawn or is dead
    WrongClass,     ///< pawn class may not place this deployable kind
    BadKind,        ///< kind out of range
    SpawnFailed,    ///< entity creation failed (no ECS world)
};

/// Cross-side snapshot of one deployable (server records and the client
/// mirror expose the same view through GetDeployable / ForEachDeployable).
struct TFDeployableView {
    EntityId       entity;      ///< network id (server ECS id)
    DeployableKind kind;
    PlayerId       owner;
    FactionId      faction;
    float          pos[3];
    float          yaw;         ///< radians
    float          health, maxHealth;
    float          life;        ///< remaining lifetime seconds
};

class TFDeployableSystem {
  public:
    TFDeployableSystem();
    ~TFDeployableSystem();

    bool Initialize(TFGameContext& ctx, TFEventBus& events);
    void Update(float deltaTime);
    void FixedUpdate(float fixedDeltaTime);
    void Shutdown();
    void RenderDebugUI();

    // --- W3 public surface --------------------------------------------------

    /// Server: validate + place a deployable for `player` (see file header).
    TFDeployResult ServerTryPlaceDeployable(PlayerId player, DeployableKind kind);

    /// Server: apply damage to a deployable (any source). Destroys + replicates
    /// the destroy at <= 0 hp. Friendly fire policy: same-faction damage is
    /// ignored (deployables are cheap; grief angle is W4 balance).
    void ServerDamageDeployable(EntityId deployable, PlayerId attackerPlayer, float amount);

    /// Server: radial splash vs all deployables (linear falloff to the rim).
    /// Called by TFWeaponServer::ExplodeAt under the W3 shared-edit grant.
    void ServerSplashDamageDeployables(const float at[3], float radiusM, float damage,
                                       PlayerId attackerPlayer);

    /// Snapshot queries (server truth on authority, replicated mirror on clients).
    bool GetDeployable(EntityId entity, TFDeployableView& out) const;
    void ForEachDeployable(const std::function<void(const TFDeployableView&)>& fn) const;
    size_t DeployableCount() const { return m_deployables.size(); }

    /// Human-readable result text (console command replies / logs).
    static const char* ResultText(TFDeployResult r);

  private:
    /// One live deployable. On the authority `local` is the ECS entity id
    /// (== view.entity); on clients it is the locally created mirror entity.
    struct Rec {
        TFDeployableView view{};
        uint32_t local = 0;
        // turret runtime
        EntityId targetPawn = 0;
        double   nextShotAt = 0.0;
        // pulse runtime (ammo pack)
        double   nextPulseAt = 0.0;
    };

    // --- server side ---------------------------------------------------------
    void   ServerTick(float dt);
    void   TickTurret(Rec& rec);
    void   TickAmmoPack(Rec& rec);
    void   TickMedBeacon(Rec& rec, float dt);
    bool   TurretHasLoS(const float from[3], const float to[3]) const;
    void   ServerDestroyDeployable(EntityId entity, const char* why);
    uint32_t CreateDeployableEntity(const TFDeployableView& view);
    void   DestroyLocalEntity(uint32_t local);
    void   AttachDeployableVisual(uint32_t local, DeployableKind kind, FactionId faction);
    double NowSec() const;

    // --- replication (server broadcast / client mirror) ----------------------
#ifdef ENABLE_NETWORKING
    bool ServerNetActive() const;
    bool ClientNetActive() const;
    void ServerPollNewClients();
    void SendCreate(PlayerId target, const TFDeployableView& view);
    void SendUpdate(const TFDeployableView& view);
    void SendDestroy(EntityId entity);
    void SendRep(PlayerId target, uint16_t msgId, const void* payload, size_t size,
                 bool reliable);
    void ClientEnsureHandlers();
    void ClientReleaseHandlers();
    void OnDeployCreate(const void* data, size_t size);
    void OnDeployUpdate(const void* data, size_t size);
    void OnDeployDestroy(const void* data, size_t size);
#endif

    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};
    double         m_clock{0.0};

    /// entity -> record (server truth on authority, mirror on clients).
    std::unordered_map<EntityId, Rec> m_deployables;
    /// owner -> kind -> entity (server-side per-player-per-kind limit).
    std::unordered_map<PlayerId,
                       std::unordered_map<uint8_t, EntityId>> m_ownerIndex;

    WeaponId m_turretWeapon{kInvalidWeapon}; ///< killfeed credit id (lazy lookup)

#ifdef ENABLE_NETWORKING
    std::unordered_set<PlayerId> m_knownClients;
    bool  m_clientHandlers{false};
    float m_keepaliveAccum{0.0f};
#endif

    // debug counters
    uint32_t m_placed{0}, m_expired{0}, m_destroyedByDamage{0}, m_turretShots{0};
    float    m_healGiven{0.0f};
};

} // namespace Terrafront
