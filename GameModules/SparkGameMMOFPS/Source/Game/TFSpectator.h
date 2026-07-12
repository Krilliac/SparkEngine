/**
 * @file TFSpectator.h
 * @brief Dead-time spectator camera (W12 spectator-mode lane): squadmate
 *        follow-cam with LMB/RMB cycling, plus a bounded free-fly fallback
 *        when no squadmate is alive.
 *
 * OWNERSHIP: spectator-mode lane (this header + TFSpectator.cpp), plus the
 * killcam lane (W13: killcam trigger in UI/TFDeathRecap.h/.cpp, reserved
 * TFMsg block 0x5484-0x5487 — left unused, see the KILLCAM_FOLLOW mode note
 * below). Purely client-side — NO TFMsg block, NO server changes. Everything
 * it consumes is already replicated: squadmate (and, for killcam, killer)
 * pawns arrive through the TFReplication RemotePawn store (broadcast to ALL
 * clients with no interest culling — see TFReplication::ServerBroadcastUpdates),
 * surfaced via the frozen TFPlayerSystem::GetPawnByPlayer API and the
 * interpolated local visual entities TFClientNet writes each frame.
 * SetDeathRecapSource wires the two lanes' instances directly (Main.cpp,
 * one line — neither system lives on TFGameContext).
 *
 * Camera-ownership handoff (verified against TFClientNet):
 *  - ALIVE: TFClientNet::DriveFirstPersonCamera owns the module camera
 *    (TFWorldSetup::GetCamera) and PumpInput owns its angles.
 *  - DEAD: DriveFirstPersonCamera early-outs and PumpInput is suppressed
 *    (aliveLocalPawn == false), so NOBODY drives the camera — this system
 *    takes it for exactly that window (alive->dead edge activates, any
 *    alive edge deactivates) and never fights the first-person path.
 *
 * Modes:
 *  - FOLLOW: smooth third-person chase of an alive squadmate's replicated
 *    pawn (kTFSpectFollowBackM back / kTFSpectFollowUpM up along the pawn's
 *    yaw), terrain-clamped via TFWorldSetup::TerrainHeightAt and pulled in
 *    front of walls with a PhysicsSystem::RaycastFiltered pullback when a
 *    live Jolt world exists (GetJoltSystem() probe — the stub returns dummy
 *    bodies, so the probe is mandatory). LMB = next squadmate, RMB = prev.
 *  - FREE: fly camera from the death position when there is no alive
 *    squadmate (or no squad): W/S along the view direction, A/D strafe,
 *    hold RMB to look, kTFSpectFreeSpeedMps, bounded to kTFSpectFreeRangeM
 *    of the death point and to terrain + kTFSpectFreeTerrainClearM.
 *    Automatically returns to FOLLOW when a squadmate (re)spawns.
 *  - KILLCAM_FOLLOW (killcam lane, W13): on a fresh death, a brief
 *    (kTFKillcamDurationSec) forced follow-cam on the KILLER before FOLLOW/
 *    FREE ever get to pick a squadmate. No new wire message and no server
 *    replay buffer (reserved TFMsg block 0x5484-0x5487 stays unused —
 *    documented in Net/TFNetProtocol.h): the killer id comes from
 *    TF_DeathRecap.killerPlayer (already sent to the victim, see
 *    UI/TFDeathRecap.h's SetKillcamNotify push hook) and the live pose is
 *    the SAME globally-broadcast PawnInfo the FOLLOW mode already reads via
 *    TargetPose — DriveFollow is reused verbatim with m_target pointed at
 *    the killer instead of a squadmate. A short grace window
 *    (kTFKillcamGraceSec) absorbs the recap's network delivery lag relative
 *    to the alive->dead edge (the authority's own in-process mirror usually
 *    beats it; a pure client's reliable TF_DeathRecap usually lands inside
 *    the window). Ends on kTFKillcamDurationSec, ESC (TFTutorial skip-key
 *    precedent), or the killer's PawnInfo going unresolvable for good (dead/
 *    disconnected/never arrived) — the last two cases freeze on the last
 *    resolved pose (DriveFollow's existing "target vanished" behavior) and
 *    fade a dim vignette in via m_killerGoneFade. Hands off to whatever
 *    FOLLOW/FREE would have picked on a normal death.
 *
 * Input routing (verified against the death screen):
 *  - The world renders before ImGui, so spectating naturally sits BEHIND the
 *    death overlay / recap / DEPLOY-MAP buttons.
 *  - Cycle clicks are read in RenderUI via ImGui::IsMouseClicked gated on
 *    !WantCaptureMouse, so clicking DEPLOY SCREEN / MAP / recap widgets never
 *    also cycles the target; WantCaptureMouse is latched there for Update's
 *    free-cam RMB-look gate (Update runs outside the ImGui frame).
 *  - Keyboard: only W/A/S/D are read (dead == PumpInput inert, so they are
 *    free); SPACE (HUD deploy), M (map) and chat keys are untouched, and all
 *    spectator input pauses behind the same fullscreen-UI gate set as
 *    TFPingUI::FullscreenUiOpen (map / spawn / login / chat / social).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <cstddef>
#include <vector>

class SparkEngineCamera;

namespace Terrafront
{

    class TFDeathRecap;

    // --- follow-cam tuning ------------------------------------------------------
    constexpr float kTFSpectFollowBackM = 4.0f;   ///< chase distance behind the pawn
    constexpr float kTFSpectFollowUpM = 1.8f;     ///< chase height above the pawn's feet
    constexpr float kTFSpectTerrainClearM = 0.3f; ///< min camera clearance above terrain (follow)
    constexpr float kTFSpectWallSkinM = 0.25f;    ///< stop this short of a blocking wall
    constexpr float kTFSpectMinPullbackM = 0.5f;  ///< never pull the camera closer than this to the eye
    constexpr float kTFSpectPosLerpRate = 8.0f;   ///< exp smoothing rate for the chase position

    // --- free-cam tuning ---------------------------------------------------------
    constexpr float kTFSpectFreeSpeedMps = 12.0f;     ///< fly speed
    constexpr float kTFSpectFreeRangeM = 300.0f;      ///< max distance from the death point
    constexpr float kTFSpectFreeTerrainClearM = 0.5f; ///< min clearance above terrain (free)

    // --- killcam tuning (killcam lane, W13) --------------------------------------
    constexpr float kTFKillcamDurationSec = 4.0f;     ///< forced killer follow-cam hold
    constexpr float kTFKillcamGraceSec = 0.5f;        ///< wait this long for the killer id before giving up
    constexpr float kTFKillcamFadeRatePerSec = 1.0f;  ///< dim-vignette ramp once the killer pose is unresolvable

    class TFSpectator
    {
      public:
        enum class Mode : uint8_t
        {
            Follow = 0,
            Free,
            KillcamFollow, ///< forced follow on the killer (killcam lane, W13)
        };

        TFSpectator();
        ~TFSpectator();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderUI(); ///< 'Spectating: <name>' label + controls hint + cycle clicks
        void RenderDebugUI();

        bool IsActive() const { return m_active; }
        Mode ActiveMode() const { return m_mode; }
        PlayerId Target() const { return m_target; }

        /// Killcam lane wiring: called once from Main.cpp right after both
        /// lanes are constructed (TFClientNet::SetChatUI self-wiring
        /// precedent — neither system lives on TFGameContext, so this is a
        /// direct pointer hookup instead of a ctx.* publish). Registers
        /// OnRecapKiller as TFDeathRecap's push notify.
        void SetDeathRecapSource(TFDeathRecap& recap);

      private:
        // lifecycle
        void Activate(SparkEngineCamera& cam);
        void Deactivate();

        // per-frame helpers
        bool LocalPawnAlive() const;
        bool FullscreenUiOpen() const; ///< TFPingUI::FullscreenUiOpen gate set
        void BuildTargets(std::vector<PlayerId>& out) const;
        void CycleTarget(int step, const std::vector<PlayerId>& targets);
        void DriveFollow(SparkEngineCamera& cam, float dt);
        void DriveFree(SparkEngineCamera& cam, float dt, bool inputBlocked);

        /// Best available pose of a target pawn (squadmate in FOLLOW, killer
        /// in KILLCAM_FOLLOW — GetPawnByPlayer is not squad-filtered, so
        /// either works): the interpolated local visual entity Transform
        /// when resolvable (smooth), else the raw replicated PawnInfo
        /// sample. Returns false when the pawn is gone.
        bool TargetPose(PlayerId player, float outPos[3], float& outYawRad) const;

        /// Absolute camera aim via incremental Yaw/Pitch (those apply
        /// rotationSpeed * mouseSensitivity — compensated from Console_GetState
        /// so the aim lands exactly; Console_SetRotation would log per call).
        static void AimCameraAt(SparkEngineCamera& cam, const float from[3], const float at[3]);

        void ResolveTargetName(PlayerId player, char* out, size_t outSize) const;

        /// Killcam lane push handler (registered via SetDeathRecapSource):
        /// stashes the killer id from the latest TF_DeathRecap; Update()
        /// consumes it on the next pass (kInvalidPlayer == environment kill).
        void OnRecapKiller(PlayerId killer);

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        bool m_active{false};
        bool m_wasAlive{false}; ///< alive->dead edge = activate (never before first spawn)
        Mode m_mode{Mode::Follow};
        PlayerId m_target{kInvalidPlayer};

        // camera state
        float m_deathPos[3]{}; ///< camera position snapshotted at the death edge
        float m_camPos[3]{};   ///< smoothed chase position (follow mode)
        bool m_haveCamPos{false};
        float m_freePos[3]{}; ///< free-cam position
        bool m_freeInit{false};
        bool m_lastPoseValid{true}; ///< set by DriveFollow: false when TargetPose failed this frame

        // killcam lane state (W13)
        TFDeathRecap* m_deathRecapSrc{nullptr};
        PlayerId m_pendingKiller{kInvalidPlayer};
        bool m_pendingKillerFresh{false};   ///< a NEW recap arrived; Update() hasn't consumed it yet
        bool m_killcamShownThisLife{false}; ///< the per-life killcam decision has already been made
        float m_killcamGraceTimer{0.0f};    ///< counts down while waiting for the killer id
        float m_killcamTimer{0.0f};         ///< counts down while KillcamFollow is active
        float m_killerGoneFade{0.0f};       ///< 0..1 dim-vignette ramp (killer pose unresolvable)

        // RenderUI -> Update handshake (ImGui state is only valid in OnImGui)
        bool m_uiWantsMouse{false};
        int m_pendingCycle{0}; ///< +1 = next, -1 = prev (clicks latched in RenderUI)

        // debug
        uint32_t m_cycles{0};
    };

} // namespace Terrafront
