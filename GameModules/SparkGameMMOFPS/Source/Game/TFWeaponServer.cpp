/**
 * @file TFWeaponServer.cpp
 * @brief Server half of TERRAFRONT weapons: fire validation (RoF token
 *        bucket), lag-compensated hitscan with pellet spread, server-side
 *        projectile simulation with splash, terrain occlusion.
 *        Client half in TFWeaponSystem.cpp.
 */
#include "Game/TFWeaponSystem.h"

#include "Game/TFDamageSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponMath.h"
#include "Net/TFServerSim.h"
#include "World/TFWorldSetup.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>

namespace Terrafront {

namespace {
constexpr float  kMaxHitscanRangeM   = 400.0f;
constexpr float  kProjectileLifeSec  = 8.0f;
constexpr float  kGravity            = 19.6f;   // TF movement model v1
constexpr double kFixedRttSec        = 0.100;   // TF-W2: per-client RTT from NetworkManager
constexpr float  kPawnRadius         = 0.4f;    // matches TFServerSim RewindPose capsules
constexpr float  kPawnHeight         = 1.8f;
constexpr float  kHeadBandM          = 0.30f;   // top of capsule counting as head
constexpr uint8_t kDamageKindBullet    = 0;
constexpr uint8_t kDamageKindExplosive = 1;
}

double TFWeaponSystem::ServerNow() const
{
    return (m_ctx && m_ctx->serverSim) ? m_ctx->serverSim->ServerTime() : m_serverClock;
}

// ---------------------------------------------------------------------------
// Frozen API: server fire entry
// ---------------------------------------------------------------------------

void TFWeaponSystem::ServerHandleFire(PlayerId shooter, const TF_FireEvent& ev)
{
    if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->players || !m_ctx->data ||
        !m_ctx->data->IsLoaded())
        return;

    PawnInfo pawn;
    if (!m_ctx->players->GetPawnByPlayer(shooter, pawn) || !pawn.alive)
        return;

    const WeaponDef* base = m_ctx->data->GetWeapon(ev.weaponId);
    if (!base || base->kind == "melee" || base->kind == "beam")
        return; // TF-W2: melee reach + tool beams take a different server path

    const WeaponDef def = m_ctx->data->ResolveWeapon(ev.weaponId, pawn.faction);

    ShooterState& st = m_shooters[shooter];
    const double now = ServerNow();
    if (!ValidateFire(def, st, now))
    {
        ++m_shotsRejected;
        return;
    }
    ++m_shotsValidated;

    // Server-trusted origin: the pawn's eye. Client dir is used but normalized.
    float origin[3] = {pawn.pos[0], pawn.pos[1] + 1.65f, pawn.pos[2]};
    // Registry-stub pawns report zeros; fall back to the client origin then.
    if (pawn.pos[0] == 0.0f && pawn.pos[1] == 0.0f && pawn.pos[2] == 0.0f)
    {
        origin[0] = ev.originX; origin[1] = ev.originY; origin[2] = ev.originZ;
    }
    float dir[3] = {ev.dirX, ev.dirY, ev.dirZ};
    if (!WeaponMath::Normalize3(dir))
        return;

    if (def.projSpeed > 0.0f)
    {
        SpawnServerProjectile(shooter, pawn, def, origin, dir);
        return;
    }

    const double rewindTime = now - kFixedRttSec * 0.5;
    const int pellets = std::max(1, def.pellets);
    for (int i = 0; i < pellets; ++i)
    {
        float pdir[3] = {dir[0], dir[1], dir[2]};
        if (pellets > 1 || def.spreadHipDeg > 0.0f)
            WeaponMath::PerturbCone(pdir, def.spreadHipDeg * 0.5f, m_rng);
        FireHitscanRay(shooter, pawn, def, origin, pdir, kMaxHitscanRangeM, rewindTime,
                       kDamageKindBullet);
    }
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

bool TFWeaponSystem::ValidateFire(const WeaponDef& def, ShooterState& st, double now)
{
    const double perSec = std::max(1.0, static_cast<double>(def.rofRpm) / 60.0);

    if (st.weapon != def.id)
    {
        st.weapon = def.id;
        st.tokens = 2.0f;
        st.lastRefill = now;
        st.mag = def.magSize;
    }

    st.tokens = std::min(2.0f, st.tokens + static_cast<float>((now - st.lastRefill) * perSec));
    st.lastRefill = now;

    if (st.tokens < 1.0f)
        return false;
    st.tokens -= 1.0f;

    // Approximate server mag: refills after reloadSec of silence.
    if (st.mag <= 0)
    {
        if (now - st.magEmptyTime < def.reloadSec)
            return false;
        st.mag = def.magSize;
    }
    if (--st.mag <= 0)
        st.magEmptyTime = now;

    st.lastShotTime = now;
    return true;
}

// ---------------------------------------------------------------------------
// Hitscan
// ---------------------------------------------------------------------------

void TFWeaponSystem::FireHitscanRay(PlayerId shooter, const PawnInfo& pawn, const WeaponDef& def,
                                    const float origin[3], const float dir[3], float maxDist,
                                    double rewindTime, uint8_t damageKind)
{
#ifdef ENABLE_NETWORKING
    if (!m_ctx->serverSim || !m_ctx->damage)
        return;

    float hitPoint[3];
    float hitDist = 0.0f;
    const EntityId hit = m_ctx->serverSim->LagComp().RewindRaycast(
        rewindTime, origin, dir, maxDist, pawn.entity, hitPoint, &hitDist);
    if (hit == 0)
        return;

    if (TerrainBlocked(origin, dir, hitDist))
        return;

    // Head heuristic: hit in the top band of a pawn capsule. We only know the
    // hit point; compare against the victim's current pose if available.
    bool head = false;
    PawnInfo victim;
    if (m_ctx->players->GetPawnByEntity(hit, victim) && victim.alive)
        head = hitPoint[1] >= victim.pos[1] + (kPawnHeight - kHeadBandM);

    float damage = WeaponMath::LinearFalloff(def.damage, def.minDamage, def.falloffStartM,
                                               def.falloffEndM, hitDist);
    if (head)
        damage *= def.headshotMult;

    m_ctx->damage->ServerApplyDamage(hit, pawn.entity, shooter, damage, damageKind, def.id, head);
#else
    (void)shooter; (void)pawn; (void)def; (void)origin; (void)dir; (void)maxDist;
    (void)rewindTime; (void)damageKind;
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

EntityId TFWeaponSystem::RaycastPawnsNow(const float origin[3], const float dir[3], float maxDist,
                                         EntityId ignore, float outHitPoint[3], float* outDist) const
{
#ifdef ENABLE_NETWORKING
    if (!m_ctx || !m_ctx->serverSim)
        return 0;
    return m_ctx->serverSim->LagComp().RewindRaycast(ServerNow(), origin, dir, maxDist, ignore,
                                                     outHitPoint, outDist);
#else
    (void)origin; (void)dir; (void)maxDist; (void)ignore; (void)outHitPoint; (void)outDist;
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// Projectiles
// ---------------------------------------------------------------------------

void TFWeaponSystem::SpawnServerProjectile(PlayerId shooter, const PawnInfo& pawn,
                                           const WeaponDef& def, const float origin[3],
                                           const float dir[3])
{
    ServerProjectile p;
    p.shooter = shooter;
    p.shooterPawn = pawn.entity;
    p.weapon = def.id;
    p.pos[0] = origin[0]; p.pos[1] = origin[1]; p.pos[2] = origin[2];
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
    if (m_projectiles.empty())
        return;

    for (auto it = m_projectiles.begin(); it != m_projectiles.end();)
    {
        ServerProjectile& p = *it;
        p.lifeSec += dt;

        const float prev[3] = {p.pos[0], p.pos[1], p.pos[2]};
        p.vel[1] -= kGravity * p.gravityFactor * dt;
        p.pos[0] += p.vel[0] * dt;
        p.pos[1] += p.vel[1] * dt;
        p.pos[2] += p.vel[2] * dt;

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
            const EntityId hit =
                RaycastPawnsNow(prev, dir, segLen, p.shooterPawn, hitPoint, &hitDist);
            if (hit != 0)
            {
                PawnInfo victim;
                bool head = false;
                if (m_ctx->players && m_ctx->players->GetPawnByEntity(hit, victim) && victim.alive)
                    head = hitPoint[1] >= victim.pos[1] + (kPawnHeight - kHeadBandM);

                float damage = WeaponMath::LinearFalloff(
                    p.damage, p.minDamage, p.falloffStartM, p.falloffEndM, p.traveledM);
                if (head)
                    damage *= p.headshotMult;

                if (m_ctx->damage)
                    m_ctx->damage->ServerApplyDamage(hit, p.shooterPawn, p.shooter, damage,
                                                     kDamageKindBullet, p.weapon, head);
                ExplodeAt(p, hitPoint, hit);
                dead = true;
            }
        }

        // Terrain impact.
        if (!dead && m_ctx && m_ctx->world &&
            p.pos[1] <= m_ctx->world->TerrainHeightAt(p.pos[0], p.pos[2]))
        {
            ExplodeAt(p, p.pos, 0);
            dead = true;
        }

        if (!dead && p.lifeSec > kProjectileLifeSec)
            dead = true;

        it = dead ? m_projectiles.erase(it) : it + 1;
    }
}

void TFWeaponSystem::ExplodeAt(const ServerProjectile& p, const float at[3], EntityId excludeEntity)
{
    if (p.splashRadiusM <= 0.0f || p.splashDamage <= 0.0f || !m_ctx || !m_ctx->players ||
        !m_ctx->damage)
        return;

    // TF-W1-FULL: PawnInfo.pos comes from live ECS state; the registry stub
    // reports zeros, so splash is effectively inert until the full player
    // system lands. Logic is correct against real positions.
    m_ctx->players->ForEachAlivePawn([&](const PawnInfo& pawn) {
        if (pawn.entity == excludeEntity)
            return;
        const float d = std::sqrt(WeaponMath::Dist2(pawn.pos, at));
        if (d > p.splashRadiusM)
            return;
        const float dmg = p.splashDamage * (1.0f - d / p.splashRadiusM);
        if (dmg > 1.0f)
            m_ctx->damage->ServerApplyDamage(pawn.entity, p.shooterPawn, p.shooter, dmg,
                                             kDamageKindExplosive, p.weapon, false);
    });
}

} // namespace Terrafront
