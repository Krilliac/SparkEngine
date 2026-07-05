/**
 * @file TFWeaponSystem.h
 * @brief Data-driven weapons: hitscan + projectile, ADS/spread, ammo/reload.
 *
 * OWNERSHIP: this header + TFWeaponSystem.cpp + TFWeaponServer.cpp belong to
 * ONE implementation agent. The lifecycle below is the frozen module contract
 * (called from Main.cpp) — extend this class freely, but do not change the
 * lifecycle signatures.
 *
 * W1: client fire loop (RoF gate, mag/reserve, reload, ADS spread, TF_FireEvent,
 * fire audio) + server validation (rate token bucket, ammo, lag-compensated
 * hitscan with pellets, server-simulated projectiles with splash).
 * TF-W4: full 21-weapon table polish, tracers/muzzle flash, remote fire audio.
 */
#pragma once

#include "Core/TFEvents.h"
#include "Core/TFTypes.h"
#include "Data/TFDataTables.h"
#include "Net/TFNetProtocol.h"

#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Terrafront {

struct PawnInfo; // defined in Game/TFPlayerSystem.h (frozen W1 contract)

class TFWeaponSystem {
  public:
    TFWeaponSystem();
    ~TFWeaponSystem();

    bool Initialize(TFGameContext& ctx, TFEventBus& events);
    void Update(float deltaTime);
    void FixedUpdate(float fixedDeltaTime);
    void Shutdown();
    void RenderDebugUI();

    // --- frozen cross-system API (DESIGN.md / W1 contract) ---

    /// Server: validate + resolve a client TF_FireEvent (rate, ammo, hit rewind).
    void ServerHandleFire(PlayerId shooter, const TF_FireEvent& ev);

    /// Client: fire the local player's active weapon (fx + TF_FireEvent send).
    /// Call once per desired shot; the internal RoF timer paces full-auto.
    void ClientTriggerFire();

  private:
    static constexpr EntityId kNoPawnEntity = 0xFFFFFFFFu;

    // Client weapon slots (W1 default class loadout; TF-W3: TF_LoadoutChange).
    enum Slot : int { SlotPrimary = 0, SlotSecondary, SlotTool, SlotMelee, SlotCount };

    struct SlotState {
        WeaponId  weapon = kInvalidWeapon;
        WeaponDef def{};  // resolved with faction traits (TFDataTables::ResolveWeapon)
        int       magAmmo = 0;
        int       reserveAmmo = 0;
        bool IsValid() const { return weapon != kInvalidWeapon; }
    };

    // Server-side per-shooter fire validation state.
    struct ShooterState {
        WeaponId weapon = kInvalidWeapon;
        float    tokens = 2.0f;       // RoF token bucket (burst tolerance 2)
        double   lastRefill = 0.0;
        int      mag = 0;             // approximate server mag (TF-W2: explicit reload msg)
        double   magEmptyTime = -1.0e9;
        double   lastShotTime = -1.0e9;
    };

    // Server-simulated projectile (rockets, energy bolts, sniper rounds).
    struct ServerProjectile {
        PlayerId shooter = kInvalidPlayer;
        EntityId shooterPawn = kNoPawnEntity;
        WeaponId weapon = kInvalidWeapon;
        float pos[3]{};
        float vel[3]{};
        float gravityFactor = 1.0f;
        float damage = 0.0f, minDamage = 0.0f;
        float falloffStartM = 999.0f, falloffEndM = 999.0f;
        float headshotMult = 1.0f;
        float splashRadiusM = 0.0f, splashDamage = 0.0f;
        float traveledM = 0.0f;
        float lifeSec = 0.0f;
    };

    // --- client side (TFWeaponSystem.cpp) ---
    void RefreshLocalLoadout();
    void PollClientInput();
    void UpdateReload();
    void StartReload();
    void SwitchSlot(int slotIdx);
    bool BuildViewRay(const PawnInfo& pawn, float outOrigin[3], float outDir[3]) const;
    void PlayWeaponAudio(const std::string& assetPath);
    WeaponId FindWeaponForSlotKey(const std::string& slotKey, FactionId faction) const;
    WeaponId FindToolWeapon(const std::string& toolKey) const;

    // --- server side (TFWeaponServer.cpp) ---
    double ServerNow() const;
    bool ValidateFire(const WeaponDef& def, ShooterState& st, double now);
    void FireHitscanRay(PlayerId shooter, const PawnInfo& pawn, const WeaponDef& def, const float origin[3],
                        const float dir[3], float maxDist, double rewindTime, uint8_t damageKind);
    void SpawnServerProjectile(PlayerId shooter, const PawnInfo& pawn, const WeaponDef& def, const float origin[3],
                               const float dir[3]);
    void ServerStepProjectiles(float dt);
    // W3 shared-edit (vehicles agent): excludeVehicle skips the direct-hit
    // vehicle when the warhead splashes (no double dip); defaulted so the
    // pre-W3 call sites are untouched.
    void ExplodeAt(const ServerProjectile& p, const float at[3], EntityId excludeEntity,
                   EntityId excludeVehicle = 0);
    bool TerrainBlocked(const float origin[3], const float dir[3], float dist) const;
    EntityId RaycastPawnsNow(const float origin[3], const float dir[3], float maxDist, EntityId ignore,
                             float outHitPoint[3], float* outDist) const;

    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};

    // client state
    SlotState m_slots[SlotCount];
    int       m_activeSlot = SlotPrimary;
    double    m_clock = 0.0;         // client-local seconds
    double    m_nextFireTime = 0.0;
    double    m_swapEndTime = 0.0;
    bool      m_reloading = false;
    double    m_reloadEndTime = 0.0;
    bool      m_ads = false;
    uint32_t  m_fireSeq = 0;         // TF-W2: anchor to TFClientNet input sequence
    EntityId  m_localPawn = kNoPawnEntity;
    std::unordered_set<std::string> m_loadedSounds;

    // server state
    std::unordered_map<PlayerId, ShooterState> m_shooters;
    std::vector<ServerProjectile>              m_projectiles;
    double   m_serverClock = 0.0;    // fallback time when ctx.serverSim is null
    uint32_t m_shotsValidated = 0;
    uint32_t m_shotsRejected = 0;

    std::mt19937 m_rng{0xC0FFEEu};
};

} // namespace Terrafront
