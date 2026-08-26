/**
 * @file TFWeaponSystemRemoteFire.cpp
 * @brief TFWeaponSystem remote/distant fire audio (W8 audio-polish lane):
 *        distance-bucketed ClientOnRemoteFire (near = the weapon's own clip at
 *        reduced volume, distant = capped faction tail), the listener-position
 *        helper and the faction tail table. Split from TFWeaponSystem.cpp.
 */
#include "Game/TFWeaponSystem.h"

#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponMath.h"

#include "Camera/SparkEngineCamera.h"
#include "Spark/IEngineContext.h"

#include <algorithm>
#include <cmath>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Remote / distant fire audio (W8 audio-polish lane)
    // ---------------------------------------------------------------------------

    namespace
    {
        constexpr float kRemoteFireNearM = 60.0f;    // inside: the weapon's own clip, quieter
        constexpr float kRemoteFireMaxM = 600.0f;    // beyond: inaudible, skip entirely
        constexpr double kDistantCapWindowSec = 1.2; // cap window for concurrent tails
    } // namespace

    bool TFWeaponSystem::LocalListenerPos(float out[3]) const
    {
        if (const SparkEngineCamera* cam = m_ctx->engine ? m_ctx->engine->GetCamera() : nullptr)
        {
            const auto p = cam->GetPosition();
            out[0] = p.x;
            out[1] = p.y;
            out[2] = p.z;
            return true;
        }
        PawnInfo pawn;
        if (m_ctx->players && m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn))
        {
            out[0] = pawn.pos[0];
            out[1] = pawn.pos[1] + WeaponMath::kEyeHeightM;
            out[2] = pawn.pos[2];
            return true;
        }
        return false;
    }

    const char* TFWeaponSystem::DistantTailFor(FactionId faction)
    {
        switch (faction)
        {
        case FactionId::MRA:
            return "Audio/MMOFPS/weapons/distant_fire_mra.wav";
        case FactionId::HLX:
            return "Audio/MMOFPS/weapons/distant_fire_hlx.wav";
        case FactionId::AUC:
            return "Audio/MMOFPS/weapons/distant_fire_auc.wav";
        default:
            return "Audio/MMOFPS/weapons/distant_fire_common.wav";
        }
    }

    void TFWeaponSystem::ClientOnRemoteFire(PlayerId shooter, const PawnInfo& pawn, const WeaponDef& def)
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;
        if (shooter == m_ctx->localPlayer)
            return; // ClientTriggerFire already played the first-person clip

        float listener[3];
        if (!LocalListenerPos(listener))
            return;

        const float dx = pawn.pos[0] - listener[0];
        const float dy = (pawn.pos[1] + WeaponMath::kEyeHeightM) - listener[1];
        const float dz = pawn.pos[2] - listener[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist >= kRemoteFireMaxM)
            return;

        m_remoteFireHeat = std::min(1.0f, m_remoteFireHeat + 0.12f);

        if (dist < kRemoteFireNearM)
        {
            // Near bucket: the weapon's own report, volume falling from just
            // under the local player's 0.8 down to a murmur at the bucket edge.
            const float t = dist / kRemoteFireNearM;
            const float vol = 0.65f * (1.0f - t) + 0.12f * t;
            if (!def.audioFireVariants.empty())
                PlayWeaponAudio(def.audioFireVariants[m_remoteFireSeq++ % def.audioFireVariants.size()], vol);
            else
                PlayWeaponAudio(def.audioFire, vol);
            return;
        }

        // Distant bucket: faction tail only, capped so a 12-bot firefight does
        // not spam one-shots (at most kMaxDistantOneShots starts per window).
        int recent = 0;
        for (double t0 : m_distantPlayTimes)
        {
            if (m_clock - t0 < kDistantCapWindowSec)
                ++recent;
        }
        if (recent >= kMaxDistantOneShots)
            return;
        m_distantPlayTimes[m_distantPlayCursor] = m_clock;
        m_distantPlayCursor = (m_distantPlayCursor + 1) % kMaxDistantOneShots;

        const float t = (dist - kRemoteFireNearM) / (kRemoteFireMaxM - kRemoteFireNearM);
        const float vol = 0.45f * (1.0f - t) + 0.05f * t;
        PlayWeaponAudio(DistantTailFor(pawn.faction), vol);
    }

} // namespace Terrafront
