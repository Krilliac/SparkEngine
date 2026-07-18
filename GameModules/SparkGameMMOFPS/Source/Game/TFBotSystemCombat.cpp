/**
 * @file TFBotSystemCombat.cpp
 * @brief TFBotSystem combat: target acquisition with hysteresis + rough LoS,
 *        real TF_FireEvent fire throttled to the weapon RoF, and the W9
 *        situational class-ability triggers. Split from TFBotSystem.cpp; the
 *        shared tuning constants and the ability-seam shim live in
 *        TFBotSystemInternal.h.
 */
#include "Game/TFBotSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFBotSystemInternal.h"
#include "Game/TFChaosHarness.h"
#include "Game/TFMovementModel.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponMath.h"
#include "Game/TFWeaponSystem.h"
#include "Net/TFNetProtocol.h"
#include "World/TFWorldSetup.h"

#include <algorithm>

namespace Terrafront
{

    using namespace BotDetail;

    bool TFBotSystem::AcquireTarget(const Bot& bot, const PawnInfo& self, EntityId& outTarget,
                                    float outTargetPos[3]) const
    {
        if (!m_ctx->players)
            return false;

        struct Scan
        {
            const Bot* bot;
            const PawnInfo* self;
            EntityId best = 0;
            float bestD2 = kEngageRangeM * kEngageRangeM;
            float bestPos[3]{};
        } scan;
        scan.bot = &bot;
        scan.self = &self;

        // Hysteresis: an already-engaged target stays valid out to 70 m.
        const EntityId current = bot.targetEntity;

        m_ctx->players->ForEachAlivePawn(
            [&scan, current](const PawnInfo& p)
            {
                if (p.entity == scan.self->entity || !p.alive)
                    return;
                if (p.faction == scan.bot->faction || p.faction == FactionId::None)
                    return;
                const float dx = p.pos[0] - scan.self->pos[0];
                const float dy = p.pos[1] - scan.self->pos[1];
                const float dz = p.pos[2] - scan.self->pos[2];
                float d2 = dx * dx + dy * dy + dz * dz;
                if (p.entity == current && d2 <= kDisengageRangeM * kDisengageRangeM)
                    d2 *= 0.25f; // sticky current target
                if (d2 < scan.bestD2)
                {
                    scan.bestD2 = d2;
                    scan.best = p.entity;
                    scan.bestPos[0] = p.pos[0];
                    scan.bestPos[1] = p.pos[1];
                    scan.bestPos[2] = p.pos[2];
                }
            });

        if (scan.best == 0)
            return false;

        const float eye[3] = {self.pos[0], self.pos[1] + kTFEyeHeightM, self.pos[2]};
        const float chest[3] = {scan.bestPos[0], scan.bestPos[1] + kChestHeightM, scan.bestPos[2]};
        if (!HasLineOfSight(eye, chest))
            return false;

        outTarget = scan.best;
        outTargetPos[0] = scan.bestPos[0];
        outTargetPos[1] = scan.bestPos[1];
        outTargetPos[2] = scan.bestPos[2];
        return true;
    }

    bool TFBotSystem::HasLineOfSight(const float eye[3], const float target[3]) const
    {
        if (!m_ctx->world)
            return true;
        float dir[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
        const float dist = WeaponMath::Len3(dir);
        if (dist < kLosStepM || !WeaponMath::Normalize3(dir))
            return true;
        // Terrain march, same idea as TFWeaponSystem::TerrainBlocked (coarser step:
        // this runs per brain tick, not per shot).
        for (float t = kLosStepM; t < dist; t += kLosStepM)
        {
            const float x = eye[0] + dir[0] * t;
            const float y = eye[1] + dir[1] * t;
            const float z = eye[2] + dir[2] * t;
            if (y < m_ctx->world->TerrainHeightAt(x, z))
                return false;
        }
        return true;
    }

    // ---------------------------------------------------------------------------
    // Combat (real TF_FireEvent -> ServerHandleFire, throttled to the weapon RoF)
    // ---------------------------------------------------------------------------

    void TFBotSystem::TryFire(Bot& bot, double now)
    {
        if (!m_ctx->players || !m_ctx->weapons || bot.weapon == kInvalidWeapon)
            return;

        // Mag/reload model mirroring the server's approximate mag in ValidateFire
        // (refills after reloadSec of silence) so bot shots are never rejected.
        if (bot.magLeft <= 0)
        {
            if (now < bot.reloadDoneAt)
                return;
            bot.magLeft = bot.magSize;
        }
        if (now < bot.nextFireAt)
            return;

        PawnInfo self, target;
        if (!m_ctx->players->GetPawnByPlayer(bot.id, self) || !self.alive)
            return;
        if (!m_ctx->players->GetPawnByEntity(bot.targetEntity, target) || !target.alive)
        {
            bot.targetEntity = 0;
            return;
        }

        float origin[3] = {self.pos[0], self.pos[1] + kTFEyeHeightM, self.pos[2]};
        float dir[3] = {target.pos[0] - origin[0], (target.pos[1] + kChestHeightM) - origin[1],
                        target.pos[2] - origin[2]};
        const float dist = WeaponMath::Len3(dir);
        if (dist > kDisengageRangeM || !WeaponMath::Normalize3(dir))
            return;
        WeaponMath::PerturbCone(dir, kAimErrorDeg, m_rng); // +-1.5 deg human error

        TF_FireEvent ev{};
        ev.seq = bot.seq;
        ev.weaponId = bot.weapon;
        ev.originX = origin[0];
        ev.originY = origin[1];
        ev.originZ = origin[2];
        ev.dirX = dir[0];
        ev.dirY = dir[1];
        ev.dirZ = dir[2];
        m_ctx->weapons->ServerHandleFire(bot.id, ev);

        ++m_shotsFired;
        bot.nextFireAt = now + bot.rofIntervalSec;
        if (--bot.magLeft <= 0)
            bot.reloadDoneAt = now + bot.reloadSec;
    }

    // ---------------------------------------------------------------------------
    // Class abilities (W9 bots-v2; real seam when the abilities lane is present)
    // ---------------------------------------------------------------------------

    void TFBotSystem::TryClassAbility(Bot& bot, const PawnInfo& self, double now, TF_ClientInput& in)
    {
        if (now < bot.nextAbilityAt || !m_ctx->players)
            return;

        bool want = false;
        switch (bot.cls)
        {
        case ClassId::Medtech:
        {
            // Hurt self, or a hurt friendly inside heal reach.
            want = HealthFrac(bot, self) < kMedtechHurtFrac;
            if (!want)
            {
                m_ctx->players->ForEachAlivePawn(
                    [&](const PawnInfo& p)
                    {
                        if (want || p.entity == self.entity || p.faction != bot.faction)
                            return;
                        const float dx = p.pos[0] - self.pos[0];
                        const float dz = p.pos[2] - self.pos[2];
                        if (dx * dx + dz * dz > kMedtechHealRadiusM * kMedtechHealRadiusM)
                            return;
                        float maxPool = 1000.0f; // ClassDef defaults
                        if (m_ctx->data)
                            if (const ClassDef* cd = m_ctx->data->GetClass(p.cls))
                                maxPool = std::max(1.0f, cd->health + cd->shield);
                        if ((p.health + p.shield) / maxPool < kMedtechHurtFrac)
                            want = true;
                    });
            }
            break;
        }
        case ClassId::Bulwark:
            // Overshield when actually taking hits (or brawling on low health).
            want = bot.underFire || (bot.state == BotState::Fighting && bot.lowHealth);
            break;
        case ClassId::Striker:
            // Jets to close a wide gap on the current target.
            if (bot.state == BotState::Fighting && bot.targetEntity != 0)
            {
                PawnInfo tp;
                if (m_ctx->players->GetPawnByEntity(bot.targetEntity, tp) && tp.alive)
                {
                    const float dx = tp.pos[0] - self.pos[0];
                    const float dz = tp.pos[2] - self.pos[2];
                    want = dx * dx + dz * dz > kStrikerGapM * kStrikerGapM;
                }
            }
            break;
        default:
            break; // Ghost/Fabricator: no situational trigger this wave
        }
        if (!want)
            return;

        bot.nextAbilityAt = now + kAbilityCheckSec + static_cast<double>(m_rng() % 10u) * 0.1;

        // Input-driven path: harmless if nothing consumes the bit yet, and the
        // moment an input-handled ability lane lands, bots exercise it for free.
        in.buttons |= TFB_Ability;

        // Direct seam (compile-time optional; see the shim at the top of file).
        if (TryUseAbilitySeam(m_ctx, bot.id))
        {
            ++m_abilityUses;
            if (m_chaos)
                m_chaos->NoteAbilityUse();
        }
    }

} // namespace Terrafront
