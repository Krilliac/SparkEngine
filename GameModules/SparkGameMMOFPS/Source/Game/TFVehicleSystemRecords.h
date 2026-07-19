/**
 * @file TFVehicleSystemRecords.h
 * @brief Record structs behind TFVehicleSystem, split from TFVehicleSystem.h
 *        (the umbrella includes this back and aliases them as its private
 *        nested types, so no includer or .cpp spelling changes) — plus the
 *        W3/W8/W10/W13 implementation narrative those records embody.
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

#include "Game/TFVehicleSystemTypes.h"
#include "Net/TFRepProtocol.h"

#include <cstdint>

namespace Terrafront
{
    namespace VehicleDetail
    {

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

        /// W13 persistent-wreck timer record (see SpawnWreck/UpdateWrecks).
        struct WreckRec
        {
            uint32_t local = 0;
            double expireAt = 0.0;
        };

    } // namespace VehicleDetail
} // namespace Terrafront
