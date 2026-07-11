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
 *
 * W6 expansion (this lane; kinds/specs in Game/TFDeployableTypes.h):
 *  - Three new kinds: ResupplyStation (heal pulse + repairs nearby friendly
 *    deployables), AVTurret (slow heavy shots at enemy vehicles through
 *    TFVehicleSystem::ServerDamageVehicle, applied AFTER the tick loop so a
 *    vehicle-destruction chain can never re-enter m_deployables mid-iteration),
 *    ShieldWall (destructible barrier; static Jolt box body when physics is
 *    available, null-checked, NEVER stepped here — module physics stepping
 *    stays out of MMOFPS per the W6 ground rules).
 *  - Placement validation before any state changes (TFDeployablePlacement.cpp):
 *    terrain slope, spacing vs existing deployables, region ownership (enemy-
 *    held region refuses), physics overlap probe when Jolt is live.
 *  - Per-player limits: per-kind caps (oldest replaced at cap — generalizes the
 *    W3 one-per-kind rule) + a total cap that REFUSES (LimitReached).
 *  - New kinds ride the existing 0x54FC-0x54FE wire unchanged (`kind` is a
 *    uint8_t on the wire); no protocol additions.
 *
 * W8 combat-features (this lane):
 *  - EvDeployablePlaced (Core/TFEvents.h) fired at the end of a successful
 *    ServerTryPlaceDeployable (server-side only, after all state changes +
 *    Create replication). TFDirectiveSystem subscribes for DeployItems credit;
 *    its 0.5 s diff-poll survives as the fallback path.
 *  - Killfeed naming: TickTurret / TickAVTurret lazily resolve the additive
 *    weapons.json rows "fab_turret" / "av_turret" (ids 51/52), so turret kills
 *    show a proper weapon name instead of "-".
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class PhysicsBody; // Physics/PhysicsBody.h (global namespace, raw fwd decl)

namespace Terrafront
{

    struct TFDeployableSpec; // Game/TFDeployableTypes.h (kind catalog + placement specs)

    /// Result of a ServerTryPlaceDeployable attempt.
    enum class TFDeployResult : uint8_t
    {
        Ok = 0,
        NotAuthority, ///< called on a pure client
        DataMissing,  ///< data tables not loaded
        NoPawn,       ///< player has no pawn or is dead
        WrongClass,   ///< pawn class may not place this deployable kind
        BadKind,      ///< kind out of range
        SpawnFailed,  ///< entity creation failed (no ECS world)
        // --- W6 placement validation ---
        TooSteep,      ///< terrain grade at the drop point exceeds the kind's limit
        TooClose,      ///< within minSpacingM of another deployable
        HostileRegion, ///< drop point lies in an enemy-owned region
        Blocked,       ///< physics overlap probe hit world geometry
        LimitReached,  ///< total per-player active cap reached
    };

    /// Cross-side snapshot of one deployable (server records and the client
    /// mirror expose the same view through GetDeployable / ForEachDeployable).
    struct TFDeployableView
    {
        EntityId entity; ///< network id (server ECS id)
        DeployableKind kind;
        PlayerId owner;
        FactionId faction;
        float pos[3];
        float yaw; ///< radians
        float health, maxHealth;
        float life; ///< remaining lifetime seconds
    };

    class TFDeployableSystem
    {
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
        void ServerSplashDamageDeployables(const float at[3], float radiusM, float damage, PlayerId attackerPlayer);

        /// Snapshot queries (server truth on authority, replicated mirror on clients).
        bool GetDeployable(EntityId entity, TFDeployableView& out) const;
        void ForEachDeployable(const std::function<void(const TFDeployableView&)>& fn) const;
        size_t DeployableCount() const { return m_deployables.size(); }

        /// Human-readable result text (console command replies / logs).
        static const char* ResultText(TFDeployResult r);

      private:
        /// One live deployable. On the authority `local` is the ECS entity id
        /// (== view.entity); on clients it is the locally created mirror entity.
        struct Rec
        {
            TFDeployableView view{};
            uint32_t local = 0;
            // turret runtime (targetPawn doubles as the AV turret's target vehicle)
            EntityId targetPawn = 0;
            double nextShotAt = 0.0;
            // pulse runtime (ammo pack / resupply station)
            double nextPulseAt = 0.0;
            // ShieldWall static blocker (authority + live Jolt only; empty otherwise).
            // shared_ptr copies/dtors are safe on the fwd-declared type — only
            // PhysicsSystem::CreateBody (engine side) needs the complete type.
            std::shared_ptr<PhysicsBody> wallBody;
        };

        /// AV turret shot deferred to after ServerTick's iteration: destroying a
        /// vehicle mid-shot can chain into ServerSplashDamageDeployables, which
        /// erases from m_deployables — never re-enter the map mid-iteration.
        struct PendingAVShot
        {
            EntityId vehicle = 0;
            EntityId turretEntity = 0;
            PlayerId owner = kInvalidPlayer;
        };

        // --- server side ---------------------------------------------------------
        void ServerTick(float dt);
        void TickTurret(Rec& rec);
        void TickAmmoPack(Rec& rec);
        void TickMedBeacon(Rec& rec, float dt);
        void TickResupplyStation(Rec& rec);
        void TickAVTurret(Rec& rec);
        bool TurretHasLoS(const float from[3], const float to[3]) const;
        void ServerDestroyDeployable(EntityId entity, const char* why);
        uint32_t CreateDeployableEntity(const TFDeployableView& view);
        void DestroyLocalEntity(uint32_t local);
        void AttachDeployableVisual(uint32_t local, DeployableKind kind, FactionId faction);
        double NowSec() const;

        // --- W6 placement validation + ShieldWall body (TFDeployablePlacement.cpp)
        /// All checks run BEFORE any state changes. `ignoreEntity` excludes the
        /// deployable about to be replaced from the spacing check (0 = none).
        TFDeployResult ValidatePlacement(const TFDeployableSpec& spec, FactionId placer, const float pos[3],
                                         EntityId ignoreEntity);
        float TerrainSlopeTanAt(float x, float z) const;
        bool RegionBlocksPlacement(FactionId placer, const float pos[3]);
        bool PhysicsOverlapBlocked(const TFDeployableSpec& spec, const float pos[3]) const;
        void CreateWallBody(Rec& rec);
        void ReleaseWallBody(Rec& rec);

        // --- replication (server broadcast / client mirror) ----------------------
#ifdef ENABLE_NETWORKING
        bool ServerNetActive() const;
        bool ClientNetActive() const;
        void ServerPollNewClients();
        void SendCreate(PlayerId target, const TFDeployableView& view);
        void SendUpdate(const TFDeployableView& view);
        void SendDestroy(EntityId entity);
        void SendRep(PlayerId target, uint16_t msgId, const void* payload, size_t size, bool reliable);
        void ClientEnsureHandlers();
        void ClientReleaseHandlers();
        void OnDeployCreate(const void* data, size_t size);
        void OnDeployUpdate(const void* data, size_t size);
        void OnDeployDestroy(const void* data, size_t size);
#endif

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};
        double m_clock{0.0};

        /// entity -> record (server truth on authority, mirror on clients).
        std::unordered_map<EntityId, Rec> m_deployables;
        /// owner -> kind -> entities, oldest first (server-side per-player limits;
        /// W6 generalizes the W3 one-per-kind rule to spec-table per-kind caps).
        std::unordered_map<PlayerId, std::unordered_map<uint8_t, std::vector<EntityId>>> m_ownerIndex;

        /// AV shots queued during ServerTick's iteration, applied after it.
        std::vector<PendingAVShot> m_pendingAvShots;

        WeaponId m_turretWeapon{kInvalidWeapon}; ///< killfeed credit id (lazy lookup)
        WeaponId m_avWeapon{kInvalidWeapon};     ///< AV turret credit id (lazy lookup)
        float m_regionSpacing{-1.0f};            ///< lazy min region-center spacing (< 0 = unset)

#ifdef ENABLE_NETWORKING
        std::unordered_set<PlayerId> m_knownClients;
        bool m_clientHandlers{false};
        float m_keepaliveAccum{0.0f};
#endif

        // debug counters
        uint32_t m_placed{0}, m_expired{0}, m_destroyedByDamage{0}, m_turretShots{0};
        uint32_t m_avShots{0}, m_refusedPlacements{0};
        float m_healGiven{0.0f}, m_repairGiven{0.0f};
    };

} // namespace Terrafront
