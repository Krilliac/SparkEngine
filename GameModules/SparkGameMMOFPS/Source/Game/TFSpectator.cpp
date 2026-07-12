/**
 * @file TFSpectator.cpp
 * @brief Dead-time spectator camera implementation (W12 spectator-mode lane).
 *        See TFSpectator.h for the camera-ownership handoff and input-routing
 *        contract. Client-only: no wire messages, no server state.
 */
#include "Game/TFSpectator.h"

#include "Game/TFPlayerSystem.h"
#include "Game/TFSocialSystem.h"
#include "Game/TFSquadSystem.h"
#include "Game/TFWeaponMath.h"
#include "Net/TFClientNet.h" // kTFLocalHostPlayer (HOST label fallback)
#include "UI/TFChatWindow.h"
#include "UI/TFDeathRecap.h" // killcam lane: SetKillcamNotify push hook
#include "UI/TFHUD.h"
#include "UI/TFKeybinds.h" // killcam lane: kVkEscape skip key (TFTutorial precedent)
#include "UI/TFLoginFlow.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSocialPanel.h"
#include "UI/TFSpawnScreen.h"
#include "World/TFWorldSetup.h"

#include "Camera/SparkEngineCamera.h"
#include "Engine/ECS/Components.h"
#include "Input/InputManager.h"
#include "Physics/PhysicsSystem.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace Terrafront
{

    namespace
    {

        constexpr float kRadToDeg = 57.2957795f;
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

    TFSpectator::TFSpectator() = default;

    TFSpectator::~TFSpectator()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFSpectator::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFSpectator initialized (client-only dead-time camera)");
        return true;
    }

    void TFSpectator::Shutdown()
    {
        Deactivate();
        // Killcam lane: uninstall our registration before `this` goes away
        // (the TFDeathRecap::SetDeathRecapMirror(nullptr) precedent).
        if (m_deathRecapSrc)
            m_deathRecapSrc->SetKillcamNotify(nullptr);
        m_deathRecapSrc = nullptr;
        m_wasAlive = false;
        m_initialized = false;
    }

    void TFSpectator::FixedUpdate(float) {}

    void TFSpectator::SetDeathRecapSource(TFDeathRecap& recap)
    {
        m_deathRecapSrc = &recap;
        recap.SetKillcamNotify([this](PlayerId killer) { OnRecapKiller(killer); });
    }

    void TFSpectator::OnRecapKiller(PlayerId killer)
    {
        m_pendingKiller = killer;
        m_pendingKillerFresh = true;
    }

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------

    void TFSpectator::Activate(SparkEngineCamera& cam)
    {
        m_active = true;
        m_mode = Mode::Follow;
        m_target = kInvalidPlayer;
        m_haveCamPos = false;
        m_freeInit = false;
        m_pendingCycle = 0;
        m_lastPoseValid = true;

        // Killcam lane: fresh death — arm the grace window and drop any
        // leftover fade/decision state from a previous life's killcam. Do
        // NOT clear m_pendingKiller/m_pendingKillerFresh here: the
        // authority's in-process recap mirror can fire before this Activate()
        // call in the same frame, and that value must survive to be consumed
        // by Update() right after.
        m_killcamShownThisLife = false;
        m_killcamGraceTimer = kTFKillcamGraceSec;
        m_killcamTimer = 0.0f;
        m_killerGoneFade = 0.0f;

        // Death anchor: the camera still holds the last first-person eye pose
        // (nothing drives it once DriveFirstPersonCamera early-outs on death).
        const DirectX::XMFLOAT3 p = cam.GetPosition();
        m_deathPos[0] = p.x;
        m_deathPos[1] = p.y;
        m_deathPos[2] = p.z;
    }

    void TFSpectator::Deactivate()
    {
        m_active = false;
        m_pendingCycle = 0;
    }

    // ---------------------------------------------------------------------------
    // Gates / queries
    // ---------------------------------------------------------------------------

    bool TFSpectator::LocalPawnAlive() const
    {
        PawnInfo pi{};
        return m_ctx->players && m_ctx->localPlayer != kInvalidPlayer &&
               m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pi) && pi.alive;
    }

    bool TFSpectator::FullscreenUiOpen() const
    {
        // Same input-suppression gate set as TFPingUI::FullscreenUiOpen /
        // TFClientNet's uiOpen (map click or chat line must not drive the cam).
        return (m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()) ||
               (m_ctx->loginFlow && m_ctx->loginFlow->IsOpen()) || (m_ctx->hud && m_ctx->hud->IsChatOpen()) ||
               (m_ctx->chatWindow && m_ctx->chatWindow->IsOpen()) ||
               (m_ctx->socialPanel && m_ctx->socialPanel->IsOpen());
    }

    void TFSpectator::BuildTargets(std::vector<PlayerId>& out) const
    {
        out.clear();
        if (!m_ctx->squads || !m_ctx->players)
            return;
        const TFSquadSystem::LocalSquadView view = m_ctx->squads->GetLocalSquadView();
        if (view.squad == kInvalidSquad)
            return;
        for (const PlayerId member : view.members)
        {
            if (member == m_ctx->localPlayer)
                continue;
            PawnInfo pi{};
            if (m_ctx->players->GetPawnByPlayer(member, pi) && pi.alive)
                out.push_back(member);
        }
    }

    void TFSpectator::CycleTarget(int step, const std::vector<PlayerId>& targets)
    {
        if (targets.empty())
            return;
        const auto n = static_cast<int>(targets.size());
        int idx = 0;
        const auto it = std::find(targets.begin(), targets.end(), m_target);
        if (it != targets.end())
            idx = static_cast<int>(it - targets.begin());
        idx = ((idx + step) % n + n) % n;
        if (targets[static_cast<size_t>(idx)] != m_target)
        {
            m_target = targets[static_cast<size_t>(idx)];
            m_haveCamPos = false; // snap to the new mate instead of a cross-map glide
            ++m_cycles;
        }
    }

    bool TFSpectator::TargetPose(PlayerId player, float outPos[3], float& outYawRad) const
    {
        if (!m_ctx->players)
            return false;
        PawnInfo pi{};
        if (!m_ctx->players->GetPawnByPlayer(player, pi) || !pi.alive)
            return false;

        outPos[0] = pi.pos[0];
        outPos[1] = pi.pos[1];
        outPos[2] = pi.pos[2];
        outYawRad = pi.yaw;

        // Prefer the interpolated local visual entity (TFClientNet writes the
        // 100 ms-delayed smoothed pose there each frame; on authority roles the
        // Transform is the TFServerSim per-tick truth). Raw PawnInfo replication
        // samples arrive at 20 Hz and would make the chase cam stutter.
        uint32_t localEnt = 0;
        if (m_ctx->players->ResolveEntity(pi.entity, localEnt) && m_ctx->engine)
        {
            if (World* world = m_ctx->engine->GetWorld())
            {
                const auto e = static_cast<EntityID>(localEnt);
                if (world->GetRegistry().valid(e))
                {
                    if (const Transform* t = world->GetComponent<Transform>(e))
                    {
                        outPos[0] = t->position.x;
                        outPos[1] = t->position.y;
                        outPos[2] = t->position.z;
                        outYawRad = t->rotation.y / kRadToDeg; // Transform stores degrees
                    }
                }
            }
        }
        return true;
    }

    // ---------------------------------------------------------------------------
    // Frame update
    // ---------------------------------------------------------------------------

    void TFSpectator::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;
        SparkEngineCamera* cam = m_ctx->world ? m_ctx->world->GetCamera() : nullptr;
        if (!cam)
            return; // headless: no camera to drive

        if (!m_ctx->InWorld() || m_ctx->localPlayer == kInvalidPlayer)
        {
            m_wasAlive = false;
            Deactivate();
            return;
        }

        if (LocalPawnAlive())
        {
            // TFClientNet::DriveFirstPersonCamera owns the camera again.
            Deactivate();
            m_wasAlive = true;
            return;
        }

        // Dead. Activate only on the alive->dead edge so the pre-first-spawn
        // dead state (fresh login, faction select) never grabs the camera.
        if (m_wasAlive)
            Activate(*cam);
        m_wasAlive = false;
        if (!m_active)
            return;

        // ---------------------------------------------------------------
        // Killcam gate (killcam lane, W13): decide once per life whether
        // this death has a killer to force-follow, before FOLLOW/FREE ever
        // pick a squadmate target. Holds the frozen death-frame camera (no
        // SetPosition call below) during the short grace window.
        // ---------------------------------------------------------------
        if (!m_killcamShownThisLife)
        {
            if (m_pendingKillerFresh)
            {
                m_pendingKillerFresh = false;
                m_killcamShownThisLife = true;
                if (m_pendingKiller != kInvalidPlayer)
                {
                    m_mode = Mode::KillcamFollow;
                    m_target = m_pendingKiller;
                    m_killcamTimer = kTFKillcamDurationSec;
                    m_haveCamPos = false; // snap to the killer instead of gliding
                }
                // else: environment/unknown kill — fall through to normal
                // FOLLOW/FREE selection below, same frame.
            }
            else
            {
                m_killcamGraceTimer -= deltaTime;
                if (m_killcamGraceTimer > 0.0f)
                    return; // still waiting on TF_DeathRecap; hold the frozen view
                m_killcamShownThisLife = true; // recap lost/late — proceed normally
            }
        }

        if (m_mode == Mode::KillcamFollow)
        {
            InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
            const bool skip = input && !FullscreenUiOpen() && input->IsKeyDown(TFKeys::kVkEscape);
            m_killcamTimer -= deltaTime;
            if (skip || m_killcamTimer <= 0.0f)
            {
                // Hand off to whatever FOLLOW/FREE would have picked on a
                // normal death (selection logic runs below, same frame).
                m_mode = Mode::Follow;
                m_target = kInvalidPlayer;
                m_haveCamPos = false;
            }
            else
            {
                if (!m_lastPoseValid)
                    m_killerGoneFade =
                        std::min(1.0f, m_killerGoneFade + kTFKillcamFadeRatePerSec * std::max(deltaTime, 0.0f));
                DriveFollow(*cam, deltaTime); // reused verbatim: m_target is the killer here
                return;
            }
        }

        std::vector<PlayerId> targets;
        BuildTargets(targets);

        if (targets.empty())
        {
            if (m_mode != Mode::Free)
            {
                m_mode = Mode::Free; // no squad / all dead: free-fly fallback
                m_freeInit = false;
            }
        }
        else
        {
            if (m_mode != Mode::Follow)
            {
                m_mode = Mode::Follow; // a squadmate (re)spawned: back to follow
                m_target = kInvalidPlayer;
                m_haveCamPos = false;
            }
            if (std::find(targets.begin(), targets.end(), m_target) == targets.end())
            {
                m_target = targets.front(); // first target / current one died
                m_haveCamPos = false;
            }
            if (m_pendingCycle != 0)
                CycleTarget(m_pendingCycle, targets);
        }
        m_pendingCycle = 0;

        if (m_mode == Mode::Follow)
            DriveFollow(*cam, deltaTime);
        else
            DriveFree(*cam, deltaTime, FullscreenUiOpen());
    }

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

    // ---------------------------------------------------------------------------
    // UI
    // ---------------------------------------------------------------------------

    void TFSpectator::ResolveTargetName(PlayerId player, char* out, size_t outSize) const
    {
        std::string roster;
        if (m_ctx && m_ctx->social && m_ctx->social->NameOfPlayer(player, roster) && !roster.empty())
        {
            std::snprintf(out, outSize, "%s", roster.c_str());
            return;
        }
        // Scoreboard-parity fallback (bots, pre-roster joiners).
        if (player == kTFLocalHostPlayer)
            std::snprintf(out, outSize, "HOST");
        else
            std::snprintf(out, outSize, "P%u", player);
    }

#ifdef SPARK_HAS_IMGUI

    void TFSpectator::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer() || !m_ctx->InWorld())
            return;

        // Latch for Update's free-cam RMB-look gate (Update runs outside the
        // ImGui frame, so io state must be sampled here).
        ImGuiIO& io = ImGui::GetIO();
        m_uiWantsMouse = io.WantCaptureMouse;

        if (!m_active)
            return;

        // Cycle clicks — read here (not in Update) so a click on the DEPLOY
        // SCREEN / MAP buttons or the death-recap panel never also cycles.
        // (KillcamFollow has no cycling — LMB/RMB stay inert during it.)
        if (m_mode == Mode::Follow && !io.WantCaptureMouse && !FullscreenUiOpen())
        {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left, false))
                m_pendingCycle = 1;
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right, false))
                m_pendingCycle = -1;
        }

        // Killcam lane: dim vignette once the killer's pose has gone
        // unresolvable (dead/disconnected) — a background-layer fade so it
        // never blocks the DEPLOY/MAP buttons or the recap panel underneath.
        if (m_mode == Mode::KillcamFollow && m_killerGoneFade > 0.0f)
        {
            ImGuiViewport* fvp = ImGui::GetMainViewport();
            ImGui::GetBackgroundDrawList()->AddRectFilled(
                fvp->Pos, ImVec2(fvp->Pos.x + fvp->Size.x, fvp->Pos.y + fvp->Size.y),
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.0f, m_killerGoneFade * 0.35f)));
        }

        // Hide the label under the fullscreen screens (DrawDeathActions /
        // TFDeathRecap precedent) — the map and deploy UIs own the display.
        if ((m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()))
            return;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.84f), ImGuiCond_Always,
                                ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.42f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoNav;
        if (!ImGui::Begin("##tf_spectator", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        if (m_mode == Mode::Follow)
        {
            char name[48];
            ResolveTargetName(m_target, name, sizeof(name));
            ImGui::TextColored(ImVec4(0.43f, 0.92f, 0.55f, 0.95f), "SPECTATING  %s", name);
            PawnInfo pi{};
            if (m_ctx->players && m_ctx->players->GetPawnByPlayer(m_target, pi) && pi.alive)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.88f, 0.90f), "  HP %.0f  SH %.0f", pi.health, pi.shield);
            }
            ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.78f, 0.75f), "LMB next squadmate   RMB previous");
        }
        else if (m_mode == Mode::KillcamFollow)
        {
            char name[48];
            ResolveTargetName(m_target, name, sizeof(name));
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 0.95f), "KILLED BY  %s", name);
            if (!m_lastPoseValid)
                ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.68f, 0.85f), "(signal lost)");
            ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.78f, 0.75f), "ESC to skip   %.0fs",
                               std::max(m_killcamTimer, 0.0f));
        }
        else
        {
            ImGui::TextColored(ImVec4(0.65f, 0.80f, 0.95f, 0.95f), "FREE CAMERA  (no squadmate alive)");
            ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.78f, 0.75f),
                               "W/S fly, A/D strafe, hold RMB to look   (%.0f m tether)",
                               static_cast<double>(kTFSpectFreeRangeM));
        }

        ImGui::End();
    }

    void TFSpectator::RenderDebugUI()
    {
        if (!ImGui::CollapsingHeader("TF Spectator"))
            return;
        const char* modeName =
            m_mode == Mode::Follow ? "follow" : (m_mode == Mode::KillcamFollow ? "killcam" : "free");
        ImGui::Text("active:%d  mode:%s  target:%u  cycles:%u", m_active ? 1 : 0, modeName, m_target, m_cycles);
        ImGui::Text("death:(%.1f %.1f %.1f)  cam:(%.1f %.1f %.1f)  free:(%.1f %.1f %.1f)", m_deathPos[0], m_deathPos[1],
                    m_deathPos[2], m_camPos[0], m_camPos[1], m_camPos[2], m_freePos[0], m_freePos[1], m_freePos[2]);
        ImGui::Text("killcam: shown:%d grace:%.2f timer:%.2f fade:%.2f pendingKiller:%u pendingFresh:%d",
                    m_killcamShownThisLife ? 1 : 0, m_killcamGraceTimer, m_killcamTimer, m_killerGoneFade,
                    m_pendingKiller, m_pendingKillerFresh ? 1 : 0);
    }

#else // !SPARK_HAS_IMGUI — headless: no label, no cycle clicks (no ImGui io)

    void TFSpectator::RenderUI() {}
    void TFSpectator::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
