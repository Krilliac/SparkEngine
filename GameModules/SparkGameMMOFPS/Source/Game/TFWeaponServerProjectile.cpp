/**
 * @file TFWeaponServerProjectile.cpp
 * @brief TFWeaponSystem server projectiles: spawn from validated fire events,
 *        fixed-step ballistic simulation with pawn/vehicle/world sweeps and
 *        terrain impact, and the splash detonation (pawns, deployables,
 *        vehicles). Split from TFWeaponServer.cpp; shared damage-kind
 *        constants in TFWeaponServerInternal.h.
 */
#include "Game/TFWeaponSystem.h"

#include "Game/TFBallistics.h"
#include "Game/TFDamageSystem.h"
#include "Game/TFDeployableSystem.h" // W3 shared-edit: splash vs deployables
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h" // W6 progression: per-weapon shot/hit stats
#include "Game/TFVehicleSystem.h"     // W3 shared-edit: vehicle hit tests + seat weapons
#include "Game/TFWeaponMath.h"
#include "Game/TFWeaponServerInternal.h"
#include "Net/TFFireFxProtocol.h" // W11 impact-broadcast: TFImpactSurface values
#include "World/TFWorldSetup.h"

#include <cmath>

namespace Terrafront
{

    using namespace WeaponServerDetail;

    namespace
    {
        constexpr float kProjectileLifeSec = 8.0f;
        // Projectile gravity + pawn capsule bands now live in Game/TFBallistics.h
        // (Ballistics::kProjectileGravityMps2, WeaponMath::kPawnHeightM/kHeadZoneM).
    } // namespace

    // ---------------------------------------------------------------------------
    // Projectiles
    // ---------------------------------------------------------------------------

    void TFWeaponSystem::SpawnServerProjectile(PlayerId shooter, const PawnInfo& pawn, const WeaponDef& def,
                                               const float origin[3], const float dir[3])
    {
        ServerProjectile p;
        p.shooter = shooter;
        p.shooterPawn = pawn.entity;
        p.weapon = def.id;
        p.pos[0] = origin[0];
        p.pos[1] = origin[1];
        p.pos[2] = origin[2];
        p.vel[0] = dir[0] * def.projSpeed;
        p.vel[1] = dir[1] * def.projSpeed;
        p.vel[2] = dir[2] * def.projSpeed;
        p.gravityFactor = def.gravity;
        p.damage = def.damage;
        p.minDamage = def.minDamage;
        p.falloffStartM = def.falloffStartM;
        p.falloffEndM = def.falloffEndM;
        p.headshotMult = def.headshotMult;
        p.splashRadiusM = def.splashRadiusM;
        p.splashDamage = def.splashDamage;
        m_projectiles.push_back(p);
    }

    void TFWeaponSystem::ServerStepProjectiles(float dt)
    {
        if (!m_ctx || m_projectiles.empty())
            return;

        for (auto it = m_projectiles.begin(); it != m_projectiles.end();)
        {
            ServerProjectile& p = *it;
            p.lifeSec += dt;

            const float prev[3] = {p.pos[0], p.pos[1], p.pos[2]};
            Ballistics::IntegrateProjectile(p.pos, p.vel, p.gravityFactor, dt);

            float seg[3] = {p.pos[0] - prev[0], p.pos[1] - prev[1], p.pos[2] - prev[2]};
            const float segLen = WeaponMath::Len3(seg);
            p.traveledM += segLen;

            bool dead = false;

            // Pawn sweep along this step (lag-comp buffer holds current poses too).
            if (segLen > 1.0e-4f)
            {
                float dir[3] = {seg[0] / segLen, seg[1] / segLen, seg[2] / segLen};
                float hitPoint[3];
                float hitDist = 0.0f;
                EntityId hit = RaycastPawnsNow(prev, dir, segLen, p.shooterPawn, hitPoint, &hitDist);

                // Ballistics: physics sweep along this step — buildings/rocks/
                // deployable shields detonate the warhead mid-flight. A physics
                // body carrying a pawn entity doubles as hit registration when the
                // lag-comp sweep missed. Physics unavailable -> wallBlocked stays
                // false and the terrain check below is the only world impact.
                bool wallBlocked = false;
                float wallDist = segLen + 1.0f;
                float wallPoint[3]{};
                Ballistics::WorldHit wall;
                if (Ballistics::RaycastWorld(m_ctx->engine, prev, dir, segLen, wall) && wall.blocked)
                {
                    PawnInfo pv;
                    // Flight distance from the muzzle to this wall hit (traveledM
                    // already includes the current segment).
                    const float distFromMuzzle = p.traveledM - segLen + wall.dist;
                    if (static_cast<EntityId>(wall.entityId) == p.shooterPawn ||
                        distFromMuzzle < Ballistics::kMuzzleClearanceM)
                    {
                        // Shooter's own body / muzzle-adjacent geometry on the very
                        // first step (e.g. a gunner's own vehicle hull): ignore.
                    }
                    else if (wall.entityId != 0 && m_ctx->players &&
                             m_ctx->players->GetPawnByEntity(static_cast<EntityId>(wall.entityId), pv) && pv.alive)
                    {
                        if (hit == 0)
                        {
                            hit = static_cast<EntityId>(wall.entityId);
                            hitDist = wall.dist;
                            hitPoint[0] = wall.point[0];
                            hitPoint[1] = wall.point[1];
                            hitPoint[2] = wall.point[2];
                        }
                    }
                    else
                    {
                        wallBlocked = true;
                        wallDist = wall.dist;
                        wallPoint[0] = wall.point[0];
                        wallPoint[1] = wall.point[1];
                        wallPoint[2] = wall.point[2];
                    }
                }

                // W3 shared-edit (vehicles agent): the projectile step also sweeps
                // vehicle hulls; nearest of pawn/vehicle wins. Direct vehicle hits
                // take the base weapon's vsVehicleMult, then the warhead splashes
                // around the impact (vehicle excluded — no double dip).
                float vehPoint[3];
                float vehDist = 0.0f;
                const EntityId vehHit =
                    m_ctx->vehicles ? m_ctx->vehicles->RaycastVehicles(prev, dir, segLen, vehPoint, &vehDist) : 0;
                if (vehHit != 0 && (hit == 0 || vehDist < hitDist) && vehDist <= wallDist)
                {
                    const WeaponDef* base = m_ctx->data ? m_ctx->data->GetWeapon(p.weapon) : nullptr;
                    const float vdmg =
                        WeaponMath::LinearFalloff(p.damage, p.minDamage, p.falloffStartM, p.falloffEndM, p.traveledM) *
                        (base ? base->vsVehicleMult : 1.0f);
                    m_ctx->vehicles->ServerDamageVehicle(vehHit, vdmg, p.shooterPawn, p.shooter, p.weapon);
                    ServerBroadcastImpactFx(p.shooter, vehPoint, static_cast<uint8_t>(TFImpactSurface::Vehicle));
                    ExplodeAt(p, vehPoint, 0, vehHit);
                    it = m_projectiles.erase(it);
                    continue;
                }

                if (hit != 0 && hitDist <= wallDist)
                {
                    // Hit zone from the hit height on the victim's capsule (same
                    // rule as hitscan): head band, leg band, torso baseline.
                    PawnInfo victim;
                    bool head = false;
                    float zoneMult = 1.0f;
                    if (m_ctx->players && m_ctx->players->GetPawnByEntity(hit, victim) && victim.alive)
                    {
                        const Ballistics::HitZone zone = Ballistics::ClassifyHitZone(hitPoint, victim.pos);
                        head = zone == Ballistics::HitZone::Head;
                        zoneMult = Ballistics::ZoneDamageMult(zone, p.headshotMult);
                    }

                    const float damage =
                        WeaponMath::LinearFalloff(p.damage, p.minDamage, p.falloffStartM, p.falloffEndM, p.traveledM) *
                        zoneMult;

                    // W6 progression: projectile direct hit (splash never counts).
                    if (m_ctx->progression)
                        m_ctx->progression->ServerRecordHits(p.shooter, p.weapon, 1);
                    if (m_ctx->damage)
                        m_ctx->damage->ServerApplyDamage(hit, p.shooterPawn, p.shooter, damage, kDamageKindBullet,
                                                         p.weapon, head);
                    ServerBroadcastImpactFx(p.shooter, hitPoint, static_cast<uint8_t>(TFImpactSurface::Pawn));
                    ExplodeAt(p, hitPoint, hit);
                    dead = true;
                }
                else if (wallBlocked)
                {
                    // Warhead meets world geometry mid-flight: detonate on the wall.
                    // W11 impact-broadcast: a wall stop carrying a deployable
                    // entity is a shield hit; anything else is static world.
                    TFDeployableView dv;
                    const bool shieldStop = wall.entityId != 0 && m_ctx->deployables &&
                                            m_ctx->deployables->GetDeployable(static_cast<EntityId>(wall.entityId), dv);
                    ServerBroadcastImpactFx(
                        p.shooter, wallPoint,
                        static_cast<uint8_t>(shieldStop ? TFImpactSurface::Shield : TFImpactSurface::Static));
                    ExplodeAt(p, wallPoint, 0);
                    dead = true;
                }
            }

            // Terrain impact.
            if (!dead && m_ctx->world && p.pos[1] <= m_ctx->world->TerrainHeightAt(p.pos[0], p.pos[2]))
            {
                // W11 impact-broadcast: lift the fx point back onto the surface
                // (the integrated pos can be up to one step underground).
                const float surfPt[3] = {p.pos[0], m_ctx->world->TerrainHeightAt(p.pos[0], p.pos[2]), p.pos[2]};
                ServerBroadcastImpactFx(p.shooter, surfPt, static_cast<uint8_t>(TFImpactSurface::Terrain));
                ExplodeAt(p, p.pos, 0);
                dead = true;
            }

            if (!dead && p.lifeSec > kProjectileLifeSec)
                dead = true;

            it = dead ? m_projectiles.erase(it) : it + 1;
        }
    }

    void TFWeaponSystem::ExplodeAt(const ServerProjectile& p, const float at[3], EntityId excludeEntity,
                                   EntityId excludeVehicle)
    {
        if (p.splashRadiusM <= 0.0f || p.splashDamage <= 0.0f || !m_ctx || !m_ctx->players || !m_ctx->damage)
            return;

        // TF-W1-FULL: PawnInfo.pos comes from live ECS state; the registry stub
        // reports zeros, so splash is effectively inert until the full player
        // system lands. Logic is correct against real positions.
        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& pawn)
            {
                if (pawn.entity == excludeEntity)
                    return;
                const float d = std::sqrt(WeaponMath::Dist2(pawn.pos, at));
                if (d > p.splashRadiusM)
                    return;
                // Ballistics: splash line-of-sight — intervening world geometry (walls,
                // deployable shields) blocks the blast. Physics unavailable -> always
                // visible (pre-ballistics behaviour).
                const float chest[3] = {pawn.pos[0], pawn.pos[1] + WeaponMath::kPawnHeightM * 0.5f, pawn.pos[2]};
                if (!Ballistics::SplashVisible(m_ctx->engine, at, chest))
                    return;
                const float dmg = p.splashDamage * (1.0f - d / p.splashRadiusM);
                if (dmg > 1.0f)
                    m_ctx->damage->ServerApplyDamage(pawn.entity, p.shooterPawn, p.shooter, dmg, kDamageKindExplosive,
                                                     p.weapon, false);
            });

        // W3 shared-edit (deployables agent): explosive splash also damages
        // Fabricator/Medtech deployables with the same linear falloff. Direct
        // hitscan/projectile impact vs deployables stays TF-W4 (needs deployable
        // capsules in the lag-comp raycast set).
        if (m_ctx->deployables)
            m_ctx->deployables->ServerSplashDamageDeployables(at, p.splashRadiusM, p.splashDamage, p.shooter);

        // W3 shared-edit (vehicles agent): explosive splash also damages vehicle
        // hulls (rockets near-missing a tank still hurt it). vsVehicleMult comes
        // from the base weapon row; the direct-hit vehicle is excluded above.
        if (m_ctx->vehicles)
        {
            const WeaponDef* base = m_ctx->data ? m_ctx->data->GetWeapon(p.weapon) : nullptr;
            m_ctx->vehicles->ServerApplySplash(at, p.splashRadiusM, p.splashDamage, base ? base->vsVehicleMult : 1.0f,
                                               p.shooterPawn, p.shooter, p.weapon, excludeVehicle);
        }
    }

} // namespace Terrafront
