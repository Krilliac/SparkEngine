/**
 * @file TFBotSystemBrain.cpp
 * @brief TFBotSystem 5 Hz brain: per-life state reset, the alive think
 *        (combat / march / vehicle arbitration), distance-weighted objective
 *        scoring and fireteam cohesion. Split from TFBotSystem.cpp; the
 *        shared tuning constants and the W2 region-contract shim live in
 *        TFBotSystemInternal.h.
 */
#include "Game/TFBotSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFBotSystemInternal.h"
#include "Game/TFMovementModel.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Net/TFNetProtocol.h"
#include "World/TFRegionSystem.h"

#include <algorithm>
#include <cmath>

namespace Terrafront
{

    using namespace BotDetail;

    // ---------------------------------------------------------------------------
    // Brain (5 Hz per bot, staggered)
    // ---------------------------------------------------------------------------

    void TFBotSystem::Think(Bot& bot, double now)
    {
        PawnInfo self;
        const bool alive = m_ctx->players && m_ctx->players->GetPawnByPlayer(bot.id, self) && self.alive;

        if (!alive)
        {
            bot.wantMove = false;
            bot.targetEntity = 0;
            bot.vehicleEntity = 0; // TFVehicleSystem unseats the corpse on EvPlayerKilled
            if (bot.state == BotState::Deploying)
            {
                if (now >= bot.nextSpawnTryAt)
                    TrySpawn(bot, now);
            }
            else if (bot.state != BotState::Dead)
            {
                bot.state = BotState::Dead;
                bot.nextSpawnTryAt = now + kRespawnWaitSec; // 8 s server timer + margin
            }
            else if (now >= bot.nextSpawnTryAt)
            {
                TrySpawn(bot, now);
            }
            return;
        }

        if (bot.state == BotState::Dead || bot.state == BotState::Deploying)
        {
            // Fresh pawn: reset per-life state.
            bot.state = BotState::Moving;
            bot.magLeft = bot.magSize;
            bot.reloadDoneAt = 0.0;
            bot.vehicleEntity = 0;
            bot.enterTries = 0;
            bot.lowHealth = false;
            bot.reverseUntil = 0.0;
            bot.wedgeCount = 0;
            bot.lastWedgeAt = 0.0;
            bot.lastPool = -1.0f;
            bot.underFire = false;
            if (m_chaosActive)
                bot.chaosScatterPending = true; // re-scatter every chaos life for coverage
            bot.stuckRefPos[0] = self.pos[0];
            bot.stuckRefPos[1] = self.pos[1];
            bot.stuckRefPos[2] = self.pos[2];
            bot.stuckSince = now;
            bot.jumping = false;
            ResetAvoidance(bot, self.pos[0], self.pos[2], now); // W12: fresh nav state per life
            bot.unstickCount = 0;
        }

        ThinkAlive(bot, self, now);
    }

    void TFBotSystem::ThinkAlive(Bot& bot, const PawnInfo& self, double now)
    {
        // W9 bots-v2: a REFUSED exit (airborne Vulture — the seat-op is
        // landed-gated) leaves the bot seated after ExitVehicle already reset
        // its plan. Re-latch into Driving so it keeps flying/landing instead of
        // walking-in-place through the seated-input forwarder. The one-tick
        // exit latch after a SUCCESSFUL exit also reports IsSeated, but its
        // seat slot is already freed — the seat scan below tells them apart.
        if (bot.state != BotState::Driving && m_ctx->vehicles && m_ctx->vehicles->IsSeated(bot.id))
        {
            EntityId riding = 0;
            m_ctx->vehicles->ForEachVehicle(
                [&](const TFVehicleInfo& v)
                {
                    if (riding != 0)
                        return;
                    for (uint8_t i = 0; i < v.seatCount; ++i)
                    {
                        if (v.seats[i] == bot.id)
                        {
                            riding = v.entity;
                            return;
                        }
                    }
                });
            if (riding != 0)
            {
                bot.state = BotState::Driving;
                bot.vehicleEntity = riding;
                bot.stuckSince = now; // fresh wedge window for the retry
            }
        }

        // A seated bot rides the vehicle: infantry combat, stuck-jump and walking
        // objectives do not apply until it dismounts.
        if (bot.state == BotState::Driving)
        {
            ThinkDriving(bot, self, now);
            return;
        }

        // --- chaos: teleport-scatter for coverage (start of run + every life) ---
        if (m_chaosActive && bot.chaosScatterPending)
        {
            bot.chaosScatterPending = false;
            ChaosScatter(bot, now);
            bot.wantMove = false; // rebuild input from the new position next think
            return;
        }

        // --- chaos: periodic deployable / vehicle-purchase exercise ---
        if (m_chaosActive)
        {
            ChaosTryUtility(bot, now);
            // W9 pilots: buy the designated ride at the faction terminal.
            if (ChaosIsPilot(bot) && now >= bot.pilotBuyAt)
                ChaosPilotTryPurchase(bot, self, now);
        }

        // --- stuck detection: pos unchanged > 2 s -> hold jump ---
        const float dsx = self.pos[0] - bot.stuckRefPos[0];
        const float dsy = self.pos[1] - bot.stuckRefPos[1];
        const float dsz = self.pos[2] - bot.stuckRefPos[2];
        if (dsx * dsx + dsy * dsy + dsz * dsz > kStuckEpsM * kStuckEpsM)
        {
            bot.stuckRefPos[0] = self.pos[0];
            bot.stuckRefPos[1] = self.pos[1];
            bot.stuckRefPos[2] = self.pos[2];
            bot.stuckSince = now;
            bot.jumping = false;
        }
        else if (now - bot.stuckSince > kStuckWindowSec)
        {
            bot.jumping = true;
        }

        TF_ClientInput in{};
        in.weaponSlot = 0;

        // Fitness drives engage-vs-advance below.
        bot.lowHealth = HealthFrac(bot, self) < kLowHealthFrac;

        // Under-fire detection (W9 bots-v2): the pool dropped since last think.
        const float pool = self.health + self.shield;
        bot.underFire = bot.lastPool >= 0.0f && pool < bot.lastPool - 0.5f;
        bot.lastPool = pool;

        // --- combat: nearest enemy alive pawn within 60 m with rough LoS ---
        float targetPos[3];
        if (AcquireTarget(bot, self, bot.targetEntity, targetPos))
        {
            bot.state = BotState::Fighting;
            const float dx = targetPos[0] - self.pos[0];
            const float dz = targetPos[2] - self.pos[2];
            const float dy = (targetPos[1] + kChestHeightM) - (self.pos[1] + kTFEyeHeightM);
            const float dist = std::sqrt(dx * dx + dz * dz);

            in.viewYaw = std::atan2(dx, dz); // TF yaw basis: forward = (sin, 0, cos)
            in.viewPitch = std::atan2(dy, std::max(dist, 0.001f));
            // strafe oscillation either way; push when fit, give ground (target
            // stays in the sights — moveY is body-relative) when hurt/reloading.
            const bool reloading = bot.magLeft <= 0 && now < bot.reloadDoneAt;
            in.moveX = static_cast<int8_t>(std::sin(now * 2.6 + bot.strafePhase) * 110.0);
            if (bot.lowHealth || reloading)
                in.moveY = -90;
            else
                in.moveY = static_cast<int8_t>(dist > kHoldFireCloseM ? 70 : 0);
        }
        else
        {
            bot.targetEntity = 0;

            // --- objective (region march, contested-biased). W9 chaos pilots
            //     keep their scripted objective (terminal pad, then the far
            //     ride target) — the scorer would clobber it every think. ---
            if (!(m_chaosActive && ChaosIsPilot(bot)))
                PickObjective(bot, self.pos);

            // --- chaos: randomized objective override (coverage over optimality);
            //     fireteam cohesion is skipped in chaos to maximize scatter ---
            if (m_chaosActive)
                ChaosMaybeReroll(bot, now);

            // --- fireteam cohesion: followers adopt the leader's objective and
            //     regroup on the leader when strung out ---
            if (const Bot* leader = FireteamLeader(bot); !m_chaosActive && leader && leader != &bot)
            {
                PawnInfo lp;
                if (m_ctx->players->GetPawnByPlayer(leader->id, lp) && lp.alive)
                {
                    if (leader->objectiveRegion != kInvalidRegion)
                    {
                        bot.objectiveRegion = leader->objectiveRegion;
                        bot.objectiveX = leader->objectiveX;
                        bot.objectiveZ = leader->objectiveZ;
                    }
                    const float ldx = lp.pos[0] - self.pos[0];
                    const float ldz = lp.pos[2] - self.pos[2];
                    if (ldx * ldx + ldz * ldz > kRegroupBeyondM * kRegroupBeyondM)
                    {
                        bot.objectiveRegion = kInvalidRegion; // regroup leg, not a capture
                        bot.objectiveX = lp.pos[0];
                        bot.objectiveZ = lp.pos[2];
                    }
                }
            }

            // --- vehicle plan: long march + friendly wheels nearby -> drive ---
            if (!TryUseVehicle(bot, self, now, in))
            {
                bot.state = BotState::Moving;
                const float dx = bot.objectiveX - self.pos[0];
                const float dz = bot.objectiveZ - self.pos[2];
                const float dist = std::sqrt(dx * dx + dz * dz);

                in.viewYaw = std::atan2(dx, dz);
                in.viewPitch = 0.0f;
                in.moveY = 127;
                in.moveX = 0;
                // Chaos: weave while marching so bots hit obstacles at oblique
                // angles and exercise the slide path, not just head-on stops.
                if (m_chaosActive)
                    in.moveX = static_cast<int8_t>(std::sin(now * 1.7 + bot.strafePhase) * 70.0);
                if (dist > kSprintBeyondM)
                    in.buttons |= TFB_Sprint;
            }
        }

        // W12 bot-navigation: walking legs get local obstacle avoidance (feeler
        // steering + no-progress unstick). Combat movement is strafe-driven and
        // keeps the target in the sights, so only the march/approach states
        // steer around geometry; other states just re-arm the progress window.
        if (bot.state == BotState::Moving || bot.state == BotState::ToVehicle)
        {
            ApplyAvoidance(bot, self, now, in);
        }
        else
        {
            bot.moveRefPos[0] = self.pos[0];
            bot.moveRefPos[1] = self.pos[2];
            bot.moveRefAt = now;
        }

        if (bot.jumping)
            in.buttons |= TFB_Jump;

        // W9 bots-v2: situational class ability (rate-limited inside).
        TryClassAbility(bot, self, now, in);

        bot.input = in; // seq is stamped per fixed tick in FixedUpdate
        bot.wantMove = true;
    }

    // ---------------------------------------------------------------------------
    // Objectives
    // ---------------------------------------------------------------------------

    void TFBotSystem::PickObjective(Bot& bot, const float selfPos[3]) const
    {
        // Default: map center keeps the war converging even with no data.
        float mapCenter = 2048.0f;
        bot.objectiveRegion = kInvalidRegion;

        if (!m_ctx->data || !m_ctx->data->IsLoaded())
        {
            bot.objectiveX = bot.objectiveZ = mapCenter;
            return;
        }
        const ContinentDef& cont = m_ctx->data->GetContinent();
        mapCenter = cont.sizeM * 0.5f;

        // Distance-weighted scoring: hot regions (contested, mid-capture, own
        // ground under attack) shrink their effective distance so nearby quiet
        // frontier only wins when nothing is burning. With no live region system
        // every weight is 1.0 -> identical to plain nearest-region marching.
        const RegionDef* best = nullptr;
        float bestScore = 1.0e30f;
        for (const RegionDef& r : cont.regions)
        {
            if (r.tier == "skyanchor")
                continue;
            const FactionId owner = QueryRegionOwner(m_ctx->regions, r.id, FallbackOwner(cont, r));

            FactionId capturing = FactionId::None;
            bool contested = false;
            const float progress = QueryCaptureProgress(m_ctx->regions, r.id, capturing, contested);
            const bool underAttack = progress > 0.0f && capturing != FactionId::None && capturing != owner;

            float weight = 1.0f;
            if (owner == bot.faction)
            {
                if (!underAttack && !contested)
                    continue;   // quiet home ground — nothing to do there
                weight = 0.15f; // defend own regions under attack first
            }
            else
            {
                if (!QueryRegionCapturable(m_ctx->regions, r.id, bot.faction, /*fallback*/ true))
                    continue; // lattice rule once the region system answers
                if (contested)
                    weight = 0.25f; // join the fight on the point
                else if (capturing == bot.faction && progress > 0.0f)
                    weight = 0.20f; // finish captures we already started
                else if (underAttack)
                    weight = 0.50f; // third-party brawl worth crashing
            }

            const float dx = r.centerX - selfPos[0];
            const float dz = r.centerZ - selfPos[2];
            const float score = (dx * dx + dz * dz) * weight;
            if (score < bestScore)
            {
                bestScore = score;
                best = &r;
            }
        }

        if (!best)
        {
            bot.objectiveX = bot.objectiveZ = mapCenter;
            return;
        }

        bot.objectiveRegion = best->id;
        // Nearest capture point in the chosen region; region center as fallback.
        bot.objectiveX = best->centerX;
        bot.objectiveZ = best->centerZ;
        float bestCp2 = 1.0e30f;
        for (const auto& cp : best->capturePoints)
        {
            const float dx = cp[0] - selfPos[0];
            const float dz = cp[1] - selfPos[2];
            const float d2 = dx * dx + dz * dz;
            if (d2 < bestCp2)
            {
                bestCp2 = d2;
                bot.objectiveX = cp[0];
                bot.objectiveZ = cp[1];
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Fireteam & fitness
    // ---------------------------------------------------------------------------

    const TFBotSystem::Bot* TFBotSystem::FireteamLeader(const Bot& bot) const
    {
        if (!m_ctx || !m_ctx->players)
            return nullptr;
        for (const Bot& b : m_bots)
        {
            if (b.faction != bot.faction)
                continue;
            PawnInfo p;
            if (m_ctx->players->GetPawnByPlayer(b.id, p) && p.alive)
                return &b;
        }
        return nullptr;
    }

    float TFBotSystem::HealthFrac(const Bot& bot, const PawnInfo& self) const
    {
        float maxPool = 1000.0f; // ClassDef defaults (500 hp + 500 shield)
        if (m_ctx && m_ctx->data)
            if (const ClassDef* cd = m_ctx->data->GetClass(bot.cls))
                maxPool = std::max(1.0f, cd->health + cd->shield);
        return std::clamp((self.health + self.shield) / maxPool, 0.0f, 1.0f);
    }

} // namespace Terrafront
