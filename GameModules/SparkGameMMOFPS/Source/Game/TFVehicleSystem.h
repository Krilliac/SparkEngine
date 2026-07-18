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
 * Umbrella: tuning constants / TFVehicleInfo / turret-aim wire protocol live
 * in Game/TFVehicleSystemTypes.h, private record structs (+ W3-W13 narrative)
 * in Game/TFVehicleSystemRecords.h — both included back, no includer changes.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFRepProtocol.h"
#include "Game/TFVehicleSystemTypes.h"
#include "Game/TFVehicleSystemRecords.h"

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
        // Record structs live in Game/TFVehicleSystemRecords.h; aliased back so
        // the split .cpp parts' spellings (TFVehicleSystem::VehicleRec) still work.
        using TurretRig = VehicleDetail::TurretRig;
        using VehicleRec = VehicleDetail::VehicleRec;
        using SeatRef = VehicleDetail::SeatRef;
        using MirrorRec = VehicleDetail::MirrorRec;
        using SentState = VehicleDetail::SentState;
        using AimSent = VehicleDetail::AimSent;
        using WreckRec = VehicleDetail::WreckRec;

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
