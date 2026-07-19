/**
 * @file TFServerSimMovement.cpp
 * @brief TFServerSim movement integration: per-player input replay through the
 *        shared TF movement model, seated-pawn ride sync, collision resolve,
 *        pawn Transform write-back and lag-comp snapshots (same class, split
 *        per repo file-size rules — see TFServerSim.cpp).
 */
#include "Net/TFServerSim.h"
#include "Net/TFServerSimConstants.h"

#include "Net/TFNetProtocol.h" // TF_ClientInput + TFB_* button bits
#include "Net/TFRepProtocol.h" // QuantAim::WrapPi
#include "Data/TFDataTables.h"
#include "World/TFWorldSetup.h"
#include "Game/TFAbilitySystem.h" // class-abilities lane (W9): per-pawn move modifiers + jet thrust
#include "Game/TFComponents.h"
#include "Game/TFMovementModel.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFServerValidation.h" // W13 anti-cheat lane: movement/fire-origin sanity (see file header)
#include "Game/TFVehicleSystem.h"    // TF-W3: seat routing + seated-pawn ride sync

#include "Engine/ECS/Components.h"
#include "Engine/Networking/LagCompensation.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Terrafront
{

    void TFServerSim::TickMovement(float fdt)
    {
        for (auto& [player, ms] : m_move)
        {
            // TF-W3 (vehicles agent): seated pawns don't walk. Their inputs drive
            // the VEHICLE (driver throttle/steer; gunners aim/fire via the weapon
            // path) and the pawn rides at the seat position — SyncSeatedPawn pulls
            // that pose (or the one-shot exit placement) into MoveState so the
            // Transform truth, replication, lag comp and TF_MoveState all stay
            // coherent without touching the walking model.
            if (m_ctx->vehicles && m_ctx->vehicles->IsSeated(player))
            {
                if (auto vit = m_inputs.find(player); vit != m_inputs.end())
                {
                    auto& vq = vit->second;
                    int consumed = 0;
                    while (!vq.empty() && consumed < kMaxInputsPerTick)
                    {
                        const TF_ClientInput in = vq.front();
                        vq.pop_front();
                        m_ctx->vehicles->ServerHandleSeatedInput(player, in, fdt);
                        ms.lastSeq = in.seq;
                        // Sibling of StepPlayer's guard (a5f95e7d): reject non-finite
                        // view angles before WrapPi/clamp here too -- WrapPi's
                        // while-loop never terminates on +-Inf (subtracting a finite
                        // step from infinity stays infinite), and a NaN survives
                        // clamp unchanged, poisoning ms.yaw/ms.pitch for every future
                        // tick and every TF_MoveState broadcast to all connected
                        // clients. The seated/vehicle-occupant input path took client
                        // view angles straight into WrapPi/clamp with no check.
                        if (std::isfinite(in.viewYaw) && std::isfinite(in.viewPitch))
                        {
                            ms.yaw = QuantAim::WrapPi(in.viewYaw);
                            ms.pitch = std::clamp(in.viewPitch, -kPitchLimitRad, kPitchLimitRad);
                        }
                        else if (m_serverTime - m_lastViolationLog > 5.0)
                        {
                            m_lastViolationLog = m_serverTime;
                            SPARK_LOG_WARN(Spark::LogCategory::Game,
                                           "[TF] movement validation: rejected non-finite view angle (seated, seq=%u)",
                                           in.seq);
                        }
                        ++consumed;
                    }
                }
                m_ctx->vehicles->SyncSeatedPawn(player, ms.pos, ms.vel);
                ms.grounded = true;
                WritePawnTransform(ms);
                continue;
            }

            // W13 anti-cheat lane: snapshot the pre-tick position so the whole
            // tick's NET displacement (across every replayed input below) can
            // be validated as one unit — see TFServerValidation.h file header
            // for why per-call checks inside StepPlayer would miss the
            // multi-input-per-tick catch-up exploit.
            const float prevPos[3] = {ms.pos[0], ms.pos[1], ms.pos[2]};

            auto qit = m_inputs.find(player);
            int applied = 0;
            if (qit != m_inputs.end())
            {
                auto& q = qit->second;
                while (!q.empty() && applied < kMaxInputsPerTick)
                {
                    const TF_ClientInput in = q.front();
                    q.pop_front();
                    StepPlayer(ms, &in, fdt);
                    ms.lastSeq = in.seq;
                    ++applied;
                }
            }
            if (applied == 0)
                StepPlayer(ms, nullptr, fdt); // input starvation: friction + gravity only

            // W13 anti-cheat lane: detect + clamp a tick displacement beyond
            // what this pawn's class/ability state could legitimately produce.
            // maxHorizSpeed mirrors StepPlayer's own per-call cap (class
            // sprintSpeed * ability speedMult) so a legitimate single-input
            // tick never trips this.
            {
                float sprintSpeed = kDefaultSprintSpeed;
                if (const ClassDef* cd = m_ctx->data ? m_ctx->data->GetClass(ms.cls) : nullptr)
                    sprintSpeed = cd->sprintSpeed;
                float speedMult = 1.0f;
                if (m_ctx->abilities)
                    speedMult = m_ctx->abilities->MoveModsForPawn(ms.pawn).speedMult;
                TFServerValidation::Get().ValidateMovementTick(player, prevPos, ms.pos, sprintSpeed * speedMult, fdt,
                                                               m_serverTime);
            }

            WritePawnTransform(ms);
        }
    }

    void TFServerSim::StepPlayer(MoveState& ms, const TF_ClientInput* in, float dt)
    {
        // --- class speed caps (data-driven) ---
        float runSpeed = kDefaultRunSpeed;
        float sprintSpeed = kDefaultSprintSpeed;
        if (m_ctx->data)
        {
            if (const ClassDef* cd = m_ctx->data->GetClass(ms.cls))
            {
                runSpeed = cd->runSpeed;
                sprintSpeed = cd->sprintSpeed;
            }
        }

        // --- decode + validate input ---
        float mx = 0.0f, my = 0.0f;
        uint16_t buttons = 0;
        if (in)
        {
            mx = static_cast<float>(in->moveX) / 127.0f;
            my = static_cast<float>(in->moveY) / 127.0f;
            const float mag = std::sqrt(mx * mx + my * my);
            if (mag > 1.0f)
            {
                // client asked for more than a unit wish vector: clamp, log, keep playing
                mx /= mag;
                my /= mag;
                ++m_speedClamps;
                if (m_serverTime - m_lastViolationLog > 5.0)
                {
                    m_lastViolationLog = m_serverTime;
                    SPARK_LOG_WARN(Spark::LogCategory::Game,
                                   "[TF] movement validation: clamped over-unit move input (%u total)", m_speedClamps);
                }
            }
            // Reject non-finite view angles before they reach WrapPi/clamp: WrapPi's
            // while-loop never terminates on +-Inf (subtracting a finite step from
            // infinity stays infinite), and a NaN survives clamp unchanged, poisoning
            // ms.yaw/ms.pitch for every future tick and every TF_MoveState broadcast
            // to all connected clients.
            if (std::isfinite(in->viewYaw) && std::isfinite(in->viewPitch))
            {
                ms.yaw = QuantAim::WrapPi(in->viewYaw);
                ms.pitch = std::clamp(in->viewPitch, -kPitchLimitRad, kPitchLimitRad);
            }
            else if (m_serverTime - m_lastViolationLog > 5.0)
            {
                m_lastViolationLog = m_serverTime;
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "[TF] movement validation: rejected non-finite view angle (seq=%u)", in->seq);
            }
            buttons = in->buttons;
        }

        // --- shared TF movement model v1 (Game/TFMovementModel.h) ---
        // MUST stay byte-identical in behavior to the client's ClientPrediction
        // simulator (TFClientNet::SimulateMove); both call the same TFMoveStep.
        TFMoveState mstate;
        mstate.pos[0] = ms.pos[0];
        mstate.pos[1] = ms.pos[1];
        mstate.pos[2] = ms.pos[2];
        mstate.vel[0] = ms.vel[0];
        mstate.vel[1] = ms.vel[1];
        mstate.vel[2] = ms.vel[2];
        mstate.grounded = ms.grounded;

        TFMoveInput minput;
        minput.moveX = mx;
        minput.moveY = my;
        minput.yaw = ms.yaw;
        minput.jump = (buttons & TFB_Jump) != 0;
        minput.sprint = (buttons & TFB_Sprint) != 0 && my > 0.0f;
        minput.crouch = (buttons & TFB_Crouch) != 0;

        // class-abilities lane (W9): per-pawn ability movement modifiers
        // (Striker jets / Bulwark Field slow / Colossus lockdown root).
        // MUST stay the exact mirror of TFClientNet::SimulateMove's snippet
        // (prediction symmetry — both-or-neither).
        TFAbilityMoveMods abilityMods;
        if (m_ctx->abilities)
            abilityMods = m_ctx->abilities->MoveModsForPawn(ms.pawn);
        if (abilityMods.speedMult <= 0.0f)
            minput.jump = false; // rooted: no jump either

        TFMoveStep(mstate, minput, runSpeed * abilityMods.speedMult, sprintSpeed * abilityMods.speedMult, dt,
                   [this](float x, float z) { return m_ctx->world ? m_ctx->world->TerrainHeightAt(x, z) : 0.0f; });

        // 2026-07-10 collision wave: block/slide against the static scene
        // bodies + terrain re-clamp at the resolved column. MUST stay the
        // exact mirror of TFClientNet::SimulateMove (same shared resolver,
        // TFWorldSetup::ResolveMoveCollision) or prediction rubber-bands.
        if (m_ctx->world)
        {
            const float prevPos[3] = {ms.pos[0], ms.pos[1], ms.pos[2]};
            m_ctx->world->ResolveMoveCollision(prevPos, mstate.pos, mstate.vel, &mstate.grounded);
        }

        // class-abilities lane (W9): jet thrust after step+collision resolve —
        // same order as the client mirror (see Game/TFAbilitySystem.h).
        if (abilityMods.jetThrust)
            TFApplyJetThrust(mstate, dt);

        ms.pos[0] = mstate.pos[0];
        ms.pos[1] = mstate.pos[1];
        ms.pos[2] = mstate.pos[2];
        ms.vel[0] = mstate.vel[0];
        ms.vel[1] = mstate.vel[1];
        ms.vel[2] = mstate.vel[2];
        ms.grounded = mstate.grounded;

        // --- hard speed cap (server-side validation backstop) ---
        const float hSpeed = std::sqrt(ms.vel[0] * ms.vel[0] + ms.vel[2] * ms.vel[2]);
        const float hardCap = sprintSpeed * kSpeedTolerance;
        if (hSpeed > hardCap && hSpeed > 0.0f)
        {
            const float scale = hardCap / hSpeed;
            ms.vel[0] *= scale;
            ms.vel[2] *= scale;
            ++m_speedClamps;
        }

        // --- world bounds [0, 4096] ---
        ms.pos[0] = std::clamp(ms.pos[0], kWorldMin, kWorldMax);
        ms.pos[2] = std::clamp(ms.pos[2], kWorldMin, kWorldMax);
    }

    void TFServerSim::WritePawnTransform(const MoveState& ms)
    {
        // The pawn entity's Transform is the replicated truth.
        // Convention: Transform.position == FEET position, rotation.y == yaw (degrees).
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        if (!world)
            return;
        const auto e = static_cast<EntityID>(ms.pawn);
        if (!world->GetRegistry().valid(e))
            return;
        if (Transform* t = world->GetComponent<Transform>(e))
        {
            t->position = {ms.pos[0], ms.pos[1], ms.pos[2]};
            t->rotation.y = ms.yaw * kRadToDeg;
        }
        if (TFPawnMoveComp* mv = world->GetComponent<TFPawnMoveComp>(e))
        {
            mv->vel[0] = ms.vel[0];
            mv->vel[1] = ms.vel[1];
            mv->vel[2] = ms.vel[2];
            mv->yaw = ms.yaw;
            mv->pitch = ms.pitch;
            mv->grounded = ms.grounded;
        }
    }

    void TFServerSim::RecordLagCompSnapshot()
    {
        if (!m_ctx->players)
            return;

        std::vector<Spark::Net::RewindPose> poses;
        poses.reserve(m_move.size());
        m_ctx->players->ForEachAlivePawn(
            [&poses](const auto& p)
            {
                Spark::Net::RewindPose pose{};
                pose.entityId = p.entity;
                pose.pos[0] = p.pos[0];
                pose.pos[1] = p.pos[1]; // feet; capsule spans [y, y + height]
                pose.pos[2] = p.pos[2];
                pose.radius = 0.4f;
                pose.height = 1.8f;
                poses.push_back(pose);
            });
        m_lagComp.RecordSnapshot(m_serverTime, poses);
    }

} // namespace Terrafront
