/**
 * @file TFBallistics.h
 * @brief Server ballistics helpers for TFWeaponSystem: physics-backed world
 *        occlusion + hit registration, hit-zone (head/torso/limb) damage
 *        multipliers, and projectile flight integration (gravity drop /
 *        travel time). Damage falloff itself stays in WeaponMath::LinearFalloff.
 *
 * OWNERSHIP: ballistics lane (alongside TFWeaponSystem.h/.cpp/TFWeaponServer.cpp).
 *
 * Stateless free functions — no system lifecycle, no Main.cpp wiring, no net
 * wire changes. Every physics query null-checks IEngineContext::GetPhysics()
 * (and Jolt-stub builds report no hits), so callers MUST keep their existing
 * fallbacks: terrain-heightfield occlusion and lag-comp capsule hit tests.
 */
#pragma once

#include <cstdint>

namespace Spark
{
    class IEngineContext;
}

namespace Terrafront::Ballistics
{

    // ---------------------------------------------------------------------------
    // Hit zones — derived from the hit height on the victim's pawn capsule.
    // Bands are measured up from the feet; keep in sync with
    // WeaponMath::kPawnHeightM / kHeadZoneM (the lag-comp capsule dimensions).
    // ---------------------------------------------------------------------------

    enum class HitZone : uint8_t
    {
        Torso = 0,
        Head,
        Limb
    };

    /// Lower-capsule (legs) hits deal reduced damage; torso is the 1.0 baseline.
    constexpr float kLimbDamageMult = 0.90f;
    /// Hits below this height above the feet count as limb (legs).
    constexpr float kLegBandM = 0.90f;

    /// Classify a confirmed hit point against the victim's feet position.
    HitZone ClassifyHitZone(const float hitPoint[3], const float victimFeetPos[3]);

    /// Damage multiplier for a zone: Head -> headshotMult, Limb -> kLimbDamageMult,
    /// Torso -> 1.0.
    float ZoneDamageMult(HitZone zone, float headshotMult);

    // ---------------------------------------------------------------------------
    // Physics-backed world queries (null-safe; no-ops without a PhysicsSystem)
    // ---------------------------------------------------------------------------

    /// Result of a hitscan-mask physics raycast against the world.
    struct WorldHit
    {
        bool blocked = false;  ///< a solid body lies within maxDist
        float dist = 0.0f;     ///< distance from the ray origin to the hit
        float point[3]{};      ///< world-space hit point
        uint32_t entityId = 0; ///< ECS entity of the hit body (0 = none/unknown)
    };

    /// Physics blocks closer than this are ignored by the weapon-server callers:
    /// the shooter's own capsule / the hull of the vehicle a gunner sits in can
    /// intersect the eye-origin ray at ~0 m and must not eat the shot.
    constexpr float kMuzzleClearanceM = 0.5f;

    /// Closest solid physics hit along origin+dir within maxDist, filtered to
    /// CollisionLayers::HitscanMask (world statics, players, vehicles,
    /// deployables). Returns false — and leaves `out` unblocked — when physics is
    /// unavailable or nothing was hit; callers then rely on their terrain fallback.
    bool RaycastWorld(Spark::IEngineContext* engine, const float origin[3], const float dir[3], float maxDist,
                      WorldHit& out);

    /// Splash line-of-sight: false when static world geometry or a deployable
    /// blocks the segment from the blast point to the target. Conservatively true
    /// when physics is unavailable (splash behaves exactly as before).
    bool SplashVisible(Spark::IEngineContext* engine, const float from[3], const float to[3]);

    // ---------------------------------------------------------------------------
    // Projectile flight (gravity drop + travel time)
    // ---------------------------------------------------------------------------

    /// TF movement-model gravity (matches the server sim's original constant).
    constexpr float kProjectileGravityMps2 = 19.6f;

    /// One explicit-Euler step of projectile flight: applies gravity drop
    /// (scaled by the weapon's gravityFactor) to vel, then advances pos.
    void IntegrateProjectile(float pos[3], float vel[3], float gravityFactor, float dt);

} // namespace Terrafront::Ballistics
