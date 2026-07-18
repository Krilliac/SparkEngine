/**
 * @file TFClientNetInput.cpp
 * @brief TFClientNet input pump + prediction: 60 Hz InputManager sampling,
 *        TFMoveStep ClientPrediction simulator, TF_MoveState reconciliation
 *        and the predicted-local-state getter (same class, split per repo
 *        file-size rules — connection observe/sends live in TFClientNet.cpp,
 *        the TFMsg handlers in TFClientNetHandlers.cpp, the loopback path in
 *        TFClientNetLoopback.cpp and presentation in TFClientNetView.cpp).
 */
#include "Net/TFClientNet.h"

#include "Data/TFDataTables.h"
#include "Game/TFAbilitySystem.h" // class-abilities lane (W9): move-mod mirror
#include "Game/TFMovementModel.h"
#include "Game/TFWeaponSystem.h"
#include "Net/TFReplication.h"
#include "World/TFWorldSetup.h"

#include "Camera/SparkEngineCamera.h"
#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"

#include <algorithm>

namespace Terrafront
{

    namespace
    {

        constexpr float kInputStepSec = 1.0f / 60.0f; // input send / predict cadence
        constexpr int kMaxStepsPerFrame = 4;          // catch-up bound after a hitch
        constexpr float kMouseSens = 0.005f;          // radians per count (FPS module value)

        // Windows virtual-key codes (numeric so no <windows.h> dependency here).
        constexpr int kVkShift = 0x10;
        constexpr int kVkControl = 0x11;
        constexpr int kVkSpace = 0x20;

    } // namespace

    // ---------------------------------------------------------------------------
    // Input pump + prediction
    // ---------------------------------------------------------------------------

    void TFClientNet::PumpInput(float dt, bool aliveLocalPawn)
    {
        if (!aliveLocalPawn || !IsConnected())
        {
            m_inputAccum = 0.0f;
            return;
        }

        InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        if (!input)
            return;

        // --- mouse look (camera owns the angles; we read them back) -----------
        // Module-owned camera: the engine context camera slot is empty in module
        // mode (see TFWorldSetup::GetCamera).
        SparkEngineCamera* cam = m_ctx->world ? m_ctx->world->GetCamera() : nullptr;
        if (cam)
        {
            const auto [dx, dy] = input->GetMouseDelta();
            if (dx != 0)
                cam->Yaw(static_cast<float>(dx) * kMouseSens);
            if (dy != 0)
                cam->Pitch(static_cast<float>(-dy) * kMouseSens);
            const auto rot = cam->GetRotation(); // (pitch, yaw, roll) radians
            m_viewPitch = rot.x;
            m_viewYaw = rot.y;
        }

        // --- buttons + axes ----------------------------------------------------
        uint16_t buttons = 0;
        if (input->IsMouseButtonDown(0))
        {
            buttons |= TFB_Fire;
            if (m_ctx->weapons)
                m_ctx->weapons->ClientTriggerFire(); // RoF-paced internally
        }
        if (input->IsMouseButtonDown(1))
            buttons |= TFB_AltFire;
        if (input->IsKeyDown(kVkSpace))
            buttons |= TFB_Jump;
        if (input->IsKeyDown(kVkControl))
            buttons |= TFB_Crouch;
        if (input->IsKeyDown(kVkShift))
            buttons |= TFB_Sprint;
        if (input->WasKeyPressed('R'))
            buttons |= TFB_Reload;
        if (input->IsKeyDown('E'))
            buttons |= TFB_Interact;
        if (input->IsKeyDown('F'))
            buttons |= TFB_Ability;

        float moveX = 0.0f, moveY = 0.0f;
        if (input->IsKeyDown('D'))
            moveX += 1.0f;
        if (input->IsKeyDown('A'))
            moveX -= 1.0f;
        if (input->IsKeyDown('W'))
            moveY += 1.0f;
        if (input->IsKeyDown('S'))
            moveY -= 1.0f;

        // --- fixed 60 Hz send/predict steps -------------------------------------
        m_inputAccum += dt;
        int steps = 0;
        while (m_inputAccum >= kInputStepSec && steps < kMaxStepsPerFrame)
        {
            m_inputAccum -= kInputStepSec;
            ++steps;
            SendOneInput(moveX, moveY, buttons);
            buttons = static_cast<uint16_t>(buttons & ~TFB_Reload); // one-shot per frame
        }
        if (steps == kMaxStepsPerFrame)
            m_inputAccum = 0.0f; // hitch: drop the backlog instead of bursting
    }

    void TFClientNet::SendOneInput(float moveX, float moveY, uint16_t buttons)
    {
        uint32_t seq = 0;

        if (!m_ctx->IsAuthority())
        {
            // Record + locally apply through ClientPrediction so the sequence the
            // server acks in TF_MoveState matches our pending-input buffer.
            Spark::PredictedInput pi{};
            pi.timestamp = static_cast<float>(m_clock);
            pi.moveDirection = {moveX, 0.0f, moveY};
            pi.lookYaw = m_viewYaw;
            pi.lookPitch = m_viewPitch;
            pi.jump = (buttons & TFB_Jump) != 0;
            pi.crouch = (buttons & TFB_Crouch) != 0;
            pi.sprint = (buttons & TFB_Sprint) != 0;
            pi.fire = (buttons & TFB_Fire) != 0;
            pi.reload = (buttons & TFB_Reload) != 0;
            pi.interact = (buttons & TFB_Interact) != 0;

            seq = m_prediction.RecordInput(pi);
            pi.sequenceNumber = seq;
            m_prediction.ApplyPrediction(m_predState, pi, kInputStepSec);
            m_predActive = true;
        }
        else
        {
            seq = ++m_inputSeq;
        }

        TF_ClientInput in{};
        in.seq = seq;
        in.buttons = buttons;
        in.moveX = static_cast<int8_t>(std::clamp(moveX, -1.0f, 1.0f) * 127.0f);
        in.moveY = static_cast<int8_t>(std::clamp(moveY, -1.0f, 1.0f) * 127.0f);
        in.viewYaw = m_viewYaw;
        in.viewPitch = m_viewPitch;
        in.weaponSlot = 0; // TF-W2: mirror TFWeaponSystem's active slot

        SendInput(in);
        ++m_inputsSent;
    }

    void TFClientNet::SimulateMove(Spark::PredictedState& s, const Spark::PredictedInput& in, float dt) const
    {
        TFMoveState ms;
        ms.pos[0] = s.position.x;
        ms.pos[1] = s.position.y;
        ms.pos[2] = s.position.z;
        ms.vel[0] = s.velocity.x;
        ms.vel[1] = s.velocity.y;
        ms.vel[2] = s.velocity.z;
        ms.grounded = s.isGrounded;

        TFMoveInput mi;
        mi.moveX = in.moveDirection.x;
        mi.moveY = in.moveDirection.z;
        mi.yaw = in.lookYaw;
        mi.jump = in.jump;
        mi.sprint = in.sprint;
        mi.crouch = in.crouch;

        // class-abilities lane (W9): the exact mirror of TFServerSim::
        // StepPlayer's ability move-mod snippet (both-or-neither).
        TFAbilityMoveMods abilityMods;
        if (m_ctx && m_ctx->abilities)
            abilityMods = m_ctx->abilities->MoveModsLocal();
        if (abilityMods.speedMult <= 0.0f)
            mi.jump = false; // rooted: no jump either

        const TFWorldSetup* world = m_ctx ? m_ctx->world : nullptr;
        TFMoveStep(ms, mi, m_runSpeed * abilityMods.speedMult, m_sprintSpeed * abilityMods.speedMult, dt,
                   [world](float x, float z) { return world ? world->TerrainHeightAt(x, z) : 0.0f; });

        // 2026-07-10 collision wave: identical post-step resolve as TFServerSim
        // (shared client/server code path — determinism contract).
        if (world)
        {
            const float prevPos[3] = {s.position.x, s.position.y, s.position.z};
            world->ResolveMoveCollision(prevPos, ms.pos, ms.vel, &ms.grounded);
        }

        // class-abilities lane (W9): jet thrust after step+collision resolve.
        if (abilityMods.jetThrust)
            TFApplyJetThrust(ms, dt);

        s.position = {ms.pos[0], ms.pos[1], ms.pos[2]};
        s.velocity = {ms.vel[0], ms.vel[1], ms.vel[2]};
        s.isGrounded = ms.grounded;
        s.yaw = in.lookYaw;
        s.pitch = in.lookPitch;
        s.isCrouching = in.crouch;
        s.isSprinting = in.sprint;
    }

    void TFClientNet::RefreshClassSpeeds(ClassId cls)
    {
        m_runSpeed = 5.2f;
        m_sprintSpeed = 7.2f;
        if (m_ctx && m_ctx->data && m_ctx->data->IsLoaded())
        {
            if (const ClassDef* cd = m_ctx->data->GetClass(cls))
            {
                m_runSpeed = cd->runSpeed;
                m_sprintSpeed = cd->sprintSpeed;
            }
        }
    }

    void TFClientNet::SeedPredictionAt(const float pos[3])
    {
        m_prediction = Spark::ClientPrediction{};
        m_prediction.SetMaxPendingInputs(128);
        m_prediction.SetMovementSimulator([this](Spark::PredictedState& s, const Spark::PredictedInput& in, float dt)
                                          { SimulateMove(s, in, dt); });

        m_predState = Spark::PredictedState{};
        m_predState.position = {pos[0], pos[1], pos[2]};
        m_predState.isGrounded = true;
        m_predActive = true;
    }

    void TFClientNet::ReconcileFromServer()
    {
        if (!m_ctx->replication || !m_predActive || !m_ctx->replication->HasFreshMoveState())
            return;

        TF_MoveState ms{};
        if (!m_ctx->replication->GetLatestMoveState(ms))
            return;

        Spark::PredictedState sv{};
        sv.lastProcessedInput = ms.lastAckedSeq;
        sv.position = {ms.posX, ms.posY, ms.posZ};
        sv.velocity = {ms.velX, ms.velY, ms.velZ};
        sv.yaw = ms.yaw;
        sv.pitch = ms.pitch;
        sv.isGrounded = ms.grounded != 0;

        m_prediction.Reconcile(sv, kInputStepSec);
        m_predState = m_prediction.GetState();
        ++m_reconciles;
    }

    bool TFClientNet::GetPredictedLocalState(float outPos[3], float outVel[3], float& outYaw, float& outPitch) const
    {
        if (!m_predActive || !m_ctx || m_ctx->IsAuthority())
            return false;
        outPos[0] = m_predState.position.x;
        outPos[1] = m_predState.position.y;
        outPos[2] = m_predState.position.z;
        outVel[0] = m_predState.velocity.x;
        outVel[1] = m_predState.velocity.y;
        outVel[2] = m_predState.velocity.z;
        outYaw = m_viewYaw;
        outPitch = m_viewPitch;
        return true;
    }

} // namespace Terrafront
