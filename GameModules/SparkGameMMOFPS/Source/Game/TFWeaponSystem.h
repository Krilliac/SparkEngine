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
 * Ballistics lane: Game/TFBallistics.h adds physics-backed world occlusion +
 * hit registration (null-safe when GetPhysics() is unavailable), head/limb
 * hit-zone multipliers, splash line-of-sight, and projectile gravity
 * integration. Projectile-vs-hitscan routing stays data-driven (weapons.json
 * projSpeed) — snipers/HLX/launchers fly with drop + travel time, ballistic
 * rifles stay hitscan.
 */
#pragma once

#include "Core/TFEvents.h"
#include "Core/TFTypes.h"
#include "Data/TFDataTables.h"
#include "Net/TFNetProtocol.h"

#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Terrafront
{

    struct PawnInfo; // defined in Game/TFPlayerSystem.h (frozen W1 contract)

    class TFWeaponSystem
    {
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

        /// Client: OBJ model path of the active weapon (empty if none/dead) — the
        /// first-person viewmodel drawn at the camera in TFWorldSetup::RenderWorld.
        std::string ActiveWeaponModel() const;

        // --- W11 weapon-optics additions (client presentation reads) ---

        /// Client: resolved def (faction traits applied) of the active slot, or
        /// nullptr when the local player has no pawn/valid slot. The optics lane
        /// (Game/TFOpticsSystem) reads key/slot/adsSec for sight presentation —
        /// spread/recoil/damage stay server-validated data truth. Pointer is
        /// into slot state: do not hold across frames.
        const WeaponDef* ActiveWeaponDefLocal() const;

        /// Client: local aim-down-sights state (RMB; the same flag the spread
        /// pick in ClientTriggerFire uses). Presentation consumers only.
        bool IsAdsLocal() const { return m_ads; }

        // --- W8 audio-polish additions (remote/distant gunfire) ---

        /// Local-audio seam for OTHER shooters: called from ServerHandleFire for
        /// every VALIDATED fire event so the local client hears bots + remote
        /// players. Distance-bucketed: <60 m plays the weapon's own fire clip at
        /// reduced volume, 60-600 m plays the faction's distant_fire_* tail
        /// (concurrent tails capped), beyond that nothing. No-op on dedicated
        /// servers (no local player / no audio device) and for the local shooter
        /// (ClientTriggerFire already played the first-person clip).
        /// NOTE: only reaches remote-HUMAN fire on Standalone/ListenHost — pure
        /// clients have no per-shot wire message (see wave report).
        void ClientOnRemoteFire(PlayerId shooter, const PawnInfo& pawn, const WeaponDef& def);

        /// 0..1 decaying "someone is shooting near me" heat for the ambience
        /// system's combat layer (TFAudioAmbience). Client-presentation only.
        float RemoteFireHeat() const { return m_remoteFireHeat; }

        // --- W10 sanctuary-v2 addition (client presentation seam) ---

        /// Local-shot presentation hook: invoked once per LOCAL shot that
        /// actually fires (after the RoF/ammo/reload gates, with the same
        /// post-spread ray ClientTriggerFire puts in TF_FireEvent) plus the
        /// resolved weapon def. COSMETIC ONLY — consumers must never feed this
        /// back into damage/movement/net; the server stays the only damage
        /// authority. Consumer: Game/TFTargetRange (sanctuary range dummies).
        /// Single-slot (last setter wins); cleared by Shutdown.
        using LocalShotHook = std::function<void(const float origin[3], const float dir[3], const WeaponDef& def)>;
        void SetLocalShotHook(LocalShotHook hook) { m_localShotHook = std::move(hook); }

      private:
        static constexpr EntityId kNoPawnEntity = 0xFFFFFFFFu;

        // Client weapon slots (W1 default class loadout; TF-W3: TF_LoadoutChange).
        enum Slot : int
        {
            SlotPrimary = 0,
            SlotSecondary,
            SlotTool,
            SlotMelee,
            SlotCount
        };

        struct SlotState
        {
            WeaponId weapon = kInvalidWeapon;
            WeaponDef def{}; // resolved with faction traits (TFDataTables::ResolveWeapon)
            int magAmmo = 0;
            int reserveAmmo = 0;
            bool IsValid() const { return weapon != kInvalidWeapon; }
        };

        // Server-side per-weapon fire validation state (RoF token bucket + approx
        // mag). SECURITY: this is keyed per weapon id inside ShooterState (below)
        // so alternating between weapon ids cannot refill/reset the rate limiter
        // -- each weapon's bucket persists independently across weapon switches.
        struct WeaponFireState
        {
            float tokens = 2.0f; // RoF token bucket (burst tolerance 2)
            double lastRefill = 0.0;
            int mag = 0; // approximate server mag (TF-W2: explicit reload msg)
            double magEmptyTime = -1.0e9;
            double lastShotTime = -1.0e9;
        };

        // Server-side per-shooter fire validation state.
        struct ShooterState
        {
            std::unordered_map<WeaponId, WeaponFireState> perWeapon;
            // W9 remote-fire-events: last 0x54F4 TF_RemoteFireFx actually sent
            // for this shooter (rate cap kTFRemoteFireFxMinIntervalSec; only
            // advanced when a client in range received one).
            double lastFireFxSent = -1.0e9;
            // W11 impact-broadcast: last 0x54F5 TF_ImpactFx actually delivered
            // for this shooter (rate cap kTFImpactFxMinIntervalSec; same
            // advance-on-delivery semantics as lastFireFxSent).
            double lastImpactFxSent = -1.0e9;
        };

        // Server-simulated projectile (rockets, energy bolts, sniper rounds).
        struct ServerProjectile
        {
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
        void PlayWeaponAudio(const std::string& assetPath, float volume = 0.8f);
        WeaponId FindWeaponForSlotKey(const std::string& slotKey, FactionId faction) const;
        WeaponId FindToolWeapon(const std::string& toolKey) const;

        // --- remote/distant fire audio (W8 audio-polish, TFWeaponSystemRemoteFire.cpp) ---
        bool LocalListenerPos(float out[3]) const;
        static const char* DistantTailFor(FactionId faction);

        // --- server side (TFWeaponServer.cpp) ---
        double ServerNow() const;
        // SECURITY: the fired weapon id must resolve to something the shooter's
        // class loadout actually grants (primary/secondary/tool/melee) -- a
        // client can put ANY WeaponId in TF_FireEvent, so this must be checked
        // server-side before the id is trusted for damage/def lookups.
        bool IsWeaponInLoadout(WeaponId fireWeapon, const PawnInfo& pawn) const;
        bool ValidateFire(const WeaponDef& def, ShooterState& st, double now);
        /// Returns true when the ray registered damage on a pawn or vehicle
        /// (W6 progression: per-fire-event hit stat, deduped across pellets).
        bool FireHitscanRay(PlayerId shooter, const PawnInfo& pawn, const WeaponDef& def, const float origin[3],
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
        // W9 remote-fire-events (TFWeaponServer.cpp broadcast section): send
        // 0x54F4 TF_RemoteFireFx (Net/TFFireFxProtocol.h) for a VALIDATED fire
        // to every connected client with a pawn within kTFRemoteFireFxRangeM
        // of the muzzle, EXCEPT the shooter. Rate-capped per shooter via
        // st.lastFireFxSent; no-op when NetworkManager is not a server
        // (Standalone) or no client is in range. Pure clients route it to
        // TFAudioAmbience::ClientOnRemoteFire (flash + distant tail + heat).
        void ServerBroadcastRemoteFireFx(ShooterState& st, PlayerId shooter, EntityId shooterPawn, WeaponId weapon,
                                         const float muzzle[3], const float dir[3]);
        // W11 impact-broadcast (TFWeaponServer.cpp impact broadcast section):
        // deliver the AUTHORITATIVE impact point of a validated shot — 0x54F5
        // TF_ImpactFx (Net/TFFireFxProtocol.h) to clients with a pawn within
        // kTFImpactFxRangeM of the point, plus the in-process listen-host/
        // standalone route straight into TFImpactFx — always EXCEPT the
        // shooter (their puff is the immediate OnLocalShot prediction).
        // Rate-capped per shooter via st.lastImpactFxSent. `surface` is a
        // TFImpactSurface value as uint8_t.
        void ServerBroadcastImpactFx(PlayerId shooter, const float point[3], uint8_t surface);
        /// True when the shooter's 0x54F5 rate-cap window is open (cheap
        /// pre-gate so miss-path terrain marches are skipped while capped).
        bool ImpactFxCapOpen(PlayerId shooter) const;
        /// Refined terrain-impact distance along the ray (TerrainBlocked's
        /// march + bisection), or a negative value when the ray stays clear
        /// within `dist`.
        float TerrainHitT(const float origin[3], const float dir[3], float dist) const;

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        // client state
        SlotState m_slots[SlotCount];
        int m_activeSlot = SlotPrimary;
        double m_clock = 0.0; // client-local seconds
        double m_nextFireTime = 0.0;
        double m_swapEndTime = 0.0;
        bool m_reloading = false;
        double m_reloadEndTime = 0.0;
        bool m_ads = false;
        uint32_t m_fireSeq = 0; // TF-W2: anchor to TFClientNet input sequence
        EntityId m_localPawn = kNoPawnEntity;
        std::unordered_set<std::string> m_loadedSounds;

        // remote/distant fire audio (W8 audio-polish; client presentation only)
        static constexpr int kMaxDistantOneShots = 6; // concurrent-tail cap
        // Ring of tail start times, seeded far in the past so the cap never
        // miscounts the zero-initialized slots against a near-zero m_clock.
        double m_distantPlayTimes[kMaxDistantOneShots] = {-1.0e9, -1.0e9, -1.0e9, -1.0e9, -1.0e9, -1.0e9};
        int m_distantPlayCursor = 0;
        uint32_t m_remoteFireSeq = 0;  // round-robin over the shooter's fire variants
        float m_remoteFireHeat = 0.0f; // see RemoteFireHeat()

        // W10 sanctuary-v2: cosmetic local-shot seam (see SetLocalShotHook).
        LocalShotHook m_localShotHook;

        // server state
        std::unordered_map<PlayerId, ShooterState> m_shooters;
        std::vector<ServerProjectile> m_projectiles;
        double m_serverClock = 0.0;  // fallback time when ctx.serverSim is null
        uint32_t m_fireAudioSeq = 0; // round-robin index into WeaponDef.audioFireVariants
        uint32_t m_shotsValidated = 0;
        uint32_t m_shotsRejected = 0;

        std::mt19937 m_rng{0xC0FFEEu};
    };

} // namespace Terrafront
