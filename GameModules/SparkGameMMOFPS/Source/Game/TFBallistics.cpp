/**
 * @file TFBallistics.cpp
 * @brief Implementation of the stateless ballistics helpers: hit-zone
 *        classification, PhysicsSystem raycast wrappers (null-safe), and
 *        projectile gravity integration. See TFBallistics.h for the contract.
 */
#include "Game/TFBallistics.h"

#include "Game/TFWeaponMath.h"

#include "Physics/PhysicsSystem.h" // engine umbrella header; stub-safe when Jolt is absent
#include "Spark/IEngineContext.h"

#include <cmath>

namespace Terrafront::Ballistics
{

    namespace
    {

        /// Engine PhysicsSystem, or nullptr when the engine/context/physics is absent.
        ::PhysicsSystem* WorldPhysics(Spark::IEngineContext* engine)
        {
            return engine ? engine->GetPhysics() : nullptr;
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Hit zones
    // ---------------------------------------------------------------------------

    HitZone ClassifyHitZone(const float hitPoint[3], const float victimFeetPos[3])
    {
        const float h = hitPoint[1] - victimFeetPos[1]; // height above the feet
        if (h >= WeaponMath::kPawnHeightM - WeaponMath::kHeadZoneM)
            return HitZone::Head;
        if (h <= kLegBandM)
            return HitZone::Limb;
        return HitZone::Torso;
    }

    float ZoneDamageMult(HitZone zone, float headshotMult)
    {
        switch (zone)
        {
        case HitZone::Head:
            return headshotMult;
        case HitZone::Limb:
            return kLimbDamageMult;
        case HitZone::Torso:
            break;
        }
        return 1.0f;
    }

    // ---------------------------------------------------------------------------
    // Physics-backed world queries
    // ---------------------------------------------------------------------------

    bool RaycastWorld(Spark::IEngineContext* engine, const float origin[3], const float dir[3], float maxDist,
                      WorldHit& out)
    {
        out = WorldHit{};
        ::PhysicsSystem* physics = WorldPhysics(engine);
        if (!physics || maxDist <= 0.0f)
            return false;

        const XMFLOAT3 o{origin[0], origin[1], origin[2]};
        const XMFLOAT3 d{dir[0], dir[1], dir[2]};
        const RaycastHit hit = physics->RaycastFiltered(o, d, maxDist, CollisionLayers::HitscanMask);
        if (!hit.hasHit || hit.distance > maxDist)
            return false;

        out.blocked = true;
        out.dist = hit.distance;
        out.point[0] = hit.point.x;
        out.point[1] = hit.point.y;
        out.point[2] = hit.point.z;
        out.entityId = hit.entityId;
        return true;
    }

    bool SplashVisible(Spark::IEngineContext* engine, const float from[3], const float to[3])
    {
        ::PhysicsSystem* physics = WorldPhysics(engine);
        if (!physics)
            return true; // no physics: splash behaves exactly as before

        float d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
        const float len = WeaponMath::Len3(d);
        constexpr float kNudgeM = 0.05f; // the blast point sits ON the surface it hit
        if (len <= 2.0f * kNudgeM || !WeaponMath::Normalize3(d))
            return true; // target effectively at the blast point

        const XMFLOAT3 o{from[0] + d[0] * kNudgeM, from[1] + d[1] * kNudgeM, from[2] + d[2] * kNudgeM};
        const XMFLOAT3 dv{d[0], d[1], d[2]};
        const RaycastHit hit = physics->RaycastFiltered(o, dv, len - 2.0f * kNudgeM,
                                                        CollisionLayers::WorldStatic | CollisionLayers::Deployable);
        return !hit.hasHit;
    }

    // ---------------------------------------------------------------------------
    // Projectile flight
    // ---------------------------------------------------------------------------

    void IntegrateProjectile(float pos[3], float vel[3], float gravityFactor, float dt)
    {
        vel[1] -= kProjectileGravityMps2 * gravityFactor * dt;
        pos[0] += vel[0] * dt;
        pos[1] += vel[1] * dt;
        pos[2] += vel[2] * dt;
    }

} // namespace Terrafront::Ballistics
