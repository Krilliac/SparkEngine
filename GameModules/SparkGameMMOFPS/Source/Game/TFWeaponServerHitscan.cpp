/**
 * @file TFWeaponServerHitscan.cpp
 * @brief TFWeaponSystem server hitscan: lag-compensated pellet rays with
 *        physics-backed world occlusion, vehicle hull tests, hit zones and
 *        terrain blocking, plus the present-time pawn raycast helper the
 *        projectile step reuses. Split from TFWeaponServer.cpp.
 */
#include "Game/TFWeaponSystem.h"

#include "Game/TFBallistics.h"
#include "Game/TFDamageSystem.h"
#include "Game/TFDeployableSystem.h" // W3 shared-edit: splash vs deployables
#include "Game/TFPlayerSystem.h"
#include "Game/TFVehicleSystem.h" // W3 shared-edit: vehicle hit tests + seat weapons
#include "Game/TFWeaponMath.h"
#include "Net/TFFireFxProtocol.h" // W11 impact-broadcast: TFImpactSurface values
#include "Net/TFServerSim.h"
#include "World/TFWorldSetup.h"

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Hitscan
    // ---------------------------------------------------------------------------

    bool TFWeaponSystem::FireHitscanRay(PlayerId shooter, const PawnInfo& pawn, const WeaponDef& def,
                                        const float origin[3], const float dir[3], float maxDist, double rewindTime,
                                        uint8_t damageKind)
    {
#ifdef ENABLE_NETWORKING
        if (!m_ctx->serverSim || !m_ctx->damage)
            return false;

        // Ballistics: physics raycast against the world (buildings, rocks,
        // deployable shields). A hit body that carries an ECS pawn entity doubles
        // as hit registration; any other solid body clamps the ray. When physics
        // is unavailable (null GetPhysics / Jolt stub) blockDist stays maxDist and
        // the terrain heightfield below remains the only world occluder — exactly
        // the pre-ballistics behaviour.
        float blockDist = maxDist;
        EntityId physPawn = 0;
        float physPawnDist = 0.0f;
        float physPawnPoint[3]{};
        // W11 impact-broadcast: remember the world-geometry stop so blocked
        // shots can broadcast their true visual endpoint (0x54F5).
        bool wallValid = false;
        float wallPointFx[3]{};
        uint8_t wallSurface = static_cast<uint8_t>(TFImpactSurface::Static);
        Ballistics::WorldHit wall;
        if (Ballistics::RaycastWorld(m_ctx->engine, origin, dir, maxDist, wall) && wall.blocked)
        {
            PawnInfo pv;
            if (static_cast<EntityId>(wall.entityId) == pawn.entity || wall.dist < Ballistics::kMuzzleClearanceM)
            {
                // Shooter's own body / near-clip geometry (e.g. the hull of the
                // vehicle a gunner sits in): never eats the shot.
            }
            else if (wall.entityId != 0 && m_ctx->players &&
                     m_ctx->players->GetPawnByEntity(static_cast<EntityId>(wall.entityId), pv) && pv.alive)
            {
                physPawn = static_cast<EntityId>(wall.entityId);
                physPawnDist = wall.dist;
                physPawnPoint[0] = wall.point[0];
                physPawnPoint[1] = wall.point[1];
                physPawnPoint[2] = wall.point[2];
            }
            else
            {
                blockDist = wall.dist;
                wallValid = true;
                wallPointFx[0] = wall.point[0];
                wallPointFx[1] = wall.point[1];
                wallPointFx[2] = wall.point[2];
                TFDeployableView dv;
                if (wall.entityId != 0 && m_ctx->deployables &&
                    m_ctx->deployables->GetDeployable(static_cast<EntityId>(wall.entityId), dv))
                    wallSurface = static_cast<uint8_t>(TFImpactSurface::Shield);
            }
        }

        // W11 impact-broadcast: terrain-stop fx helper (cap pre-gated so capped
        // shots skip the march entirely).
        const auto fxTerrain = [&](float dist)
        {
            if (!ImpactFxCapOpen(shooter))
                return;
            const float tT = TerrainHitT(origin, dir, dist);
            if (tT <= 0.0f)
                return;
            const float pt[3] = {origin[0] + dir[0] * tT, origin[1] + dir[1] * tT, origin[2] + dir[2] * tT};
            ServerBroadcastImpactFx(shooter, pt, static_cast<uint8_t>(TFImpactSurface::Terrain));
        };

        float hitPoint[3];
        float hitDist = 0.0f;
        EntityId hit = m_ctx->serverSim->LagComp().RewindRaycast(rewindTime, origin, dir, maxDist, pawn.entity,
                                                                 hitPoint, &hitDist);

        // Physics pawn body as fallback registration when the lag-comp capsule set
        // missed (e.g. a pawn body not present in the rewind buffer). Rewound
        // capsules stay authoritative when they DO report a hit.
        if (hit == 0 && physPawn != 0)
        {
            hit = physPawn;
            hitDist = physPawnDist;
            hitPoint[0] = physPawnPoint[0];
            hitPoint[1] = physPawnPoint[1];
            hitPoint[2] = physPawnPoint[2];
        }

        // W3 shared-edit (vehicles agent): the same ray also tests vehicle hulls;
        // the NEAREST of pawn/vehicle wins. Vehicle hits take def.vsVehicleMult
        // and route to TFVehicleSystem's hp pool instead of ctx.damage. (Vehicles
        // move slowly relative to the lag-comp window, so present-time hulls are
        // an acceptable W3 approximation vs. rewound pawn capsules.)
        float vehPoint[3];
        float vehDist = 0.0f;
        const EntityId vehHit =
            m_ctx->vehicles ? m_ctx->vehicles->RaycastVehicles(origin, dir, maxDist, vehPoint, &vehDist) : 0;

        if (vehHit != 0 && (hit == 0 || vehDist < hitDist))
        {
            if (vehDist > blockDist)
            {
                if (wallValid)
                    ServerBroadcastImpactFx(shooter, wallPointFx, wallSurface);
                return false; // world geometry in front of the vehicle
            }
            if (TerrainBlocked(origin, dir, vehDist))
            {
                fxTerrain(vehDist);
                return false;
            }
            const float vdmg =
                WeaponMath::LinearFalloff(def.damage, def.minDamage, def.falloffStartM, def.falloffEndM, vehDist) *
                def.vsVehicleMult;
            m_ctx->vehicles->ServerDamageVehicle(vehHit, vdmg, pawn.entity, shooter, def.id);
            ServerBroadcastImpactFx(shooter, vehPoint, static_cast<uint8_t>(TFImpactSurface::Vehicle));
            return true;
        }

        if (hit == 0)
        {
            // W11 impact-broadcast: nothing registered — the shot's true visual
            // stop is the world-geometry block or terrain along the full ray
            // (a genuine sky shot broadcasts nothing).
            if (wallValid)
                ServerBroadcastImpactFx(shooter, wallPointFx, wallSurface);
            else
                fxTerrain(maxDist);
            return false;
        }

        if (hitDist > blockDist)
        {
            if (wallValid)
                ServerBroadcastImpactFx(shooter, wallPointFx, wallSurface);
            return false; // world geometry in front of the target
        }

        if (TerrainBlocked(origin, dir, hitDist))
        {
            fxTerrain(hitDist);
            return false;
        }

        // Hit zone from the hit height on the victim's capsule: head band takes
        // headshotMult, leg band takes the limb malus, torso is baseline.
        bool head = false;
        float zoneMult = 1.0f;
        PawnInfo victim;
        if (m_ctx->players->GetPawnByEntity(hit, victim) && victim.alive)
        {
            const Ballistics::HitZone zone = Ballistics::ClassifyHitZone(hitPoint, victim.pos);
            head = zone == Ballistics::HitZone::Head;
            zoneMult = Ballistics::ZoneDamageMult(zone, def.headshotMult);
        }

        const float damage =
            WeaponMath::LinearFalloff(def.damage, def.minDamage, def.falloffStartM, def.falloffEndM, hitDist) *
            zoneMult;

        m_ctx->damage->ServerApplyDamage(hit, pawn.entity, shooter, damage, damageKind, def.id, head);
        ServerBroadcastImpactFx(shooter, hitPoint, static_cast<uint8_t>(TFImpactSurface::Pawn));
        return true;
#else
        (void)shooter;
        (void)pawn;
        (void)def;
        (void)origin;
        (void)dir;
        (void)maxDist;
        (void)rewindTime;
        (void)damageKind;
        return false;
#endif
    }

    bool TFWeaponSystem::TerrainBlocked(const float origin[3], const float dir[3], float dist) const
    {
        if (!m_ctx || !m_ctx->world)
            return false;
        const float step = 1.0f;
        for (float t = step; t < dist; t += step)
        {
            const float x = origin[0] + dir[0] * t;
            const float y = origin[1] + dir[1] * t;
            const float z = origin[2] + dir[2] * t;
            if (y < m_ctx->world->TerrainHeightAt(x, z))
                return true;
        }
        return false;
    }

    EntityId TFWeaponSystem::RaycastPawnsNow(const float origin[3], const float dir[3], float maxDist, EntityId ignore,
                                             float outHitPoint[3], float* outDist) const
    {
#ifdef ENABLE_NETWORKING
        if (!m_ctx || !m_ctx->serverSim)
            return 0;
        return m_ctx->serverSim->LagComp().RewindRaycast(ServerNow(), origin, dir, maxDist, ignore, outHitPoint,
                                                         outDist);
#else
        (void)origin;
        (void)dir;
        (void)maxDist;
        (void)ignore;
        (void)outHitPoint;
        (void)outDist;
        return 0;
#endif
    }

} // namespace Terrafront
