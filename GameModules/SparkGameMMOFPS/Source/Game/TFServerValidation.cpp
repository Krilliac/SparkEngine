/**
 * @file TFServerValidation.cpp
 * @brief Server-authoritative anti-cheat sanity layer. See TFServerValidation.h
 *        for the full design writeup (movement / fire-rate / position-sanity).
 */
#include "Game/TFServerValidation.h"

#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>

namespace Terrafront
{

    namespace
    {
        // Horizontal displacement in a single tick is already bounded to
        // ~maxHorizSpeed*dt by the shared movement model's own clamp (TFMoveStep
        // step 3) when exactly one input is consumed; this fudge only needs to
        // cover floating rounding + terrain-slope edge cases for that normal
        // case while still firmly flagging the multi-input-per-tick catch-up
        // path (kMaxInputsPerTick in TFServerSim.cpp: 2x/3x overshoot).
        constexpr float kHorizFudge = 1.5f;
        // Vertical displacement per tick is left generous on purpose (jumps,
        // long falls and jet thrust are already bounded by the movement model;
        // this is only a backstop against a gross instantaneous teleport-style
        // bug/exploit, not a tight physics cap).
        constexpr float kMaxVerticalDeltaPerTickM = 3.0f;
        // A clamp ratio at or above this multiple of the allowed cap is logged
        // (throttled) as a "spike" in addition to being counted -- lets a live
        // server operator spot an obvious speed-hack attempt in the log instead
        // of only seeing it after the fact in tf_cheat_stats.
        constexpr float kSpikeRatio = 2.5f;
        constexpr double kSpikeLogThrottleSec = 5.0;
    } // namespace

    TFServerValidation& TFServerValidation::Get()
    {
        static TFServerValidation instance;
        return instance;
    }

    void TFServerValidation::ValidateMovementTick(PlayerId player, const float prevPos[3], float pos[3],
                                                   float maxHorizSpeed, float dt, double now)
    {
        if (auto ex = m_exemptOnce.find(player); ex != m_exemptOnce.end())
        {
            m_exemptOnce.erase(ex);
            return; // explicit server reposition this tick (redeploy / tf_tp) -- not a validated walk
        }
        if (dt <= 0.0f || maxHorizSpeed <= 0.0f)
            return; // rooted (ability speedMult 0) or a degenerate tick -- nothing to validate

        bool clamped = false;
        float worstRatio = 1.0f;

        const float dx = pos[0] - prevPos[0];
        const float dz = pos[2] - prevPos[2];
        const float horizDist = std::sqrt(dx * dx + dz * dz);
        const float horizCap = maxHorizSpeed * dt * kHorizFudge;
        if (horizDist > horizCap && horizDist > 1.0e-5f)
        {
            const float k = horizCap / horizDist;
            pos[0] = prevPos[0] + dx * k;
            pos[2] = prevPos[2] + dz * k;
            worstRatio = std::max(worstRatio, horizDist / horizCap);
            clamped = true;
        }

        const float dy = pos[1] - prevPos[1];
        const float absDy = std::fabs(dy);
        if (absDy > kMaxVerticalDeltaPerTickM)
        {
            pos[1] = prevPos[1] + (dy > 0.0f ? kMaxVerticalDeltaPerTickM : -kMaxVerticalDeltaPerTickM);
            worstRatio = std::max(worstRatio, absDy / kMaxVerticalDeltaPerTickM);
            clamped = true;
        }

        if (!clamped)
            return;

        TFViolationStats& st = m_stats[player];
        ++st.movementClamps;
        if (worstRatio < kSpikeRatio)
            return;

        ++st.movementSpikes;
        double& lastLog = m_lastSpikeLog[player];
        if (now - lastLog > kSpikeLogThrottleSec)
        {
            lastLog = now;
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF-anticheat] movement spike: player %u displacement %.1fx the plausible cap (clamped, "
                           "%u total clamps)",
                           player, worstRatio, st.movementClamps);
        }
    }

    void TFServerValidation::NoteExemptTeleport(PlayerId player) { m_exemptOnce[player] = true; }

    bool TFServerValidation::CheckFireOrigin(PlayerId player, const float claimed[3], const float trusted[3],
                                             float maxDivergenceM)
    {
        const float dx = claimed[0] - trusted[0];
        const float dy = claimed[1] - trusted[1];
        const float dz = claimed[2] - trusted[2];
        const float dist2 = dx * dx + dy * dy + dz * dz;
        if (dist2 <= maxDivergenceM * maxDivergenceM)
            return true;

        TFViolationStats& st = m_stats[player];
        ++st.fireOriginRejects;
        SPARK_LOG_WARN(Spark::LogCategory::Game,
                       "[TF-anticheat] player %u fire-event origin diverged %.1fm from the trusted pawn position "
                       "(rejected, %u total)",
                       player, std::sqrt(dist2), st.fireOriginRejects);
        return false;
    }

    void TFServerValidation::RecordFireRateReject(PlayerId player) { ++m_stats[player].fireRateRejects; }

    void TFServerValidation::ClearPlayer(PlayerId player)
    {
        m_stats.erase(player);
        m_exemptOnce.erase(player);
        m_lastSpikeLog.erase(player);
    }

} // namespace Terrafront
