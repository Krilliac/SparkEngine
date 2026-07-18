/**
 * @file TFBotSystemVehicle.cpp
 * @brief TFBotSystem vehicle use: driver-seat approach/boarding through the
 *        real seat-op path, ground driving with terrain look-ahead + wedge
 *        recovery, and Vulture VTOL flight. Split from TFBotSystem.cpp; the
 *        shared tuning constants live in TFBotSystemInternal.h.
 */
#include "Game/TFBotSystem.h"

#include "Game/TFBotSystemInternal.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Net/TFNetProtocol.h"
#include "World/TFWorldSetup.h"

#include <algorithm>
#include <cmath>

namespace Terrafront
{

    using namespace BotDetail;

    // ---------------------------------------------------------------------------
    // Vehicles (driver seat 0 only; TFServerSim forwards a seated bot's
    // TF_ClientInput to TFVehicleSystem::ServerHandleSeatedInput)
    // ---------------------------------------------------------------------------

    bool TFBotSystem::TryUseVehicle(Bot& bot, const PawnInfo& self, double now, TF_ClientInput& in)
    {
        if (!m_ctx->vehicles)
            return false;

        // Only worth driving when the march is long.
        const float odx = bot.objectiveX - self.pos[0];
        const float odz = bot.objectiveZ - self.pos[2];
        if (odx * odx + odz * odz < kVehSeekBeyondM * kVehSeekBeyondM)
        {
            bot.vehicleEntity = 0;
            return false;
        }

        // Scan for a ride (rate-limited after failures).
        if (bot.vehicleEntity == 0)
        {
            if (now < bot.vehicleRetryAt)
                return false;
            EntityId best = 0;
            float bestD2 = kVehScanRadiusM * kVehScanRadiusM;
            m_ctx->vehicles->ForEachVehicle(
                [&](const TFVehicleInfo& v)
                {
                    if (v.faction != bot.faction || v.hp <= 0.0f || v.deployed)
                        return; // enemy, dead, or a parked Aegis spawn (leave it deployed)
                    if (v.seatCount == 0 || v.seats[0] != kInvalidPlayer)
                        return; // no driver seat to take
                    const float dx = v.pos[0] - self.pos[0];
                    const float dz = v.pos[2] - self.pos[2];
                    const float d2 = dx * dx + dz * dz;
                    if (d2 < bestD2)
                    {
                        bestD2 = d2;
                        best = v.entity;
                    }
                });
            if (best == 0)
            {
                bot.vehicleRetryAt = now + kVehRetrySec;
                return false;
            }
            bot.vehicleEntity = best;
            bot.enterTries = 0;
        }

        // Validate the plan every think (vehicle may die / get taken meanwhile).
        TFVehicleInfo veh;
        if (!m_ctx->vehicles->GetVehicleInfo(bot.vehicleEntity, veh) || veh.hp <= 0.0f || veh.deployed ||
            veh.seatCount == 0 || veh.seats[0] != kInvalidPlayer)
        {
            bot.vehicleEntity = 0;
            bot.vehicleRetryAt = now + kVehRetrySec;
            return false;
        }

        const float dx = veh.pos[0] - self.pos[0];
        const float dz = veh.pos[2] - self.pos[2];
        const float d2 = dx * dx + dz * dz;

        if (d2 <= kVehEnterTryM * kVehEnterTryM)
        {
            // At the hull: take the driver seat through the real seat-op path.
            TF_VehicleSeatOp op{};
            op.vehicleEntity = veh.entity;
            op.seatIndex = 0;
            m_ctx->vehicles->ServerHandleSeatOp(bot.id, op, true);
            if (m_ctx->vehicles->IsSeated(bot.id))
            {
                bot.state = BotState::Driving;
                bot.stuckRefPos[0] = self.pos[0];
                bot.stuckRefPos[1] = self.pos[1];
                bot.stuckRefPos[2] = self.pos[2];
                bot.stuckSince = now;
                bot.jumping = false;
                bot.reverseUntil = 0.0; // fresh ride: clear wedge-recovery state
                bot.wedgeCount = 0;
                bot.lastWedgeAt = 0.0;
                in = TF_ClientInput{}; // neutral until ThinkDriving steers next think
                return true;
            }
            if (++bot.enterTries >= kVehMaxEnterTries)
            {
                bot.vehicleEntity = 0;
                bot.vehicleRetryAt = now + kVehRetrySec;
                return false;
            }
        }

        // Walk to the hull; this plan owns the movement this think.
        bot.state = BotState::ToVehicle;
        in.viewYaw = std::atan2(dx, dz);
        in.viewPitch = 0.0f;
        in.moveY = 127;
        in.moveX = 0;
        if (d2 > kSprintBeyondM * kSprintBeyondM)
            in.buttons |= TFB_Sprint;
        return true;
    }

    void TFBotSystem::ThinkDriving(Bot& bot, const PawnInfo& self, double now)
    {
        TFVehicleInfo veh;
        const bool seated = m_ctx->vehicles && m_ctx->vehicles->IsSeated(bot.id) &&
                            m_ctx->vehicles->GetVehicleInfo(bot.vehicleEntity, veh);
        if (!seated || veh.hp <= 0.0f)
        {
            // Ejected / destroyed / despawned underneath us: back on foot.
            bot.state = BotState::Moving;
            bot.vehicleEntity = 0;
            bot.vehicleRetryAt = now + kVehRetrySec;
            bot.wantMove = false;
            return;
        }

        const bool vtol = veh.vehId == VehicleId::Vulture;

        // Keep the objective fresh while riding (ownership can flip mid-drive).
        // In chaos the scatter/pilot objective is kept: re-rolls happen on foot
        // only, and the pilots' far target must survive the whole ride.
        if (!m_chaosActive)
            PickObjective(bot, self.pos);

        const float dx = bot.objectiveX - self.pos[0];
        const float dz = bot.objectiveZ - self.pos[2];
        const float dist = std::sqrt(dx * dx + dz * dz);
        const float desiredYaw = std::atan2(dx, dz);
        const float yawErr = WrapPi(desiredYaw - veh.yaw);

        // Wedge detection: ride pose unchanged too long. VTOLs get more slack —
        // a gunship yaws in place at zero ground speed while lining up.
        bool wedged = false;
        const float dsx = self.pos[0] - bot.stuckRefPos[0];
        const float dsz = self.pos[2] - bot.stuckRefPos[2];
        if (dsx * dsx + dsz * dsz > kStuckEpsM * kStuckEpsM)
        {
            bot.stuckRefPos[0] = self.pos[0];
            bot.stuckRefPos[1] = self.pos[1];
            bot.stuckRefPos[2] = self.pos[2];
            bot.stuckSince = now;
        }
        else if (now - bot.stuckSince > (vtol ? kVulStuckSec : kVehStuckSec))
        {
            wedged = true;
        }

        TF_ClientInput in{};
        in.weaponSlot = 0;
        in.viewYaw = desiredYaw;
        in.viewPitch = 0.0f;

        if (vtol)
        {
            // --- Vulture flight: altitude hold + fly at the objective, then
            //     descend and take the landed-gated exit on arrival. A wedged
            //     hover (world edge / AGL ceiling) also lands and walks. ---
            const float terrain = m_ctx->world ? m_ctx->world->TerrainHeightAt(veh.pos[0], veh.pos[2]) : 0.0f;
            const float agl = veh.pos[1] - terrain;
            in.moveX = static_cast<int8_t>(std::clamp(yawErr * 1.2f, -1.0f, 1.0f) * 127.0f);

            if (dist < kVulLandWithinM || wedged)
            {
                in.buttons |= TFB_Crouch; // descend onto the point
                in.moveY = static_cast<int8_t>((dist > 20.0f && !wedged) ? 45 : 0);
                if (agl <= kVulExitAglM || wedged)
                {
                    // Server's VehicleLanded gate opens at 2 m AGL (clearance-
                    // aware with a Jolt hull, so pads/roofs count even though
                    // this terrain-AGL proxy can't see them). A refused exit
                    // (still airborne) re-latches into Driving next think and
                    // the descent simply continues.
                    ExitVehicle(bot, now);
                    return;
                }
            }
            else
            {
                if (agl < kVulCruiseMinAglM)
                    in.buttons |= TFB_Jump; // climb
                else if (agl > kVulCruiseMaxAglM)
                    in.buttons |= TFB_Crouch; // descend back into the cruise band
                in.moveY = static_cast<int8_t>(std::fabs(yawErr) < 0.9f ? 127 : 40);
            }

            bot.input = in;
            bot.wantMove = true;
            return;
        }

        // --- ground hover-rig ---

        if (dist < kVehExitWithinM)
        {
            ExitVehicle(bot, now);
            return;
        }

        if (wedged)
        {
            // First wedge on this leg: reverse out and try again. A second
            // wedge inside the window means the ride is truly stuck — walk.
            if (bot.wedgeCount > 0 && now - bot.lastWedgeAt < kVehWedgeWindowSec)
            {
                ExitVehicle(bot, now);
                return;
            }
            ++bot.wedgeCount;
            bot.lastWedgeAt = now;
            bot.reverseUntil = now + kVehReverseSec;
            bot.reverseSteer = (m_rng() & 1u) != 0u ? static_cast<int8_t>(90) : static_cast<int8_t>(-90);
            bot.stuckSince = now; // fresh window after the recovery attempt
        }

        // Reverse-out recovery phase: back away swinging the tail.
        if (now < bot.reverseUntil)
        {
            in.viewYaw = veh.yaw; // hold the current heading while backing out
            in.moveY = -100;
            in.moveX = bot.reverseSteer;
            bot.input = in;
            bot.wantMove = true;
            return;
        }

        // Drive: moveY = throttle, moveX = steer (ServerHandleSeatedInput mapping).
        float steer = std::clamp(yawErr * 1.5f, -1.0f, 1.0f);
        float throttle = std::fabs(yawErr) > kVehHardTurnRad ? 60.0f : 127.0f;

        // Terrain look-ahead: a wall-steep rise dead ahead means steer along
        // the lower side probe instead of nosing in and wedging (plateau walls
        // and mesa edges; the Jolt hull can't climb them either).
        if (m_ctx->world)
        {
            const float here = m_ctx->world->TerrainHeightAt(veh.pos[0], veh.pos[2]);
            const auto riseAt = [&](float relYaw)
            {
                const float a = veh.yaw + relYaw;
                return m_ctx->world->TerrainHeightAt(veh.pos[0] + std::sin(a) * kVehProbeAheadM,
                                                     veh.pos[2] + std::cos(a) * kVehProbeAheadM) -
                       here;
            };
            if (riseAt(0.0f) > kVehClimbLimitM)
            {
                const float left = riseAt(-kVehProbeSideRad);
                const float right = riseAt(kVehProbeSideRad);
                steer = left < right ? -1.0f : 1.0f;
                throttle = std::min(left, right) > kVehClimbLimitM ? 40.0f : 80.0f;
            }
        }

        in.moveX = static_cast<int8_t>(steer * 127.0f);
        in.moveY = static_cast<int8_t>(throttle);

        bot.input = in;
        bot.wantMove = true;
    }

    void TFBotSystem::ExitVehicle(Bot& bot, double now)
    {
        if (m_ctx->vehicles && m_ctx->vehicles->IsSeated(bot.id))
        {
            TF_VehicleSeatOp op{};
            op.vehicleEntity = bot.vehicleEntity; // ignored by the exit path (leave own seat)
            m_ctx->vehicles->ServerHandleSeatOp(bot.id, op, false);
        }
        bot.state = BotState::Moving;
        bot.vehicleEntity = 0;
        bot.vehicleRetryAt = now + kVehRetrySec;
        bot.wantMove = false; // fresh walking input next think
    }

} // namespace Terrafront
