/**
 * @file TFBotSystemNav.cpp
 * @brief TFBotSystem local obstacle avoidance (W12 bot-navigation): chest-
 *        height feeler rays against WorldStatic, path memory, pocket
 *        back-turns and the coarse no-progress unstick detector. Split from
 *        TFBotSystem.cpp; the shared tuning constants live in
 *        TFBotSystemInternal.h.
 */
#include "Game/TFBotSystem.h"

#include "Game/TFBotSystemInternal.h"
#include "Game/TFPlayerSystem.h"
#include "Net/TFNetProtocol.h"

// W12 bot-navigation: feeler rays against the live Jolt world (GetJoltSystem()
// probe — the no-Jolt stub hands out dummy bodies; TFImpactFx precedent).
#include "Physics/PhysicsSystem.h" // engine umbrella header; stub-safe when Jolt is absent
#include "Spark/IEngineContext.h"

#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>

namespace Terrafront
{

    using namespace BotDetail;

    // ---------------------------------------------------------------------------
    // Local obstacle avoidance (W12 bot-navigation)
    //
    // W11 root cause: bots nosed into watchtower leg-cage OBBs and stalemated
    // (no fights -> the chaos abilities gate failed and the walkable-gates data
    // was reverted). Feeler rays steer marching bots around static geometry
    // BEFORE they wedge; the coarse no-progress detector below catches whatever
    // slips through and breaks it with a random escape leg, escalating to the
    // existing chaos teleport-scatter after three consecutive failures (chaos
    // mode only — the scatter path already resets objectives + stuck state).
    // ---------------------------------------------------------------------------

    void TFBotSystem::ResetAvoidance(Bot& bot, float x, float z, double now)
    {
        bot.moveRefPos[0] = x;
        bot.moveRefPos[1] = z;
        bot.moveRefAt = now;
        bot.unstickUntil = 0.0;
        bot.backTurnUntil = 0.0;
        bot.blockedYawUntil = 0.0;
        bot.lastAvoidSide = 0;
    }

    float TFBotSystem::FeelerClearance(const float origin[3], float yaw) const
    {
        ::PhysicsSystem* physics = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetPhysics() : nullptr;
        if (!physics || !physics->GetJoltSystem())
            return kFeelerLenM; // no live Jolt world: nothing to feel, walk on
        const DirectX::XMFLOAT3 o{origin[0], origin[1], origin[2]};
        const DirectX::XMFLOAT3 d{std::sin(yaw), 0.0f, std::cos(yaw)};
        const RaycastHit hit = physics->RaycastFiltered(o, d, kFeelerLenM, CollisionLayers::WorldStatic);
        return hit.hasHit ? std::min(hit.distance, kFeelerLenM) : kFeelerLenM;
    }

    float TFBotSystem::SteerFeelers(Bot& bot, const PawnInfo& self, double now, float desiredYaw)
    {
        // Pocket escape in progress: hold the back-turn heading, no rays.
        if (now < bot.backTurnUntil)
            return bot.backTurnYaw;

        // Stagger: full pattern 1-in-4 thinks per bot in open field; every
        // think while actively working an obstacle (fresh blocked memory).
        bot.feelerPhase = static_cast<uint8_t>((bot.feelerPhase + 1u) & 3u);
        const bool active = now < bot.blockedYawUntil;
        if (bot.feelerPhase != 0 && !active)
            return desiredYaw;

        // Path memory: while the remembered blocked heading is fresh, bias away
        // from it instead of re-probing head-on every time the scorer re-aims.
        const auto memoryBias = [&](float yaw)
        {
            if (now >= bot.blockedYawUntil)
                return yaw;
            const float delta = WrapPi(yaw - bot.lastBlockedYaw);
            if (std::fabs(delta) >= kFeelerSideRad)
                return yaw;
            float side = static_cast<float>(bot.lastAvoidSide);
            if (side == 0.0f)
                side = delta >= 0.0f ? 1.0f : -1.0f;
            return WrapPi(yaw + side * kFeelerSideRad);
        };

        const float origin[3] = {self.pos[0], self.pos[1] + kChestHeightM, self.pos[2]};
        const float fwd = FeelerClearance(origin, desiredYaw);
        if (fwd >= kFeelerLenM)
            return memoryBias(desiredYaw); // clear ahead

        // Forward blocked: remember the heading and probe the side feelers.
        ++m_feelerBlocked;
        bot.lastBlockedYaw = desiredYaw;
        bot.blockedYawUntil = now + kBlockedMemorySec;

        const float left = FeelerClearance(origin, WrapPi(desiredYaw - kFeelerSideRad));
        const float right = FeelerClearance(origin, WrapPi(desiredYaw + kFeelerSideRad));
        if (left < kFeelerLenM)
            ++m_feelerBlocked;
        if (right < kFeelerLenM)
            ++m_feelerBlocked;

        if (left < kFeelerLenM && right < kFeelerLenM)
        {
            // Boxed in: back-turn 120 deg for a second and re-approach.
            const float sign = (m_rng() & 1u) != 0u ? 1.0f : -1.0f;
            bot.backTurnYaw = WrapPi(desiredYaw + sign * kBackTurnRad);
            bot.backTurnUntil = now + kBackTurnSec;
            bot.lastAvoidSide = 0;
            return bot.backTurnYaw;
        }

        // Steer along the least-blocked feeler; ties break away from the
        // remembered blocked heading via the previous side, else randomly.
        float side;
        if (left > right)
            side = -1.0f;
        else if (right > left)
            side = 1.0f;
        else if (bot.lastAvoidSide != 0)
            side = static_cast<float>(bot.lastAvoidSide);
        else
            side = (m_rng() & 1u) != 0u ? 1.0f : -1.0f;
        bot.lastAvoidSide = side > 0.0f ? int8_t{1} : int8_t{-1};
        return WrapPi(desiredYaw + side * kAvoidSteerRad);
    }

    void TFBotSystem::ApplyAvoidance(Bot& bot, const PawnInfo& self, double now, TF_ClientInput& in)
    {
        // --- no-progress unstick detector (coarser than the jump nudge) ---
        const float mdx = self.pos[0] - bot.moveRefPos[0];
        const float mdz = self.pos[2] - bot.moveRefPos[1];
        if (mdx * mdx + mdz * mdz >= kUnstickMinMoveM * kUnstickMinMoveM)
        {
            bot.moveRefPos[0] = self.pos[0];
            bot.moveRefPos[1] = self.pos[2];
            bot.moveRefAt = now;
            bot.unstickCount = 0; // making progress again
        }
        else if (now - bot.moveRefAt >= kUnstickWindowSec)
        {
            bot.moveRefPos[0] = self.pos[0];
            bot.moveRefPos[1] = self.pos[2];
            bot.moveRefAt = now;
            ++m_unsticks;
            if (bot.unstickCount < kUnstickScatterAfter)
                ++bot.unstickCount;
            if (bot.unstickCount >= kUnstickScatterAfter && m_chaosActive)
            {
                // Persistent stuck: fall back to the chaos teleport-scatter
                // (consumed at the top of the next alive think — it re-rolls
                // the drop point and resets objective + stuck state there).
                bot.chaosScatterPending = true;
                bot.unstickCount = 0;
                ++m_stuckTeleports;
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] bot 0x%08X persistently stuck -> chaos scatter", bot.id);
            }
            else
            {
                // Random 90-150 deg escape heading for 2 s, then re-seek.
                const float sign = (m_rng() & 1u) != 0u ? 1.0f : -1.0f;
                const float mag = 1.5708f + static_cast<float>(m_rng() % 61u) * 0.0174533f;
                bot.unstickYaw = WrapPi(in.viewYaw + sign * mag);
                bot.unstickUntil = now + kUnstickSteerSec;
            }
        }

        // Escape leg overrides the objective heading while it runs.
        if (now < bot.unstickUntil)
        {
            in.viewYaw = bot.unstickYaw;
            in.moveY = 127;
        }

        // Feelers steer whatever heading survived the above.
        in.viewYaw = SteerFeelers(bot, self, now, in.viewYaw);
    }

} // namespace Terrafront
