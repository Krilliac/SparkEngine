/**
 * @file TFSpectator.cpp
 * @brief Dead-time spectator camera lifecycle (W12 spectator-mode lane):
 *        activation edges, killcam gating, target selection and the per-frame
 *        Update dispatch. See TFSpectator.h for the camera-ownership handoff
 *        and input-routing contract. Client-only: no wire messages, no server
 *        state. The FOLLOW/FREE camera drive lives in TFSpectatorDrive.cpp;
 *        the ImGui overlay lives in TFSpectatorUi.cpp.
 */
#include "Game/TFSpectator.h"

#include "Game/TFPlayerSystem.h"
#include "Game/TFSquadSystem.h"
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
#include "Core/Platform.h"
#include "Engine/ECS/Components.h"
#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <vector>

namespace Terrafront
{

    namespace
    {

        constexpr float kRadToDeg = 57.2957795f;

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
                    return;                    // still waiting on TF_DeathRecap; hold the frozen view
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

} // namespace Terrafront
