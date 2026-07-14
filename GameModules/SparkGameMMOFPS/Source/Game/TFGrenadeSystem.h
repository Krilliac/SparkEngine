/**
 * @file TFGrenadeSystem.h
 * @brief Frag grenades (W10): server-authoritative throw validation, ballistic
 *        simulation with terrain/world-static bounces, 2.5 s fuse, splash
 *        damage through TFDamageSystem, and replicated client visuals (dark
 *        sphere body + additive muzzle-sheet boom flipbook).
 *
 * OWNERSHIP: grenades lane (this header + TFGrenadeSystem.cpp + the additive
 * "frag_grenade" row in Assets/MMOFPS/Data/weapons.json).
 *
 * Design (TFAbilitySystem precedent: server registry + client mirror in one
 * class; wire ids + packed structs live HERE, not in TFNetProtocol.h):
 *  - SERVER (authority roles): per-player grenade counts seeded from
 *    classes.json ClassDef::grenades on EvPlayerSpawned (per LIFE — reset on
 *    every spawn, record dropped on death). A throw is validated (alive pawn,
 *    grenades remaining, kTFGrenadeThrowIntervalSec since the last throw,
 *    global kTFMaxLiveGrenades cap) then simulated at the fixed tick:
 *    Ballistics::IntegrateProjectile gravity arc, one RaycastFiltered
 *    (WorldStatic|Deployable) sweep per step for wall bounces, terrain-
 *    heightfield reflection with kTFGrenadeRestitution, and a rest state once
 *    the bounce energy is spent. At fuse expiry the splash mirrors
 *    TFWeaponSystem::ExplodeAt exactly: pawns via ForEachAlivePawn + linear
 *    falloff + Ballistics::SplashVisible LOS -> TFDamageSystem::
 *    ServerApplyDamage (explosive kind, frag_grenade WeaponId for killfeed),
 *    plus ServerSplashDamageDeployables and TFVehicleSystem::ServerApplySplash.
 *    Grenades deliberately do NOT bounce off pawns/vehicles (mask excludes
 *    them) — they pass through, like the reference game.
 *  - WIRE: reserved TFMsg block 0x5464-0x5467. C->S TF_GrenadeThrow rides
 *    TFServerSim::RouteClientMessage (enter-world-gated choke point, wiring
 *    snippet in the wave report). S->C spawn/update/boom are sent by this
 *    system to all clients (spawn/boom reliable, 10 Hz position updates
 *    unreliable, every struct <= 16 bytes, quantized positions). No
 *    late-joiner burst: a grenade lives 2.5 s, so a client joining mid-flight
 *    misses at most one already-airborne visual (documented gap, not a bug).
 *    Listen-host/standalone local player is fed by direct mirror calls
 *    (TFAbilitySystem::ServerBroadcastState pattern).
 *  - CLIENT: G-key edge -> throw request (same fullscreen-UI suppression gate
 *    set as TFAbilitySystem::ClientPollAbilityKey; key swappable via
 *    SetThrowKey — the TFKeybinds rebind seam hooks this without touching
 *    this lane). Mirror store extrapolates replicated grenades between 10 Hz
 *    updates with the same gravity constant and renders them from the
 *    TFWorldSetup::RenderWorld hook (wiring snippet): small dark spheres
 *    (opaque basic-shader draw) + a 4-frame additive camera-facing flipbook
 *    boom using Textures/MMOFPS/fx/muzzle_flash_common.png (TFGroundFx
 *    render recipe: SetBasicShaders/Additive/ReadOnly depth, restored after).
 *    Boom audio: Audio/MMOFPS/vehicles/explosion.wav via PlaySound3D.
 *    Client handlers self-register when the socket connects (TFAbilitySystem
 *    EnsureClientHandlers pattern) — no TFClientNet edits.
 *  - HUD: GetLocalGrenadeCount() feeds the ammo-cluster wiring snippet in
 *    TFHUD::DrawWeaponBox (count is authoritative via TF_GrenadeSpawn's
 *    `remaining` echo; reseeded locally from ClassDef::grenades on respawn).
 *  - BOTS: CanThrowGrenade/ServerBotThrowGrenade are the public server-side
 *    entry points for TFBotSystem (direct calls, same validation path,
 *    bypassing the enter-world gate exactly like bot spawn/fire calls).
 *
 * loadout-depth wave (progression lane): grenade KIND selection.
 *  - Every throw now resolves the thrower's weapons.json row from
 *    TFProgressionSystem::GetLoadout(player)->grenade (empty/invalid/
 *    not-unlocked all fall back to kTFGrenadeWeaponKey == frag_grenade,
 *    exactly the pre-wave behavior) instead of always FragDef() — see
 *    SelectedGrenadeDef. The def's additive "grenadeEffect" JSON field
 *    (own lazy parse, EnsureEffectTable — TFOpticsSystem precedent, NOT
 *    TFDataTables) selects the detonation behavior:
 *     - absent / "damage" (frag_grenade): unchanged splash-damage path.
 *     - "smoke" (smoke_grenade, splashDamage 0 in its data row so the
 *       existing damage pass already no-ops): ServerDetonate broadcasts
 *       TF_SmokeSpawn instead; presentation-only, no server state kept.
 *     - "flash" (flash_grenade, splashDamage 0 likewise): ServerDetonate
 *       walks ForEachAlivePawn with the same LOS+falloff recipe as splash
 *       damage, records each affected player's flash-until in m_flashedUntil
 *       (server truth, IsFlashed/FlashAlpha01 query it) and unicasts
 *       TF_FlashState to that player's owner so the HUD-adjacent overlay
 *       (RenderFlashOverlay, hooked next to TFOpticsSystem::RenderOverlay)
 *       can fade the screen white.
 *  - WIRE: rides the loadout-depth lane's reserved TFMsg block
 *    0x5488-0x548B (Game/TFProgressionSystem.h owns 0x5488; the S->C ids
 *    here are 0x5489 TF_FlashState and 0x548A TF_SmokeSpawn, unicast/
 *    broadcast respectively via the same ServerBroadcast/SendToOwner
 *    machinery as the W10 grenade ids).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs on Linux
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class GraphicsEngine;
class Mesh;

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Wire protocol — reserved TFMsg id block 0x5464-0x5467 (grenades lane).
    // TFNetProtocol.h (contended) only gains the C->S enum entry via the wave
    // wiringNotes; these constants keep this lane compiling standalone and
    // MUST stay value-identical to any future enum entries.
    // ---------------------------------------------------------------------------

    constexpr uint16_t kTFMsgGrenadeThrow = 0x5464;  // C->S  TF_GrenadeThrow (server validates)
    constexpr uint16_t kTFMsgGrenadeSpawn = 0x5465;  // S->C  TF_GrenadeSpawn (reliable)
    constexpr uint16_t kTFMsgGrenadeUpdate = 0x5466; // S->C  TF_GrenadeUpdate (~10 Hz, unreliable)
    constexpr uint16_t kTFMsgGrenadeBoom = 0x5467;   // S->C  TF_GrenadeBoom (reliable)

    // loadout-depth wave: riding the loadout-depth lane's reserved block
    // (Game/TFProgressionSystem.h owns 0x5488; see that header's wire-protocol
    // comment). Unicast/broadcast respectively, both reliable (rare events).
    constexpr uint16_t kTFMsgFlashState = 0x5489; // S->C  TF_FlashState (unicast to the flashed player)
    constexpr uint16_t kTFMsgSmokeSpawn = 0x548A; // S->C  TF_SmokeSpawn (broadcast, reliable)

    // ---------------------------------------------------------------------------
    // Tuning (fuse/interval/bounce are gameplay contract from the wave brief;
    // splash radius/damage/throw speed live in the weapons.json frag_grenade row)
    // ---------------------------------------------------------------------------

    constexpr float kTFGrenadeFuseSec = 2.5f;           ///< throw -> detonation
    constexpr float kTFGrenadeThrowIntervalSec = 1.0f;  ///< min seconds between throws per player
    constexpr float kTFGrenadeRestitution = 0.4f;       ///< bounce energy kept along the surface normal
    constexpr float kTFGrenadeTangentDamping = 0.8f;    ///< bounce energy kept along the surface tangent
    constexpr float kTFGrenadeRestSpeedMps = 1.0f;      ///< below this after a bounce the grenade rests
    constexpr float kTFGrenadeRadiusM = 0.09f;          ///< body radius (visual + terrain clearance)
    constexpr uint32_t kTFMaxLiveGrenades = 32;         ///< global live-simulation cap (server)
    constexpr float kTFGrenadeUpdatePeriodSec = 0.1f;   ///< ~10 Hz position updates
    constexpr float kTFGrenadeFallbackThrowMps = 17.0f; ///< used when the data row is missing/zero

    /// weapons.json key of the splash-stat row (ADDITIVE data entry, this lane).
    constexpr const char* kTFGrenadeWeaponKey = "frag_grenade";

    // loadout-depth wave: the two additional player-selectable grenade kinds
    // (weapons.json keys; additive rows, "grenadeEffect": "smoke"/"flash").
    // TFProgressionSystem::ValidGrenadeChoiceKey checks a loadout pick against
    // these three names (single source of truth to avoid drift).
    constexpr const char* kTFGrenadeKeySmoke = "smoke_grenade";
    constexpr const char* kTFGrenadeKeyFlash = "flash_grenade";

    // Detonation effect kinds (ServerDetonate branches on this; see
    // EnsureEffectTable's lazy "grenadeEffect" parse).
    constexpr uint8_t kTFGrenadeEffectDamage = 0; // frag_grenade (default when the field is absent)
    constexpr uint8_t kTFGrenadeEffectSmoke = 1;
    constexpr uint8_t kTFGrenadeEffectFlash = 2;

    constexpr float kTFFlashMaxDurationSec = 3.0f; ///< at point-blank (falloff to 0 at the splash edge)

    // ---------------------------------------------------------------------------
    // Quantization (TFFireFxProtocol precedent). Positions: 0.125 m steps in
    // int16 (covers ±4095 m; the shared world is [0,4096]^2 m). Velocities:
    // 0.25 m/s steps in int8 (±31.75 m/s; throw speed + fall stay inside).
    // ---------------------------------------------------------------------------

    constexpr float kTFGrenadePosScale = 8.0f;
    constexpr float kTFGrenadeVelScale = 4.0f;

    namespace GrenadeDetail
    {
        inline int16_t QuantPos(float v)
        {
            return static_cast<int16_t>(std::lround(std::clamp(v * kTFGrenadePosScale, -32767.0f, 32767.0f)));
        }

        inline int8_t QuantVel(float v)
        {
            return static_cast<int8_t>(std::lround(std::clamp(v * kTFGrenadeVelScale, -127.0f, 127.0f)));
        }
    } // namespace GrenadeDetail

#pragma pack(push, 1)

    /// C->S: throw request. The server trusts only the view ANGLES (dir is
    /// rebuilt with the TFWeaponSystem::BuildViewRay convention) — the origin
    /// is always the authoritative pawn eye, never client data.
    struct TF_GrenadeThrow
    {
        float viewYaw;   // radians
        float viewPitch; // radians
    };
    static_assert(sizeof(TF_GrenadeThrow) == 8, "wire layout frozen");

    /// S->C: a validated throw entered simulation. `remaining` is the
    /// thrower's authoritative per-life count AFTER this throw (HUD truth for
    /// the owning player; everyone else ignores it).
    struct TF_GrenadeSpawn
    {
        uint16_t grenadeId; // server-monotonic (wraps)
        int16_t posQX, posQY, posQZ;
        int8_t velQX, velQY, velQZ;
        uint8_t remaining;
        uint32_t player; // thrower PlayerId
    };
    static_assert(sizeof(TF_GrenadeSpawn) == 16, "wire layout frozen (<= 16 byte budget)");

    /// S->C: ~10 Hz position/velocity correction (unreliable; drops are free —
    /// clients extrapolate the arc between updates).
    struct TF_GrenadeUpdate
    {
        uint16_t grenadeId;
        int16_t posQX, posQY, posQZ;
        int8_t velQX, velQY, velQZ;
        uint8_t flags; // bit0: resting (client pins the body, stops extrapolating)
    };
    static_assert(sizeof(TF_GrenadeUpdate) == 12, "wire layout frozen (<= 16 byte budget)");

    /// S->C: detonation at the final position (boom flipbook + audio + erase).
    struct TF_GrenadeBoom
    {
        uint16_t grenadeId;
        int16_t posQX, posQY, posQZ;
    };
    static_assert(sizeof(TF_GrenadeBoom) == 8, "wire layout frozen (<= 16 byte budget)");

    /// S->C: unicast to the flashed player only. durationMs/intensityQ (0..255,
    /// linear falloff already baked in server-side) drive RenderFlashOverlay's
    /// fade; no positional data needed (self-only presentation).
    struct TF_FlashState
    {
        uint16_t durationMs;
        uint8_t intensityQ;
    };
    static_assert(sizeof(TF_FlashState) == 3, "wire layout frozen (<= 16 byte budget)");

    /// S->C: broadcast (visible to everyone, like a grenade boom). Position +
    /// radius quantized with the existing grenade scale; durationMs is the
    /// puff's client-side presentation lifetime.
    struct TF_SmokeSpawn
    {
        int16_t posQX, posQY, posQZ;
        uint16_t radiusQ; // meters * kTFGrenadePosScale
        uint16_t durationMs;
    };
    static_assert(sizeof(TF_SmokeSpawn) == 10, "wire layout frozen (<= 16 byte budget)");

#pragma pack(pop)

    struct WeaponDef; // Data/TFDataTables.h

    // ---------------------------------------------------------------------------
    // System
    // ---------------------------------------------------------------------------

    class TFGrenadeSystem
    {
      public:
        TFGrenadeSystem();
        ~TFGrenadeSystem();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        // --- server surface ------------------------------------------------------

        /// Server: TFServerSim routes TFMsg 0x5464 (GrenadeThrow) here
        /// (size-validates, then dispatches) — same one-line hook shape as
        /// TFAbilitySystem::ServerHandleAbilityMsgRaw.
        void ServerHandleThrowMsgRaw(PlayerId sender, const void* data, size_t size);

        /// Server: true when `player` could throw RIGHT NOW (alive pawn,
        /// grenades remaining, interval elapsed, live cap not hit). Bots-lane
        /// query — free to call every think tick.
        bool CanThrowGrenade(PlayerId player) const;

        /// Server: throw along `dirUnit` (normalized internally; rejected if
        /// degenerate). Full validation; returns true when a grenade spawned.
        /// Bots-lane entry point — bypasses the enter-world gate exactly like
        /// TFBotSystem's spawn/fire calls.
        bool ServerBotThrowGrenade(PlayerId player, const float dirUnit[3]);

        /// Server: grenades this player has left this life (-1 = no record).
        int GrenadesRemaining(PlayerId player) const;

        // --- client surface ------------------------------------------------------

        /// HUD (TFHUD wiring snippet): the LOCAL player's grenade count.
        /// -1 = unknown (no pawn yet / tables unloaded) — HUD hides the row.
        int GetLocalGrenadeCount() const { return m_localRemaining; }

        /// Rebind seam for the ui-keys lane (TFKeybinds::BindKey pattern —
        /// callable at runtime, no compile-time coupling). VK code; default 'G'.
        void SetThrowKey(int vk) { m_throwVk = vk; }

        /// Draw replicated grenade bodies + boom flipbooks. Called once per
        /// frame from TFWorldSetup::RenderWorld (wiring snippet) while
        /// BeginFrame is active, after the world/ECS passes. Restores the
        /// basic-path blend/depth/texture state it touches (TFGroundFx
        /// recipe; DirectX types come from Core/Platform.h stubs off-Windows).
        void RenderClientFx(GraphicsEngine* gfx, const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj);

        // --- loadout-depth wave: flash presentation ------------------------------

        /// True while the LOCAL player is under a flash effect. HUD-adjacent
        /// query (no TFHUD coupling needed — RenderFlashOverlay draws itself).
        bool IsLocalFlashed() const { return m_localFlashUntil > NowSec(); }

        /// Draw the full-screen flash whiteout fade. MUST run in the OnImGui
        /// phase (TFOpticsSystem::RenderOverlay precedent) — hooked next to it
        /// from TFNameplates::RenderUI (wiring snippet). Headless no-op.
        void RenderFlashOverlay();

        /// Server: true while `player` is currently flashed (server truth;
        /// other systems, e.g. a future accuracy-penalty pass, can query this
        /// without depending on the wire message).
        bool IsFlashed(PlayerId player) const;

      private:
        // --- server state ---
        struct ThrowerRec
        {
            int remaining = 0;
            double nextThrowAt = 0.0;
        };

        struct ServerGrenade
        {
            uint16_t id = 0;
            PlayerId thrower = kInvalidPlayer;
            EntityId throwerPawn = 0;
            WeaponId weapon = kInvalidWeapon;
            float splashRadiusM = 0.0f;
            float splashDamage = 0.0f;
            float vsVehicleMult = 1.0f;
            float gravityFactor = 1.0f;
            float pos[3]{};
            float vel[3]{};
            float lifeSec = 0.0f;
            bool resting = false;
            uint8_t effect = kTFGrenadeEffectDamage; ///< loadout-depth wave: damage/smoke/flash
        };

        // --- client mirror ---
        struct ClientGrenade
        {
            float pos[3]{};
            float vel[3]{};
            bool resting = false;
            float staleSec = 0.0f; ///< safety prune (no update / missed boom)
        };

        struct BoomFx
        {
            float pos[3]{};
            float age = 0.0f;
        };

        /// loadout-depth wave: client-side smoke puff mirror (presentation only).
        struct SmokePuff
        {
            float pos[3]{};
            float radiusM = 0.0f;
            float life = 1.0f;
            float age = 0.0f;
        };

        double NowSec() const;
        const WeaponDef* FragDef() const; ///< nullptr while tables unloaded

        // --- loadout-depth wave: grenade-kind resolution + effects ---------------

        /// Server: the thrower's selected grenade def, resolved from
        /// TFProgressionSystem::GetLoadout (falls back to FragDef() when no
        /// pick / unknown key / not unlocked — defense in depth mirrors
        /// TFProgressionSystem::ValidGrenadeChoiceKey, which already gated the
        /// pick at save time).
        const WeaponDef* SelectedGrenadeDef(PlayerId player) const;

        /// Own lazy one-shot parse of weapons.json's additive "grenadeEffect"
        /// field (TFOpticsSystem::EnsureTable precedent). Absent/unknown ==
        /// kTFGrenadeEffectDamage.
        void EnsureEffectTable();
        uint8_t EffectKindOf(const std::string& weaponKey); ///< calls EnsureEffectTable (lazy load)

        void ServerSpawnSmoke(const ServerGrenade& g);
        void ServerApplyFlash(const ServerGrenade& g);
        void SendToOwner(PlayerId owner, uint16_t msgId, const void* payload, size_t size); ///< unicast (TFProgressionSystem::SendToOwner pattern)

        void ClientHandleFlash(const TF_FlashState& msg);
        void ClientHandleSmoke(const TF_SmokeSpawn& msg);
        void ClientAdvanceSmoke(float dt);

        // server internals
        bool ServerTryThrow(PlayerId player, const float dirUnit[3]);
        void ServerStepGrenade(ServerGrenade& g, float fdt);
        void ServerBounce(ServerGrenade& g, const float normal[3]);
        void ServerDetonate(const ServerGrenade& g);
        void ServerBroadcast(uint16_t msgId, const void* payload, size_t size, bool reliable);
        void OnPlayerSpawned(const EvPlayerSpawned& ev);
        void OnPlayerKilled(const EvPlayerKilled& ev);

        // client internals
        void ClientPollThrowKey();
        void SendThrow(float viewYaw, float viewPitch); ///< authority: direct (gated); client: SendMsg
        void ClientHandleSpawn(const TF_GrenadeSpawn& msg);
        void ClientHandleUpdate(const TF_GrenadeUpdate& msg);
        void ClientHandleBoom(const TF_GrenadeBoom& msg);
        void ClientAdvance(float dt); ///< extrapolate bodies, age booms, prune stale
        void ClientReseedLocalCount();

#ifdef ENABLE_NETWORKING
        bool ClientNetActive() const;
        void EnsureClientHandlers();
        void ReleaseClientHandlers();
#endif

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};
        double m_clock{0.0}; ///< fallback time base when serverSim is absent

        // server state (authority only)
        std::unordered_map<PlayerId, ThrowerRec> m_throwers;
        std::vector<ServerGrenade> m_live;
        uint16_t m_nextGrenadeId{1};
        float m_updateAccum{0.0f};

        // loadout-depth wave: own lazy parse of weapons.json's additive
        // "grenadeEffect" field (frag/smoke/flash), and server-truth flash state.
        bool m_effectTableLoaded{false};
        std::unordered_map<std::string, uint8_t> m_effectByKey;
        std::unordered_map<PlayerId, double> m_flashedUntil; ///< server time (NowSec()) the flash clears

        // client state (all roles with a local player)
        std::unordered_map<uint16_t, ClientGrenade> m_clientGrenades;
        std::vector<BoomFx> m_booms;
        std::vector<SmokePuff> m_smokePuffs; ///< loadout-depth wave
        double m_localFlashUntil{-1.0};      ///< loadout-depth wave: local player's flash-clears time
        float m_localFlashDurationSec{0.0f}; ///< loadout-depth wave: total duration, for the fade curve
        int m_localRemaining{-1};
        bool m_wasAliveLocal{false};
        int m_throwVk{'G'};
        bool m_clientHandlers{false};
        bool m_boomAudioLoaded{false};

        // render resources (lazy; render thread == game thread)
        std::unique_ptr<Mesh> m_sphere; ///< unit sphere scaled per grenade
        std::unique_ptr<Mesh> m_quad;   ///< unit plane billboarded for the boom flipbook

        // debug counters
        uint32_t m_throws{0}, m_rejected{0}, m_badPackets{0}, m_detonations{0};
    };

} // namespace Terrafront
