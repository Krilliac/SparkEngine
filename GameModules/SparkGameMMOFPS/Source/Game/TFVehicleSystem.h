/**
 * @file TFVehicleSystem.h
 * @brief Drifter/Aegis/Ravager/Vulture: seats, driving (ground + VTOL flight),
 *        vehicle weapons, Aegis deploy-spawn (W3 combined-arms core; Vulture
 *        VTOL gunship enabled in W8 — Jump/Crouch climb axis, ~120 m AGL
 *        ceiling, landed-only exit).
 *
 * OWNERSHIP: this header + the TFVehicleSystem*.cpp split parts (core /
 * Spawn / Seats / Drive / Damage + TFVehicleSystemInternal.h) +
 * TFVehicleNet.cpp + TFVehicleClient.cpp + TFVehiclePhysics.h/.cpp belong to
 * ONE implementation agent. The lifecycle below is the frozen module contract (called from
 * Main.cpp) — extend this class freely, but do not change the lifecycle
 * signatures.
 *
 * Driving model: when the engine has a live Jolt world, each vehicle gets a
 * dynamic rigid-body hull (TFVehiclePhysics — hover suspension, drive/steer
 * forces, collision vs world geometry and other hulls) and StepVehicleJolt
 * replaces the math integrator. Without Jolt the original pure-math
 * StepVehicle path runs unchanged. Both paths produce the same authoritative
 * pos/yaw/pitch/roll/speed record, so the replication wire format, seats,
 * damage and client mirror are identical either way. The single per-process
 * physics step is issued by TFServerSim's fixed tick, never by this system.
 *
 * W3 implementation:
 *  Server (authority roles):
 *   - ServerPurchaseVehicle: validated terminal purchase (alive + friendly
 *     region's vehicleTerminal within kTFVehTerminalRangeM + region owned +
 *     ServerSpendFlux). Creates the vehicle ECS entity (Transform +
 *     TFVehicleComp + TFFactionComp + TFAegisDeployComp + HealthComponent +
 *     faction-tinted OBJ MeshRenderer) on the terminal pad.
 *   - Seats: TF_VehicleSeatOp routed here by TFServerSim. Enter within 4 m of
 *     the hull, first free seat (driver = 0) wins on conflict; exit places the
 *     pawn beside the vehicle at terrain height. Seated pawns RIDE the
 *     vehicle: TFServerSim::TickMovement skips StepPlayer for IsSeated()
 *     players, forwards their inputs to ServerHandleSeatedInput (driver
 *     throttle/steer) and pulls the ride pose back via SyncSeatedPawn — so
 *     the pawn Transform / replication / lag-comp all follow the vehicle for
 *     free. IsSeated stays true for one extra tick after exit so the exit
 *     placement flows through that same sync before walking resumes.
 *   - Driving: data-driven (vehicles.json topSpeed/accel/turnRate), terrain-
 *     following with pitch/roll visual tilt from terrain slope, world-bounds
 *     clamp, throttle decay when input starves. Deployed Aegis is immobile.
 *   - Vehicle weapons: seated players fire through the NORMAL infantry path
 *     (TF_FireEvent -> TFWeaponSystem::ServerHandleFire); TFWeaponServer asks
 *     GetSeatWeapon() and swaps the shot to the seat weapon (or drops it for
 *     unarmed seats). The riding pawn IS the shooter surrogate — its
 *     transform already sits on the vehicle, so origin, lag comp and hit
 *     confirm feedback all work unchanged.
 *   - Damage/destruction: ServerDamageVehicle (hp pool, no regen, friendly
 *     fire at 50% like infantry). Destruction ejects occupants ALIVE with 50%
 *     max-pool damage, splashes kTFVehExplodeRadiusM/kTFVehExplodeDamage on
 *     other nearby pawns via ctx.damage, awards 300 XP to the destroyer,
 *     fires EvVehicleDestroyed and replicates the destroy.
 *   - Aegis: TF_AegisDeploy toggles below 1 m/s (driver only). Deployed =
 *     mobile spawn: GetAegisSpawnPos feeds TFPlayerSystem spawnKind==2.
 *  Replication (0x54F8+ block, TFRepProtocol.h): compact create/update/
 *   destroy/seats mirroring the pawn channel — 20 Hz dirty-checked updates,
 *   reliable creates to late joiners (GetClients() diff poll), client mirror
 *   store + interpolated OBJ visuals in this system (TFVehicleNet/Client.cpp).
 *  W8 turret aim: the turret-controller seat's view angles (already in its
 *   60 Hz TF_ClientInput) drive the vehicle's aim rig server-side (pitch
 *   clamped to 35 deg up / 10 deg down) and replicate on 0x5448
 *   (TF_RepVehicleAim, 10 Hz on change / 1 Hz keepalive). The Ravager turret
 *   yaws + its 90 mm barrel pitches; the Aegis PDW head yaws+pitches on a
 *   static pedestal. Controller-seat shots leave the aimed muzzle frame
 *   (GetSeatFireFrame, consumed by TFWeaponServer) so fire matches the visual.
 *  Client UX (RenderDebugUI hook, SPARK_HAS_IMGUI): terminal purchase menu (E),
 *   enter/exit prompts (E), Aegis deploy toggle (B), vehicle hp bar while
 *   seated. Pure-client caveat (documented): walking prediction keeps running
 *   while seated, so the first-person view reconciles toward the ride pose at
 *   20 Hz instead of predicting it — listen-host/standalone is smooth.
 *  W10 vehicle HUD + seat swap (this lane):
 *   - Seated cockpit widget lives in UI/TFVehicleHUD.h/.cpp (owned + rendered
 *     by this system from RenderDebugUI; it replaced the W3 RenderSeatedHud
 *     bar). GetSeatOf is the public accessor it reads seats through.
 *   - SEAT SWAP reuses the existing wire message: TFMsg::VehicleEnter with a
 *     TF_VehicleSeatOp naming the CURRENT vehicle while already seated in it
 *     is a swap request (no new TFMsg, no struct change — the previous
 *     behaviour for that packet was a silent no-op). Server validation is
 *     STRICT: same vehicle, requested seat exists, is not yours, and is empty
 *     — no first-free fallback, a failed swap keeps the current seat. Keys:
 *     1-8 request that seat, F cycles to the next free seat.
 *  W13 damage states + wrecks (this lane):
 *   - No new TFMsg: hull hp already replicates (0x54F8 create/update health
 *     fields) so the client-side tier presentation (smoke/fire/spark, see
 *     Game/TFVehicleFx.h) reads TFVehicleInfo.hp/maxHp off the existing
 *     ForEachVehicle enumerator.
 *   - PERFORMANCE DEGRADATION is server-authoritative: critical hulls
 *     (<=33% hp) lose ~30% top speed + turn authority
 *     (DamageMovementMults). Determinism: StepVehicle (math) and
 *     StepVehicleJolt (Jolt) are the only two driving paths and both run
 *     authority-only (pure clients interpolate the replicated pose, never
 *     predict vehicle physics — see the W3 caveat above), so applying the
 *     identical multiplier to whichever path executes for a given tick is
 *     sufficient; there is no separate client prediction to keep in sync.
 *   - DESTRUCTION now leaves a persistent charred wreck (SpawnWreck/
 *     UpdateWrecks): the hull entity is retinted (Structure_AlloyDark.json,
 *     same mesh) instead of destroyed immediately, and despawns after
 *     kTFVehWreckLifeSec (15 s). It is removed from m_vehicles/m_mirror at
 *     the same point as before, so every other query (damage, raycast,
 *     ForEachVehicle, HUD, TFGroundFx/TFVehicleFx) stops seeing it as a
 *     "vehicle" the instant it becomes a wreck — only the leftover render
 *     entity lingers.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFRepProtocol.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Terrafront
{

    struct VehicleDef;      // Data/TFDataTables.h
    struct RegionDef;       // Data/TFDataTables.h
    class TFVehiclePhysics; // Game/TFVehiclePhysics.h (Jolt-backed server driving)
    class TFVehicleHUD;     // UI/TFVehicleHUD.h (W10 seated cockpit widget)

    // W3 vehicle tuning constants (engine-side limits; balance lives in vehicles.json).
    constexpr float kTFVehTerminalRangeM = 25.0f; ///< purchase reach from a terminal
    constexpr float kTFVehTerminalPromptM = 8.0f; ///< client prompt / menu-open reach
    constexpr float kTFVehEnterRangeM = 4.0f;     ///< enter reach from the hull
    constexpr float kTFVehExplodeRadiusM = 6.0f;  ///< destruction splash radius
    constexpr float kTFVehExplodeDamage = 400.0f; ///< destruction splash at center
    constexpr float kTFVehEjectDamageFrac = 0.5f; ///< of max health+shield on destruction
    constexpr float kTFVehDeployMaxSpeed = 1.0f;  ///< m/s — must be ~stopped to deploy
    constexpr uint16_t kTFVehKillXP = 300;

    // W8 turret aim: server pitch clamp in CAMERA convention (positive = down —
    // the sign TF_ClientInput.viewPitch and BuildViewRay use). Design range is
    // "[-10, +35] deg": 35 deg elevation (up) / 10 deg depression (down).
    constexpr float kTFTurretPitchMinRad = -0.6108652f; ///< 35 deg elevation (up)
    constexpr float kTFTurretPitchMaxRad = 0.1745329f;  ///< 10 deg depression (down)

    /// Shared snapshot of one vehicle — served from the authoritative records on
    /// server roles and from the replication mirror on pure clients (same
    /// pattern as TFRegionSystem's accessors).
    struct TFVehicleInfo
    {
        EntityId entity = 0;
        VehicleId vehId = VehicleId::None;
        FactionId faction = FactionId::None;
        float pos[3] = {0.0f, 0.0f, 0.0f};
        float yaw = 0.0f; ///< radians
        float hp = 0.0f;
        float maxHp = 0.0f;
        bool deployed = false;
        PlayerId seats[8] = {kInvalidPlayer, kInvalidPlayer, kInvalidPlayer, kInvalidPlayer,
                             kInvalidPlayer, kInvalidPlayer, kInvalidPlayer, kInvalidPlayer};
        uint8_t seatCount = 0;
    };

    // --- W8 seat-driven turret aim (reserved TFMsg block 0x5448-0x544B) ---------
    // Ids + packed structs live HERE, outside the frozen TFMsg enum, following
    // the TFRepProtocol/TFSocialProtocol precedent. 0x5449-0x544B stay free for
    // this lane. Channel map:
    //   0x5448 TurretAim S->C unreliable (10 Hz active / 1 Hz keepalive; sent
    //          reliable once per vehicle in the late-join burst) TF_RepVehicleAim
    constexpr uint16_t kTFVehMsg_TurretAim = 0x5448;

#pragma pack(push, 1)
    /// Gunner turret aim for one vehicle. yaw16 is the WORLD-space turret yaw,
    /// [-pi, pi) mapped onto [0, 65535]. pitchDeg is camera-convention degrees
    /// (positive = down) clamped to the kTFTurretPitch* range ([-35, +10]).
    struct TF_RepVehicleAim
    {
        uint32_t entityId;
        uint16_t yaw16;
        int8_t pitchDeg;
        uint8_t _pad;
    };
    static_assert(sizeof(TF_RepVehicleAim) == 8, "wire layout frozen");
#pragma pack(pop)

    class TFVehicleSystem
    {
      public:
        TFVehicleSystem();
        ~TFVehicleSystem();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        // --- W3 cross-agent contract (consumed by TFServerSim / TFWeaponServer /
        //     TFPlayerSystem / TFSpawnScreen; wired by the orchestrator) ---------

        /// True while `player` occupies a seat (works on both roles). On the
        /// server it also stays true for one tick after exit so the exit
        /// placement reaches TFServerSim's MoveState (see file header).
        bool IsSeated(PlayerId player) const;

        /// Server: buy + spawn a vehicle at the nearest friendly terminal.
        /// Public so tf_* console commands can wire straight to it.
        bool ServerPurchaseVehicle(PlayerId player, VehicleId vehId);

        /// Server: TFServerSim routes TFMsg::VehicleEnter / VehicleExit here.
        void ServerHandleSeatOp(PlayerId player, const TF_VehicleSeatOp& op, bool enter);

        /// Server: TFServerSim routes TFMsg::AegisDeploy here.
        void ServerHandleAegisDeploy(PlayerId player, const TF_AegisDeploy& msg);

        /// Server: TFServerSim forwards a seated player's TF_ClientInput here
        /// instead of walking them (driver seat 0 drives; other seats no-op).
        void ServerHandleSeatedInput(PlayerId player, const TF_ClientInput& input, float dt);

        /// Server: pull the seated pawn's ride pose (or one-shot exit placement)
        /// into TFServerSim's MoveState. Clears the pending-exit latch.
        void SyncSeatedPawn(PlayerId player, float outPos[3], float outVel[3]);

        /// Server: nearest vehicle hit by the ray, or 0. Vehicles whose bounding
        /// sphere CONTAINS the origin are skipped (a gunner's muzzle sits inside
        /// the hull — own-vehicle shots must not self-hit at distance ~0).
        EntityId RaycastVehicles(const float origin[3], const float dir[3], float maxDist, float outHitPoint[3],
                                 float* outDist) const;

        /// Server: apply damage to a vehicle hp pool (friendly fire 50%).
        void ServerDamageVehicle(EntityId vehicle, float amount, EntityId attackerPawn, PlayerId attackerPlayer,
                                 WeaponId weapon);

        /// Server: radial splash against vehicles (TFWeaponServer explosion hook).
        /// `excludeVehicle` skips the direct-hit vehicle so it is not damaged twice.
        void ServerApplySplash(const float at[3], float radiusM, float damage, float vsVehicleMult,
                               EntityId attackerPawn, PlayerId attackerPlayer, WeaponId weapon,
                               EntityId excludeVehicle);

        /// Deployed + friendly + alive Aegis spawn pad (TFPlayerSystem spawnKind==2).
        bool GetAegisSpawnPos(EntityId vehicle, FactionId faction, float out[3]) const;

        /// Aegis respawn timer (vehicles.json deployRespawnSec; DESIGN §4 default 5 s).
        float AegisRespawnDelaySec() const;

        /// Seat weapon of the seat `player` occupies. Returns false when not
        /// seated; true with out==kInvalidWeapon for an unarmed seat.
        bool GetSeatWeapon(PlayerId player, WeaponId& out) const;

        /// Server (W8 turret aim): aimed turret muzzle frame for the
        /// turret-controller seat `player` occupies — world-space origin at the
        /// muzzle plus the unit aim direction, matching the replicated visual.
        /// False for passengers, non-controller seats and vehicles without a
        /// turret rig; the caller keeps the pawn-eye origin + client direction.
        bool GetSeatFireFrame(PlayerId player, float outOrigin[3], float outDir[3]) const;

        /// HUD: hp pool of the vehicle `player` is seated in (both roles).
        bool GetSeatedVehicleHp(PlayerId player, float& outCur, float& outMax) const;

        /// W10 HUD: which vehicle + seat index `player` occupies (both roles —
        /// authoritative seat map on servers, replicated seat tables on pure
        /// clients). False when not seated. Read-only; TFVehicleHUD's seam.
        bool GetSeatOf(PlayerId player, EntityId& outVehicle, uint8_t& outSeatIdx) const;

        /// Enumerate vehicles (authoritative records on server roles, replication
        /// mirror on pure clients). TFSpawnScreen builds Aegis entries from this.
        void ForEachVehicle(const std::function<void(const TFVehicleInfo&)>& fn) const;
        bool GetVehicleInfo(EntityId vehicle, TFVehicleInfo& out) const;

        /// Debug panel toggle (hidden by default; wired from tf_* console commands).
        void ToggleDebugUI() { m_showDebug = !m_showDebug; }

      private:
        // ------------------------------------------------------------------ types
        /// Local entity ids of a vehicle's aim-rig children (0 = not created).
        /// yawChild carries the aim yaw (Ravager turret; the Aegis PDW head is
        /// both), pitchChild the aim pitch (Ravager barrel / Aegis head — may
        /// alias yawChild), baseChild a static pedestal (Aegis PDW base).
        struct TurretRig
        {
            uint32_t yawChild = 0;
            uint32_t pitchChild = 0;
            uint32_t baseChild = 0;
        };

        /// Server-side authoritative vehicle record.
        struct VehicleRec
        {
            EntityId entity = 0; ///< network id == server ECS id
            uint32_t local = 0;  ///< local entt id (0 = none/headless test)
            VehicleId vehId = VehicleId::None;
            FactionId faction = FactionId::None;
            float pos[3]{0.0f, 0.0f, 0.0f};
            float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
            float speed = 0.0f; ///< signed forward speed, m/s
            float hp = 0.0f, maxHp = 0.0f;
            bool deployed = false;
            PlayerId seats[8] = {kInvalidPlayer, kInvalidPlayer, kInvalidPlayer, kInvalidPlayer,
                                 kInvalidPlayer, kInvalidPlayer, kInvalidPlayer, kInvalidPlayer};
            uint8_t seatCount = 0;
            // driver input cache (integrated once per fixed tick)
            float throttle = 0.0f, steer = 0.0f;
            float lift = 0.0f; ///< [-1,1] Jump/Crouch climb axis (VTOL hulls only)
            double lastDriveInput = -1.0e9;
            PlayerId lastAttacker = kInvalidPlayer;
            bool seatsDirty = false; ///< reliable seats broadcast pending
            // W8 turret aim (controller-seat view; see TurretControllerSeat)
            TurretRig rig;         ///< aim-rig ECS children (0s when headless)
            float aimYaw = 0.0f;   ///< world-space turret yaw, radians
            float aimPitch = 0.0f; ///< camera convention (positive = down), clamped
        };

        /// Server-side player -> seat mapping (+ one-tick exit latch).
        struct SeatRef
        {
            EntityId vehicle = 0;
            uint8_t seatIdx = 0;
            bool exiting = false; ///< exit placed; clear after one sync
            int exitTicks = 0;    ///< safety sweep if sync never runs
            float exitPos[3]{0.0f, 0.0f, 0.0f};
        };

        /// Client mirror of one replicated vehicle (pure clients only).
        struct MirrorRec
        {
            TFVehicleInfo info;
            uint32_t local = 0; ///< local visual entity
            // interpolation targets (info.pos holds the rendered pose)
            float targetPos[3]{0.0f, 0.0f, 0.0f};
            float targetYaw = 0.0f, targetPitch = 0.0f, targetRoll = 0.0f;
            bool hasTarget = false;
            // W8 turret aim mirror (world-space yaw + camera-convention pitch)
            TurretRig rig;
            float aimYaw = 0.0f, aimPitch = 0.0f;
            float targetAimYaw = 0.0f, targetAimPitch = 0.0f;
        };

        /// Last broadcast state per vehicle (server dirty check, pawn-channel style).
        struct SentState
        {
            QuantPos pos;
            int16_t yaw10k = 0, pitch10k = 0, roll10k = 0;
            uint16_t health = 0;
            uint8_t deployed = 0;
            bool operator==(const SentState&) const = default;
        };

        /// Last broadcast turret aim per vehicle (10 Hz on change / 1 Hz keepalive).
        struct AimSent
        {
            uint16_t yaw16 = 0;
            int8_t pitchDeg = 0;
            double lastSend = -1.0e9;
        };

        // ------------------------------------------------------- server internals
        VehicleRec* FindRec(EntityId vehicle);
        const VehicleRec* FindRec(EntityId vehicle) const;
        const VehicleDef* DefOf(VehicleId id) const;
        float TerrainAt(float x, float z) const;
        float VehicleRadius(VehicleId id) const;
        void RidePos(const VehicleRec& v, float out[3]) const;
        void ExitPosFor(const VehicleRec& v, uint8_t seatIdx, float out[3]) const;
        bool FindTerminal(const float pawnPos[3], FactionId faction, float maxRangeM, float outPad[2]) const;
        uint32_t CreateVehicleEntity(const VehicleDef& def, FactionId faction, const float pos[3], float yaw,
                                     TurretRig& outRig);
        void WriteVehicleTransform(VehicleRec& v);
        // --- W8 turret aim ----------------------------------------------------
        /// Lowest seat index with a non-empty weaponKey — that seat aims the
        /// turret (Ravager: driver 0; Aegis: gunner 1). -1 = no armed seat / null.
        static int TurretControllerSeat(const VehicleDef* def);
        /// Create the hull-parented aim-rig children (data-driven turret mesh at
        /// def.turretPivot plus the per-vehicle extras: Ravager 90 mm barrel at
        /// turret-space (0,0.18,0.75); Aegis PDW pedestal + head on the roof
        /// ring) and record their ids. Shared by server + mirror visual paths.
        void AttachTurretRig(uint32_t hullLocal, const VehicleDef& def, FactionId faction, TurretRig& out);
        /// Write the aim onto the rig children's Transforms: yawChild local yaw =
        /// WrapPi(aimYaw - hullYaw); pitchChild rotation.x = aimPitch (camera
        /// convention, positive = down — same sign as Transform +X). Radians in.
        void ApplyTurretPose(const TurretRig& rig, float hullYaw, float aimYaw, float aimPitch);
        /// Destroy the rig children (incl. the grandchild barrel the hull
        /// child-sweep would miss) and zero the ids. Safe to call twice.
        void DestroyTurretRig(TurretRig& rig);
        void StepVehicle(VehicleRec& v, const VehicleDef* def, float dt);
        /// Jolt path (TFVehiclePhysics): read back the stepped hull pose and queue
        /// forces for the next step. False = no body — StepVehicle math fallback.
        bool StepVehicleJolt(VehicleRec& v, const VehicleDef* def);
        /// W13 damage-state: hull-hp-fraction speed/turn multipliers (critical
        /// tier only, see kTFVehCriticalHpFrac in the .cpp). Computed once and
        /// applied identically by StepVehicle (math) and StepVehicleJolt (Jolt)
        /// — the only two driving paths, both authority-only (pure clients only
        /// interpolate the replicated pose, per the VehicleLanded pure-client
        /// caveat above), so applying the same multiplier function to whichever
        /// path executes for a given tick is sufficient for determinism — there
        /// is no separate client-side prediction of vehicle movement to mirror.
        void DamageMovementMults(const VehicleRec& v, float& outSpeedMult, float& outTurnMult) const;
        /// VTOL (Vulture): hull base close enough to the ground under it to count
        /// as landed — exit is only allowed while landed. Physics-aware when the
        /// Jolt hull exists (pads/roofs count); analytic terrain fallback otherwise.
        bool VehicleLanded(const VehicleRec& v) const;
        /// Write/refresh the pawn's TFSeatComp ECS mirror (enter + W10 swap).
        void WriteSeatComp(PlayerId player, EntityId vehicle, uint8_t seatIdx);
        void UnseatPlayer(PlayerId player, bool placeBeside);
        void DestroyVehicle(VehicleRec& v, PlayerId destroyer);
        void OnPlayerKilled(const EvPlayerKilled& ev);
        void PlayOneShot(const std::string& assetsRelPath);

        // --- W13 persistent wreck (kills leave a mark) -------------------------
        /// Retint the given local entity's MeshRenderer charred/static and hand
        /// it to the wreck timer instead of destroying it immediately. Shared by
        /// DestroyVehicle (server hull) and OnNetVehDestroy (client mirror hull)
        /// — both already removed the entity from m_vehicles/m_mirror by the
        /// time this runs, so it stops being a "vehicle" to every other query
        /// (damage, raycast, ForEachVehicle, TFGroundFx/TFVehicleFx) the instant
        /// it becomes a wreck. No-op on an invalid/zero local id.
        void SpawnWreck(uint32_t local);
        /// Despawns wrecks whose kTFVehWreckLifeSec timer has elapsed. Called
        /// once per Update (both roles — wrecks can exist on pure clients too).
        void UpdateWrecks();

        // ---------------------------------------------- net half (TFVehicleNet.cpp)
#ifdef ENABLE_NETWORKING
        bool ServerNetActive() const;
        bool ClientNetActive() const;
        void ServerEnsureNetHandlers();
        void ServerReleaseNetHandlers();
        void ServerPollNewClients();
        void ServerBroadcastUpdates();
        void ServerSendCreate(PlayerId target, const VehicleRec& v);
        void ServerSendSeats(PlayerId target, const VehicleRec& v);
        void ServerSendAim(PlayerId target, const VehicleRec& v, bool reliable);
        void ServerBroadcastAim(); ///< 10 Hz on-change / 1 Hz keepalive throttle
        void ServerSendDestroy(EntityId vehicle);
        void SendNet(PlayerId target, uint16_t msgId, const void* payload, size_t size, bool reliable);
        void ClientEnsureHandlers();
        void ClientReleaseHandlers();
        void OnNetVehCreate(const void* data, size_t size);
        void OnNetVehUpdate(const void* data, size_t size);
        void OnNetVehDestroy(const void* data, size_t size);
        void OnNetVehSeats(const void* data, size_t size);
        void OnNetVehAim(const void* data, size_t size);
        void OnNetPurchase(PlayerId sender, const void* data, size_t size);
#endif
        void NetFixedUpdate(float fdt);   ///< role polling + 20 Hz broadcast driver
        void ClientInterpolate(float dt); ///< smooth mirror visuals (pure client)
        void ClientDropMirror();
        void MirrorAttachVisual(MirrorRec& m);

        // ------------------------------------------- client UX (TFVehicleClient.cpp)
        void ClientUpdateUX(float dt);
        void ClientRequestSeatOp(EntityId vehicle, uint8_t seatIdx, bool enter);
        void ClientRequestDeploy(EntityId vehicle, bool deploy);
        void ClientRequestPurchase(VehicleId vehId);
        bool LocalPlayerPawn(float outPos[3], bool& outAlive) const;
        PlayerId LocalPlayerId() const;
        void RenderPromptsAndMenus(); ///< SPARK_HAS_IMGUI-only body
        void SetShopOpen(bool open);

        // -------------------------------------------------------------------- state
        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};
        double m_clock{0.0};

        // server state
        std::unique_ptr<TFVehiclePhysics> m_joltDrive; ///< non-null only with a live Jolt world
        std::vector<VehicleRec> m_vehicles;
        std::unordered_map<PlayerId, SeatRef> m_seatOf;
        std::unordered_map<EntityId, SentState> m_lastSent;
        std::unordered_map<EntityId, AimSent> m_lastAimSent; ///< W8 turret aim throttle
        std::unordered_set<PlayerId> m_knownClients;         ///< server join diff
        float m_updateAccum{0.0f};
        bool m_serverHandlers{false};

        // client mirror state (pure clients)
        std::unordered_map<EntityId, MirrorRec> m_mirror;
        std::unordered_map<EntityId, TF_RepVehicleSeats> m_mirrorSeats;
        bool m_clientHandlers{false};

        // client UX state
        std::unique_ptr<TFVehicleHUD> m_vehicleHud; ///< W10 cockpit widget (lazy, ImGui builds only)
        bool m_shopOpen{false};
        float m_shopTerminal[2]{0.0f, 0.0f};
        float m_interactDebounce{0.0f};
        EntityId m_promptVehicle{0}; ///< nearest enterable vehicle this frame
        uint8_t m_promptSeat{0};

        // audio (one-shot explode sfx; PlayWeaponAudio pattern)
        std::unordered_set<std::string> m_loadedSounds;

        // W13 persistent wrecks (kills leave a mark; see SpawnWreck/UpdateWrecks)
        struct WreckRec
        {
            uint32_t local = 0;
            double expireAt = 0.0;
        };
        std::vector<WreckRec> m_wrecks;

        // stats / debug
        uint32_t m_purchases{0};
        uint32_t m_purchasesRejected{0};
        uint32_t m_seatOps{0};
        uint32_t m_destroyed{0};
        uint32_t m_badPackets{0};
        bool m_showDebug{false};
    };

} // namespace Terrafront
