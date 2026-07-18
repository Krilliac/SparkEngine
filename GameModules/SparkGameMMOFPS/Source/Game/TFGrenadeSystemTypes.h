/**
 * @file TFGrenadeSystemTypes.h
 * @brief Grenades-lane wire protocol (reserved TFMsg ids + packed structs),
 *        gameplay tuning constants, and quantization helpers, split out of
 *        TFGrenadeSystem.h (which includes this header, so no includer
 *        changes). See that header for the full lane design doc.
 *
 *  - WIRE: reserved TFMsg block 0x5464-0x5467. C->S TF_GrenadeThrow rides
 *    TFServerSim::RouteClientMessage (enter-world-gated choke point, wiring
 *    snippet in the wave report). S->C spawn/update/boom are sent by this
 *    system to all clients (spawn/boom reliable, 10 Hz position updates
 *    unreliable, every struct <= 16 bytes, quantized positions). No
 *    late-joiner burst: a grenade lives 2.5 s, so a client joining mid-flight
 *    misses at most one already-airborne visual (documented gap, not a bug).
 *    Listen-host/standalone local player is fed by direct mirror calls
 *    (TFAbilitySystem::ServerBroadcastState pattern).
 *  - WIRE: rides the loadout-depth lane's reserved TFMsg block
 *    0x5488-0x548B (Game/TFProgressionSystem.h owns 0x5488; the S->C ids
 *    here are 0x5489 TF_FlashState and 0x548A TF_SmokeSpawn, unicast/
 *    broadcast respectively via the same ServerBroadcast/SendToOwner
 *    machinery as the W10 grenade ids).
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

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

} // namespace Terrafront
