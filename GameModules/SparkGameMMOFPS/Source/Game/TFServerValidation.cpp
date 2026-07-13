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

        // AllowInput's token bucket. Refill rate is the server tick rate with
        // a fudge multiplier -- generous enough that a legitimate client's
        // own frame-time jitter (its inputs are enqueued once per client
        // frame, not perfectly paced to the server's fixed tick) never trips
        // it, while still firmly bounding a client that floods TF_ClientInput
        // packets far faster than the server can ever consume them (see file
        // header: this is what actually closes the input-queue catch-up
        // exploit, not just the queue-depth cap in TFServerSim::EnqueueInput).
        constexpr float kInputRateFudge = 1.5f;
        constexpr float kInputRefillPerSec = kServerTickHz * kInputRateFudge;
        // Small burst allowance -- covers a client briefly bunching a couple
        // of frames' worth of inputs (e.g. a hitch catching back up), same
        // spirit as TFWeaponSystem::ValidateFire's 2-token burst cap.
        constexpr float kInputBucketCapacity = 4.0f;
        constexpr double kInputRejectLogThrottleSec = 5.0;

        // KICK ESCALATION (W14, file header): per-counter weights feeding
        // ViolationScore(). fireOriginRejects is weighted highest -- a claimed
        // muzzle position that grossly diverges from the server's trusted pawn
        // position has essentially no legitimate cause. movementSpikes (the
        // >= kSpikeRatio subset of clamps) is next -- a sustained catch-up
        // flood clamped hard enough to log as a spike. fireRateRejects and
        // inputRateRejects are weighted lowest since their underlying gates
        // (TFWeaponSystem::ValidateFire's burst-2 bucket, AllowInput's
        // burst-4 bucket) are already the most jitter-tolerant of the four.
        constexpr uint32_t kWeightMovementSpike = 3;
        constexpr uint32_t kWeightFireOriginReject = 4;
        constexpr uint32_t kWeightFireRateReject = 2;
        constexpr uint32_t kWeightInputRateReject = 1;
        // Requires accumulating several violations, never a single one (the
        // highest single weight above is 4): e.g. 3 movement spikes (9) still
        // doesn't trip, but 3 spikes + 1 fire-origin reject (13) does.
        constexpr uint32_t kKickThreshold = 10;
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

    bool TFServerValidation::AllowInput(PlayerId player, double now)
    {
        auto [tokIt, tokInserted] = m_inputTokens.try_emplace(player, kInputBucketCapacity);
        if (tokInserted)
            m_inputLastRefill[player] = now;

        double& lastRefill = m_inputLastRefill[player];
        float& tokens = tokIt->second;
        tokens = std::min(kInputBucketCapacity, tokens + static_cast<float>((now - lastRefill) * kInputRefillPerSec));
        lastRefill = now;

        if (tokens < 1.0f)
        {
            TFViolationStats& st = m_stats[player];
            ++st.inputRateRejects;
            double& lastLog = m_lastInputRejectLog[player];
            if (now - lastLog > kInputRejectLogThrottleSec)
            {
                lastLog = now;
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "[TF-anticheat] player %u input rate exceeded the 60Hz(+fudge) budget (dropped, "
                               "%u total)",
                               player, st.inputRateRejects);
            }
            return false;
        }

        tokens -= 1.0f;
        return true;
    }

    uint32_t TFServerValidation::ViolationScore(PlayerId player) const
    {
        const auto it = m_stats.find(player);
        if (it == m_stats.end())
            return 0;

        const TFViolationStats& st = it->second;
        return st.movementSpikes * kWeightMovementSpike + st.fireOriginRejects * kWeightFireOriginReject +
               st.fireRateRejects * kWeightFireRateReject + st.inputRateRejects * kWeightInputRateReject;
    }

    bool TFServerValidation::ShouldKick(PlayerId player) const { return ViolationScore(player) >= kKickThreshold; }

    void TFServerValidation::ClearPlayer(PlayerId player)
    {
        m_stats.erase(player);
        m_exemptOnce.erase(player);
        m_lastSpikeLog.erase(player);
        m_inputTokens.erase(player);
        m_inputLastRefill.erase(player);
        m_lastInputRejectLog.erase(player);
    }

} // namespace Terrafront
