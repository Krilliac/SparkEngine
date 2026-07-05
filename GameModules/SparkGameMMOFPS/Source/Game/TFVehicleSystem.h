/**
 * @file TFVehicleSystem.h
 * @brief Drifter/Aegis/Ravager: seats, driving, vehicle weapons, Aegis
 *        deploy-spawn (W3 combined-arms core).
 *
 * OWNERSHIP: this header + TFVehicleSystem.cpp + TFVehicleNet.cpp +
 * TFVehicleClient.cpp belong to ONE implementation agent. The lifecycle below
 * is the frozen module contract (called from Main.cpp) — extend this class
 * freely, but do not change the lifecycle signatures.
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
 *  Client UX (RenderDebugUI hook, SPARK_HAS_IMGUI): terminal purchase menu (E),
 *   enter/exit prompts (E), Aegis deploy toggle (B), vehicle hp bar while
 *   seated. Pure-client caveat (documented): walking prediction keeps running
 *   while seated, so the first-person view reconciles toward the ride pose at
 *   20 Hz instead of predicting it — listen-host/standalone is smooth.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFRepProtocol.h"

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Terrafront {

struct VehicleDef;   // Data/TFDataTables.h
struct RegionDef;    // Data/TFDataTables.h

// W3 vehicle tuning constants (engine-side limits; balance lives in vehicles.json).
constexpr float kTFVehTerminalRangeM   = 25.0f;  ///< purchase reach from a terminal
constexpr float kTFVehTerminalPromptM  = 8.0f;   ///< client prompt / menu-open reach
constexpr float kTFVehEnterRangeM      = 4.0f;   ///< enter reach from the hull
constexpr float kTFVehExplodeRadiusM   = 6.0f;   ///< destruction splash radius
constexpr float kTFVehExplodeDamage    = 400.0f; ///< destruction splash at center
constexpr float kTFVehEjectDamageFrac  = 0.5f;   ///< of max health+shield on destruction
constexpr float kTFVehDeployMaxSpeed   = 1.0f;   ///< m/s — must be ~stopped to deploy
constexpr uint16_t kTFVehKillXP        = 300;

/// Shared snapshot of one vehicle — served from the authoritative records on
/// server roles and from the replication mirror on pure clients (same
/// pattern as TFRegionSystem's accessors).
struct TFVehicleInfo {
    EntityId  entity   = 0;
    VehicleId vehId    = VehicleId::None;
    FactionId faction  = FactionId::None;
    float     pos[3]   = {0.0f, 0.0f, 0.0f};
    float     yaw      = 0.0f;              ///< radians
    float     hp       = 0.0f;
    float     maxHp    = 0.0f;
    bool      deployed = false;
    PlayerId  seats[8] = { kInvalidPlayer, kInvalidPlayer, kInvalidPlayer, kInvalidPlayer,
                           kInvalidPlayer, kInvalidPlayer, kInvalidPlayer, kInvalidPlayer };
    uint8_t   seatCount = 0;
};

class TFVehicleSystem {
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
    EntityId RaycastVehicles(const float origin[3], const float dir[3], float maxDist,
                             float outHitPoint[3], float* outDist) const;

    /// Server: apply damage to a vehicle hp pool (friendly fire 50%).
    void ServerDamageVehicle(EntityId vehicle, float amount, EntityId attackerPawn,
                             PlayerId attackerPlayer, WeaponId weapon);

    /// Server: radial splash against vehicles (TFWeaponServer explosion hook).
    /// `excludeVehicle` skips the direct-hit vehicle so it is not damaged twice.
    void ServerApplySplash(const float at[3], float radiusM, float damage,
                           float vsVehicleMult, EntityId attackerPawn,
                           PlayerId attackerPlayer, WeaponId weapon,
                           EntityId excludeVehicle);

    /// Deployed + friendly + alive Aegis spawn pad (TFPlayerSystem spawnKind==2).
    bool GetAegisSpawnPos(EntityId vehicle, FactionId faction, float out[3]) const;

    /// Aegis respawn timer (vehicles.json deployRespawnSec; DESIGN §4 default 5 s).
    float AegisRespawnDelaySec() const;

    /// Seat weapon of the seat `player` occupies. Returns false when not
    /// seated; true with out==kInvalidWeapon for an unarmed seat.
    bool GetSeatWeapon(PlayerId player, WeaponId& out) const;

    /// HUD: hp pool of the vehicle `player` is seated in (both roles).
    bool GetSeatedVehicleHp(PlayerId player, float& outCur, float& outMax) const;

    /// Enumerate vehicles (authoritative records on server roles, replication
    /// mirror on pure clients). TFSpawnScreen builds Aegis entries from this.
    void ForEachVehicle(const std::function<void(const TFVehicleInfo&)>& fn) const;
    bool GetVehicleInfo(EntityId vehicle, TFVehicleInfo& out) const;

    /// Debug panel toggle (hidden by default; wired from tf_* console commands).
    void ToggleDebugUI() { m_showDebug = !m_showDebug; }

  private:
    // ------------------------------------------------------------------ types
    /// Server-side authoritative vehicle record.
    struct VehicleRec {
        EntityId  entity = 0;            ///< network id == server ECS id
        uint32_t  local  = 0;            ///< local entt id (0 = none/headless test)
        VehicleId vehId  = VehicleId::None;
        FactionId faction = FactionId::None;
        float pos[3]{0.0f, 0.0f, 0.0f};
        float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
        float speed = 0.0f;              ///< signed forward speed, m/s
        float hp = 0.0f, maxHp = 0.0f;
        bool  deployed = false;
        PlayerId seats[8] = { kInvalidPlayer, kInvalidPlayer, kInvalidPlayer, kInvalidPlayer,
                              kInvalidPlayer, kInvalidPlayer, kInvalidPlayer, kInvalidPlayer };
        uint8_t  seatCount = 0;
        // driver input cache (integrated once per fixed tick)
        float  throttle = 0.0f, steer = 0.0f;
        double lastDriveInput = -1.0e9;
        PlayerId lastAttacker = kInvalidPlayer;
        bool  seatsDirty = false;        ///< reliable seats broadcast pending
    };

    /// Server-side player -> seat mapping (+ one-tick exit latch).
    struct SeatRef {
        EntityId vehicle = 0;
        uint8_t  seatIdx = 0;
        bool     exiting = false;        ///< exit placed; clear after one sync
        int      exitTicks = 0;          ///< safety sweep if sync never runs
        float    exitPos[3]{0.0f, 0.0f, 0.0f};
    };

    /// Client mirror of one replicated vehicle (pure clients only).
    struct MirrorRec {
        TFVehicleInfo info;
        uint32_t local = 0;              ///< local visual entity
        // interpolation targets (info.pos holds the rendered pose)
        float targetPos[3]{0.0f, 0.0f, 0.0f};
        float targetYaw = 0.0f, targetPitch = 0.0f, targetRoll = 0.0f;
        bool  hasTarget = false;
    };

    /// Last broadcast state per vehicle (server dirty check, pawn-channel style).
    struct SentState {
        QuantPos pos;
        int16_t  yaw10k = 0, pitch10k = 0, roll10k = 0;
        uint16_t health = 0;
        uint8_t  deployed = 0;
        bool operator==(const SentState&) const = default;
    };

    // ------------------------------------------------------- server internals
    VehicleRec* FindRec(EntityId vehicle);
    const VehicleRec* FindRec(EntityId vehicle) const;
    const VehicleDef* DefOf(VehicleId id) const;
    float TerrainAt(float x, float z) const;
    float VehicleRadius(VehicleId id) const;
    void  RidePos(const VehicleRec& v, float out[3]) const;
    void  ExitPosFor(const VehicleRec& v, uint8_t seatIdx, float out[3]) const;
    bool  FindTerminal(const float pawnPos[3], FactionId faction, float maxRangeM,
                       float outPad[2]) const;
    uint32_t CreateVehicleEntity(const VehicleDef& def, FactionId faction,
                                 const float pos[3], float yaw);
    void  WriteVehicleTransform(VehicleRec& v);
    void  StepVehicle(VehicleRec& v, const VehicleDef* def, float dt);
    void  UnseatPlayer(PlayerId player, bool placeBeside);
    void  DestroyVehicle(VehicleRec& v, PlayerId destroyer);
    void  OnPlayerKilled(const EvPlayerKilled& ev);
    void  PlayOneShot(const std::string& assetsRelPath);

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
    void ServerSendDestroy(EntityId vehicle);
    void SendNet(PlayerId target, uint16_t msgId, const void* payload,
                 size_t size, bool reliable);
    void ClientEnsureHandlers();
    void ClientReleaseHandlers();
    void OnNetVehCreate(const void* data, size_t size);
    void OnNetVehUpdate(const void* data, size_t size);
    void OnNetVehDestroy(const void* data, size_t size);
    void OnNetVehSeats(const void* data, size_t size);
    void OnNetPurchase(PlayerId sender, const void* data, size_t size);
#endif
    void NetFixedUpdate(float fdt);      ///< role polling + 20 Hz broadcast driver
    void ClientInterpolate(float dt);    ///< smooth mirror visuals (pure client)
    void ClientDropMirror();
    void MirrorAttachVisual(MirrorRec& m);

    // ------------------------------------------- client UX (TFVehicleClient.cpp)
    void ClientUpdateUX(float dt);
    void ClientRequestSeatOp(EntityId vehicle, uint8_t seatIdx, bool enter);
    void ClientRequestDeploy(EntityId vehicle, bool deploy);
    void ClientRequestPurchase(VehicleId vehId);
    bool LocalPlayerPawn(float outPos[3], bool& outAlive) const;
    PlayerId LocalPlayerId() const;
    void RenderPromptsAndMenus();        ///< SPARK_HAS_IMGUI-only body
    void RenderSeatedHud();              ///< SPARK_HAS_IMGUI-only body
    void SetShopOpen(bool open);

    // -------------------------------------------------------------------- state
    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};
    double         m_clock{0.0};

    // server state
    std::vector<VehicleRec>                  m_vehicles;
    std::unordered_map<PlayerId, SeatRef>    m_seatOf;
    std::unordered_map<EntityId, SentState>  m_lastSent;
    std::unordered_set<PlayerId>             m_knownClients; ///< server join diff
    float    m_updateAccum{0.0f};
    bool     m_serverHandlers{false};

    // client mirror state (pure clients)
    std::unordered_map<EntityId, MirrorRec>  m_mirror;
    std::unordered_map<EntityId, TF_RepVehicleSeats> m_mirrorSeats;
    bool m_clientHandlers{false};

    // client UX state
    bool     m_shopOpen{false};
    float    m_shopTerminal[2]{0.0f, 0.0f};
    float    m_interactDebounce{0.0f};
    EntityId m_promptVehicle{0};             ///< nearest enterable vehicle this frame
    uint8_t  m_promptSeat{0};

    // audio (one-shot explode sfx; PlayWeaponAudio pattern)
    std::unordered_set<std::string> m_loadedSounds;

    // stats / debug
    uint32_t m_purchases{0};
    uint32_t m_purchasesRejected{0};
    uint32_t m_seatOps{0};
    uint32_t m_destroyed{0};
    uint32_t m_badPackets{0};
    bool     m_showDebug{false};
};

} // namespace Terrafront
