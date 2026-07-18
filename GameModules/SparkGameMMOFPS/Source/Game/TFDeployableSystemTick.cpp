/**
 * @file TFDeployableSystemTick.cpp
 * @brief TFDeployableSystem server-tick mechanics: lifetime expiry, the W3
 *        turret / ammo pack / med beacon ticks, the W6 resupply station and
 *        AV turret ticks (vehicle shots deferred past the iteration) and the
 *        keepalive replication cadence. Split from TFDeployableSystem.cpp;
 *        the shared internals live in TFDeployableSystemInternal.h.
 */
#include "Game/TFDeployableSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFDamageSystem.h"
#include "Game/TFDeployableSystemInternal.h"
#include "Game/TFDeployableTypes.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFVehicleSystem.h"
#include "World/TFWorldSetup.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Terrafront
{

    using namespace DeploySysDetail;

    namespace
    {

        // W3 kStats moved to Game/TFDeployableTypes.h (kTFDeployableSpecs) — one spec
        // table now carries stats + placement validation inputs for all six kinds.

        constexpr float kTurretRangeM = 40.0f;
        constexpr float kTurretDamage = 95.0f;
        constexpr float kTurretRpm = 450.0f;
        constexpr float kTurretMuzzleY = 1.5f; // fire point above base
        constexpr float kTurretAimY = 1.2f;    // aim at pawn chest height
        constexpr float kAmmoPackRadiusM = 5.0f;
        constexpr float kAmmoPackHealHp = 20.0f; // TF-W4: real ammo refill instead
        constexpr float kBeaconRadiusM = 6.0f;
        constexpr float kBeaconHealPerSec = 30.0f;
        constexpr float kKeepaliveSec = 1.0f;    // health/life refresh cadence
        constexpr uint8_t kDamageKindBullet = 0; // TF_DamageEvent convention

        // --- W6 kinds (specs in TFDeployableTypes.h; these are behavior tunings) ----
        constexpr float kResupplyRadiusM = 8.0f; // heal + repair reach
        constexpr float kResupplyPulseSec = 4.0f;
        constexpr float kResupplyHealHp = 25.0f;   // TF-W4: real ammo refill instead
        constexpr float kResupplyRepairHp = 40.0f; // per pulse, friendly deployables
        constexpr float kAVTurretRangeM = 60.0f;
        constexpr float kAVTurretDamage = 140.0f; // per shot vs vehicle hp pool
        constexpr float kAVTurretShotSec = 2.5f;  // slow heavy cadence
        constexpr float kAVTurretMuzzleY = 2.2f;  // fire point above base
        constexpr float kAVTurretAimY = 1.0f;     // aim at hull center height

    } // namespace

    // ---------------------------------------------------------------------------
    // Server tick
    // ---------------------------------------------------------------------------

    void TFDeployableSystem::ServerTick(float dt)
    {
#ifdef ENABLE_NETWORKING
        if (ServerNetActive())
            ServerPollNewClients();
        else
            m_knownClients.clear();
#endif

        // Lifetime + mechanics. Ticks never insert/erase on this map (pawn damage,
        // value-only repairs, and DEFERRED vehicle shots), so iteration is safe;
        // expiries are collected and destroyed afterwards.
        std::vector<EntityId> expired;
        for (auto& [entity, rec] : m_deployables)
        {
            rec.view.life -= dt;
            if (rec.view.life <= 0.0f)
            {
                expired.push_back(entity);
                continue;
            }
            // Extended W6 kinds are constexpr values past the frozen enum, so they
            // dispatch by comparison (a case label past COUNT would trip C4063).
            if (rec.view.kind == kDeployResupplyStation)
            {
                TickResupplyStation(rec);
            }
            else if (rec.view.kind == kDeployAVTurret)
            {
                TickAVTurret(rec); // queues into m_pendingAvShots (applied below)
            }
            else // ShieldWall is passive; W3 kinds keep their switch
            {
                switch (rec.view.kind)
                {
                case DeployableKind::FabTurret:
                    TickTurret(rec);
                    break;
                case DeployableKind::FabAmmoPack:
                    TickAmmoPack(rec);
                    break;
                case DeployableKind::MedBeacon:
                    TickMedBeacon(rec, dt);
                    break;
                default:
                    break;
                }
            }
        }
        for (EntityId e : expired)
        {
            ++m_expired;
            ServerDestroyDeployable(e, "expired");
        }

        // AV turret shots are applied AFTER the iteration: a destroyed vehicle's
        // explosion chain (TFWeaponServer::ExplodeAt -> splash vs deployables) may
        // erase from m_deployables, which must never happen mid-loop above.
        if (!m_pendingAvShots.empty())
        {
            if (m_ctx->vehicles)
                for (const PendingAVShot& s : m_pendingAvShots)
                    m_ctx->vehicles->ServerDamageVehicle(s.vehicle, kAVTurretDamage, s.turretEntity, s.owner,
                                                         m_avWeapon);
            m_pendingAvShots.clear();
        }

#ifdef ENABLE_NETWORKING
        // Slow keepalive so client health/life mirrors cannot drift for long.
        m_keepaliveAccum += dt;
        if (m_keepaliveAccum >= kKeepaliveSec)
        {
            m_keepaliveAccum = 0.0f;
            if (ServerNetActive())
                for (const auto& [entity, rec] : m_deployables)
                    SendUpdate(rec.view);
        }
#endif
    }

    bool TFDeployableSystem::TurretHasLoS(const float from[3], const float to[3]) const
    {
        if (!m_ctx || !m_ctx->world)
            return true;
        // 1 m terrain sampling, same approach as TFWeaponServer::TerrainBlocked.
        const float d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len <= 1.0f)
            return true;
        const float inv = 1.0f / len;
        for (float t = 1.0f; t < len; t += 1.0f)
        {
            const float x = from[0] + d[0] * inv * t;
            const float y = from[1] + d[1] * inv * t;
            const float z = from[2] + d[2] * inv * t;
            if (y < m_ctx->world->TerrainHeightAt(x, z))
                return false;
        }
        return true;
    }

    void TFDeployableSystem::TickTurret(Rec& rec)
    {
        if (!m_ctx->players || !m_ctx->damage || m_clock < rec.nextShotAt)
            return;

        const float muzzle[3] = {rec.view.pos[0], rec.view.pos[1] + kTurretMuzzleY, rec.view.pos[2]};

        // (Re)acquire: nearest alive enemy pawn in range with terrain LoS. The
        // current target is naturally revalidated because it competes as nearest.
        const float range2 = kTurretRangeM * kTurretRangeM;
        EntityId best = 0;
        float bestD2 = range2;
        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& p)
            {
                if (p.faction == rec.view.faction || p.faction == FactionId::None)
                    return;
                const float aim[3] = {p.pos[0], p.pos[1] + kTurretAimY, p.pos[2]};
                const float d2 = Dist2(rec.view.pos, p.pos);
                if (d2 > bestD2 || !TurretHasLoS(muzzle, aim))
                    return;
                best = p.entity;
                bestD2 = d2;
            });
        rec.targetPawn = best;
        if (best == 0)
            return;

        // Lazy killfeed weapon id: dedicated "fab_turret" row if the data agent
        // adds one; falls back to "-" in the killfeed (TF-W4: own weapon row).
        if (m_turretWeapon == kInvalidWeapon && m_ctx->data && m_ctx->data->IsLoaded())
            if (const WeaponDef* wd = m_ctx->data->GetWeaponByKey("fab_turret"))
                m_turretWeapon = wd->id;

        // Instant hitscan through the damage pipeline: OWNER gets kill credit +
        // hitmarker; the turret entity is the attacker pawn for direction UI.
        m_ctx->damage->ServerApplyDamage(best, rec.view.entity, rec.view.owner, kTurretDamage, kDamageKindBullet,
                                         m_turretWeapon, false);
        ++m_turretShots;
        rec.nextShotAt = m_clock + 60.0 / kTurretRpm; // tracers/audio are TF-W4
    }

    void TFDeployableSystem::TickAmmoPack(Rec& rec)
    {
        if (!m_ctx->players || !m_ctx->damage || m_clock < rec.nextPulseAt)
            return;
        rec.nextPulseAt = m_clock + kAmmoPackPulseSec;

        // W3 stand-in effect: small heal pulse to same-faction pawns in 5 m.
        // TF-W4: real mag/reserve refill (server ShooterState + client ammo sync).
        const float r2 = kAmmoPackRadiusM * kAmmoPackRadiusM;
        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& p)
            {
                if (p.faction != rec.view.faction || Dist2(rec.view.pos, p.pos) > r2)
                    return;
                m_ctx->damage->ServerHeal(p.entity, kAmmoPackHealHp);
                m_healGiven += kAmmoPackHealHp;
            });
    }

    void TFDeployableSystem::TickMedBeacon(Rec& rec, float dt)
    {
        if (!m_ctx->players || !m_ctx->damage)
            return;
        const float r2 = kBeaconRadiusM * kBeaconRadiusM;
        const float amount = kBeaconHealPerSec * dt;
        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& p)
            {
                if (p.faction != rec.view.faction || Dist2(rec.view.pos, p.pos) > r2)
                    return;
                m_ctx->damage->ServerHeal(p.entity, amount); // clamps, never revives
                m_healGiven += amount;
            });
    }

    void TFDeployableSystem::TickResupplyStation(Rec& rec)
    {
        if (!m_ctx->players || !m_ctx->damage || m_clock < rec.nextPulseAt)
            return;
        rec.nextPulseAt = m_clock + kResupplyPulseSec;
        const float r2 = kResupplyRadiusM * kResupplyRadiusM;

        // Infantry: heal pulse (same TF-W4 real-ammo-refill caveat as FabAmmoPack).
        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& p)
            {
                if (p.faction != rec.view.faction || Dist2(rec.view.pos, p.pos) > r2)
                    return;
                m_ctx->damage->ServerHeal(p.entity, kResupplyHealHp);
                m_healGiven += kResupplyHealHp;
            });

        // Field repair: friendly deployables in radius. Only VALUES of other
        // records change (no insert/erase), so this nested walk is safe inside
        // ServerTick's iteration.
        for (auto& [otherId, other] : m_deployables)
        {
            if (otherId == rec.view.entity || other.view.faction != rec.view.faction)
                continue;
            if (other.view.health >= other.view.maxHealth || Dist2(rec.view.pos, other.view.pos) > r2)
                continue;
            const float before = other.view.health;
            other.view.health = std::min(other.view.maxHealth, other.view.health + kResupplyRepairHp);
            m_repairGiven += other.view.health - before;
#ifdef ENABLE_NETWORKING
            if (ServerNetActive())
                SendUpdate(other.view);
#endif
        }
    }

    void TFDeployableSystem::TickAVTurret(Rec& rec)
    {
        if (!m_ctx->vehicles || m_clock < rec.nextShotAt)
            return;

        const float muzzle[3] = {rec.view.pos[0], rec.view.pos[1] + kAVTurretMuzzleY, rec.view.pos[2]};

        // Nearest alive ENEMY vehicle in range with terrain LoS (same acquire
        // shape as the infantry turret; vehicles only — never pawns).
        const float range2 = kAVTurretRangeM * kAVTurretRangeM;
        EntityId best = 0;
        float bestD2 = range2;
        m_ctx->vehicles->ForEachVehicle(
            [&](const TFVehicleInfo& v)
            {
                if (v.faction == rec.view.faction || v.faction == FactionId::None || v.hp <= 0.0f)
                    return;
                const float aim[3] = {v.pos[0], v.pos[1] + kAVTurretAimY, v.pos[2]};
                const float d2 = Dist2(rec.view.pos, v.pos);
                if (d2 > bestD2 || !TurretHasLoS(muzzle, aim))
                    return;
                best = v.entity;
                bestD2 = d2;
            });
        rec.targetPawn = best; // target slot reused for the debug panel
        if (best == 0)
            return;

        // Lazy killfeed weapon id: dedicated "av_turret" row if the data lane adds
        // one; falls back to "-" in the killfeed like the infantry turret.
        if (m_avWeapon == kInvalidWeapon && m_ctx->data && m_ctx->data->IsLoaded())
            if (const WeaponDef* wd = m_ctx->data->GetWeaponByKey("av_turret"))
                m_avWeapon = wd->id;

        // Deferred to after ServerTick's loop — see the apply site for why.
        m_pendingAvShots.push_back({best, rec.view.entity, rec.view.owner});
        ++m_avShots;
        rec.nextShotAt = m_clock + kAVTurretShotSec;
    }

} // namespace Terrafront
