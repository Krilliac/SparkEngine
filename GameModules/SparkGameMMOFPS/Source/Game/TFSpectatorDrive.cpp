/**
 * @file TFSpectatorDrive.cpp
 * @brief TFSpectator camera drive: the FOLLOW-mode third-person chase (wall
 *        pullback + terrain clamp + exp smoothing), the absolute AimCameraAt
 *        yaw/pitch compensation, and the bounded FREE fly-cam. Split from
 *        TFSpectator.cpp; lifecycle, target selection and the per-frame
 *        Update gate stay there, and the ImGui overlay lives in
 *        TFSpectatorUi.cpp.
 */
#include "Game/TFSpectator.h"

#include "Game/TFWeaponMath.h"
#include "World/TFWorldSetup.h"

#include "Camera/SparkEngineCamera.h"
#include "Core/Platform.h"
#include "Input/InputManager.h"
#include "Physics/PhysicsSystem.h"
#include "Spark/IEngineContext.h"

#include <algorithm>
#include <cmath>

namespace Terrafront
{

    namespace
    {

        constexpr float kMouseSens = 0.005f; // radians per count (TFClientNet::PumpInput value)
        constexpr float kPi = 3.14159265358979f;

        /// Wrap an angle delta into [-pi, pi] (shortest turn).
        float WrapPi(float a)
        {
            while (a > kPi)
                a -= 2.0f * kPi;
            while (a < -kPi)
                a += 2.0f * kPi;
            return a;
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Follow cam
    // ---------------------------------------------------------------------------

    void TFSpectator::AimCameraAt(SparkEngineCamera& cam, const float from[3], const float at[3])
    {
        const float dx = at[0] - from[0];
        const float dy = at[1] - from[1];
        const float dz = at[2] - from[2];
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 1.0e-4f)
            return;

        // Camera basis (SparkEngineCamera::UpdateViewMatrix): forward =
        // (sin yaw, -sin pitch, cos yaw)-ish, so yaw = atan2(x, z) and
        // pitch = asin(-y) — the exact math Console_LookAt uses.
        const float desiredYaw = std::atan2(dx, dz);
        const float desiredPitch = std::asin(std::clamp(-dy / len, -1.0f, 1.0f));

        // Yaw()/Pitch() scale by rotationSpeed * mouseSensitivity (and invertY);
        // compensate so the aim lands exactly. Console_SetRotation would be
        // absolute but logs to the console every call — unusable per frame.
        const SparkEngineCamera::CameraState st = cam.Console_GetState();
        float scale = st.rotationSpeed * st.mouseSensitivity;
        if (scale < 1.0e-3f)
            scale = 1.0f;

        const DirectX::XMFLOAT3 rot = cam.GetRotation(); // (pitch, yaw, roll) radians
        const float dyaw = WrapPi(desiredYaw - rot.y);
        if (std::fabs(dyaw) > 1.0e-5f)
            cam.Yaw(dyaw / scale);

        float dpitch = std::clamp(desiredPitch, -1.55f, 1.55f) - rot.x;
        if (st.invertY)
            dpitch = -dpitch; // Pitch() re-inverts; pre-negate so it cancels
        if (std::fabs(dpitch) > 1.0e-5f)
            cam.Pitch(dpitch / scale);
    }

    void TFSpectator::DriveFollow(SparkEngineCamera& cam, float dt)
    {
        float tpos[3];
        float tyaw = 0.0f;
        if (!TargetPose(m_target, tpos, tyaw))
        {
            // Target vanished mid-frame. Squad-FOLLOW revalidates next frame
            // (Update picks a new target); KILLCAM_FOLLOW has nowhere else to
            // go, so it freezes on the last resolved pose (m_camPos is left
            // untouched below) and Update() ramps m_killerGoneFade off this.
            m_lastPoseValid = false;
            return;
        }
        m_lastPoseValid = true;

        const float fwdX = std::sin(tyaw);
        const float fwdZ = std::cos(tyaw);
        const float eye[3] = {tpos[0], tpos[1] + WeaponMath::kEyeHeightM, tpos[2]};
        float desired[3] = {tpos[0] - fwdX * kTFSpectFollowBackM, tpos[1] + kTFSpectFollowUpM,
                            tpos[2] - fwdZ * kTFSpectFollowBackM};

        // Wall pullback — only with a live Jolt world (GetJoltSystem probe: the
        // no-Jolt stub hands out valid dummies, so a null-check is not enough).
        ::PhysicsSystem* phys = m_ctx->engine ? m_ctx->engine->GetPhysics() : nullptr;
        if (phys && phys->GetJoltSystem())
        {
            const float rx = desired[0] - eye[0];
            const float ry = desired[1] - eye[1];
            const float rz = desired[2] - eye[2];
            const float rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
            if (rlen > 1.0e-3f)
            {
                const DirectX::XMFLOAT3 origin{eye[0], eye[1], eye[2]};
                const DirectX::XMFLOAT3 dir{rx / rlen, ry / rlen, rz / rlen};
                const RaycastHit hit = phys->RaycastFiltered(origin, dir, rlen, CollisionLayers::MovementMask);
                if (hit.hasHit)
                {
                    const float travel = std::max(kTFSpectMinPullbackM, hit.distance - kTFSpectWallSkinM);
                    desired[0] = eye[0] + dir.x * travel;
                    desired[1] = eye[1] + dir.y * travel;
                    desired[2] = eye[2] + dir.z * travel;
                }
            }
        }

        // Terrain clamp last (floor-of-last-resort, mirrors the move resolver).
        if (m_ctx->world)
        {
            desired[1] =
                std::max(desired[1], m_ctx->world->TerrainHeightAt(desired[0], desired[2]) + kTFSpectTerrainClearM);
        }

        if (!m_haveCamPos)
        {
            m_camPos[0] = desired[0];
            m_camPos[1] = desired[1];
            m_camPos[2] = desired[2];
            m_haveCamPos = true;
        }
        else
        {
            const float a = 1.0f - std::exp(-kTFSpectPosLerpRate * std::max(dt, 0.0f));
            for (int i = 0; i < 3; ++i)
                m_camPos[i] += (desired[i] - m_camPos[i]) * a;
        }

        cam.SetPosition({m_camPos[0], m_camPos[1], m_camPos[2]});
        AimCameraAt(cam, m_camPos, eye);
    }

    // ---------------------------------------------------------------------------
    // Free cam
    // ---------------------------------------------------------------------------

    void TFSpectator::DriveFree(SparkEngineCamera& cam, float dt, bool inputBlocked)
    {
        if (!m_freeInit)
        {
            m_freePos[0] = m_deathPos[0];
            m_freePos[1] = m_deathPos[1];
            m_freePos[2] = m_deathPos[2];
            if (m_ctx->world)
            {
                m_freePos[1] = std::max(m_freePos[1], m_ctx->world->TerrainHeightAt(m_freePos[0], m_freePos[2]) +
                                                          kTFSpectFreeTerrainClearM);
            }
            m_freeInit = true;
        }

        InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        if (input && !inputBlocked)
        {
            // Hold-RMB drag look (cursor stays free for the DEPLOY/MAP buttons).
            if (input->IsMouseButtonDown(1) && !m_uiWantsMouse)
            {
                const auto [mdx, mdy] = input->GetMouseDelta();
                if (mdx != 0)
                    cam.Yaw(static_cast<float>(mdx) * kMouseSens);
                if (mdy != 0)
                    cam.Pitch(static_cast<float>(-mdy) * kMouseSens);
            }

            // W/S along the view direction, A/D horizontal strafe. SPACE stays
            // the HUD's deploy key and M the map key — deliberately untouched.
            float mf = 0.0f, mr = 0.0f;
            if (input->IsKeyDown('W'))
                mf += 1.0f;
            if (input->IsKeyDown('S'))
                mf -= 1.0f;
            if (input->IsKeyDown('D'))
                mr += 1.0f;
            if (input->IsKeyDown('A'))
                mr -= 1.0f;
            if (mf != 0.0f || mr != 0.0f)
            {
                const DirectX::XMFLOAT3 f = cam.GetForward();
                float rX = f.z, rZ = -f.x; // horizontal right of the view
                const float rl = std::sqrt(rX * rX + rZ * rZ);
                if (rl > 1.0e-4f)
                {
                    rX /= rl;
                    rZ /= rl;
                }
                else
                {
                    rX = 1.0f;
                    rZ = 0.0f; // looking straight up/down: keep a stable strafe axis
                }
                const float vx = f.x * mf + rX * mr;
                const float vy = f.y * mf;
                const float vz = f.z * mf + rZ * mr;
                const float vl = std::sqrt(vx * vx + vy * vy + vz * vz);
                if (vl > 1.0e-4f)
                {
                    const float s = kTFSpectFreeSpeedMps * std::max(dt, 0.0f) / vl;
                    m_freePos[0] += vx * s;
                    m_freePos[1] += vy * s;
                    m_freePos[2] += vz * s;
                }
            }
        }

        // Tether to the death point (whole 3D offset), then terrain clamp.
        const float ox = m_freePos[0] - m_deathPos[0];
        const float oy = m_freePos[1] - m_deathPos[1];
        const float oz = m_freePos[2] - m_deathPos[2];
        const float olen = std::sqrt(ox * ox + oy * oy + oz * oz);
        if (olen > kTFSpectFreeRangeM)
        {
            const float k = kTFSpectFreeRangeM / olen;
            m_freePos[0] = m_deathPos[0] + ox * k;
            m_freePos[1] = m_deathPos[1] + oy * k;
            m_freePos[2] = m_deathPos[2] + oz * k;
        }
        if (m_ctx->world)
        {
            m_freePos[1] = std::max(m_freePos[1], m_ctx->world->TerrainHeightAt(m_freePos[0], m_freePos[2]) +
                                                      kTFSpectFreeTerrainClearM);
        }

        cam.SetPosition({m_freePos[0], m_freePos[1], m_freePos[2]});
    }

} // namespace Terrafront
