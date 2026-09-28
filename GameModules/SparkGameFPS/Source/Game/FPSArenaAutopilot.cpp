/**
 * @file FPSArenaAutopilot.cpp
 * @brief Scoreboard-derived phases and input-path steering for the arena loop driver.
 */

#include "FPSArenaAutopilot.h"

#include "GameMechanics.h"
#include "GameMode.h"

#include "Camera/SparkEngineCamera.h"
#include "Input/InputManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace SparkFPS
{
    namespace
    {
        float HorizontalDistance(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
        {
            const float dx = b.x - a.x;
            const float dz = b.z - a.z;
            return std::sqrt(dx * dx + dz * dz);
        }

        /// Wrap an angle difference into [-pi, pi].
        float WrapRadians(float angle)
        {
            return std::remainder(angle, DirectX::XM_2PI);
        }

        FPSArenaAutopilot::Phase PhaseFor(const ArenaLoopTally& tally)
        {
            using Phase = FPSArenaAutopilot::Phase;
            if (tally.kills >= 2 && tally.deaths >= 1 && tally.respawns >= 1)
                return Phase::Complete;
            // A recorded death the respawn system has not published yet: the
            // player is dead, so there is nothing to drive.
            if (tally.respawns < tally.deaths)
                return Phase::Yield;
            // The first kill is in; stand still so the arena can land the death
            // the loop needs before hunting the second kill.
            if (tally.kills >= 1 && tally.deaths == 0)
                return Phase::Yield;
            return Phase::Hunt;
        }
    } // namespace

    void FPSArenaAutopilot::Enable(const DirectX::XMFLOAT3& playerPosition)
    {
        m_phase = Phase::Hunt;
        m_lifeStart = playerPosition;
        m_maxDisplacement = 0.0f;
        m_seenRespawns.reset();
    }

    void FPSArenaAutopilot::Disable(InputManager& input)
    {
        SetForwardHeld(input, false);
        m_phase = Phase::Off;
    }

    ArenaLoopTally FPSArenaAutopilot::ReadTally(const Spark::GameMode& mode, const Spark::RespawnSystem& respawn)
    {
        ArenaLoopTally tally;
        if (const Spark::PlayerScore* score = mode.GetPlayerScore(kPlayerName))
        {
            tally.kills = score->kills;
            tally.deaths = score->deaths;
            tally.score = score->totalScore;
        }
        // RespawnSystem scores each death it arms and clears the pending one only
        // after it published PlayerRespawnEvent, the event that revives the player.
        const int armedDeaths = respawn.GetPlayerScore().deaths;
        tally.respawns = std::max(0, armedDeaths - (respawn.IsWaitingForRespawn() ? 1 : 0));
        return tally;
    }

    FPSArenaAutopilot::Phase FPSArenaAutopilot::UpdatePhase(const Spark::GameMode& mode,
                                                            const Spark::RespawnSystem& respawn)
    {
        if (m_phase != Phase::Off)
            m_phase = PhaseFor(ReadTally(mode, respawn));
        return m_phase;
    }

    bool FPSArenaAutopilot::Step(const Spark::GameMode& mode, const Spark::RespawnSystem& respawn,
                                 SparkEngineCamera& camera, InputManager& input, const ArenaView& view, float dt)
    {
        if (m_phase == Phase::Off)
            return false;

        // A new respawn starts a new life: measure movement from where it began,
        // so the respawn teleport itself never counts as the player moving.
        const ArenaLoopTally tally = ReadTally(mode, respawn);
        if (m_seenRespawns && *m_seenRespawns != tally.respawns)
            m_lifeStart = view.playerPosition;
        m_seenRespawns = tally.respawns;
        if (view.playerAlive)
            m_maxDisplacement = std::max(m_maxDisplacement, HorizontalDistance(m_lifeStart, view.playerPosition));

        m_phase = PhaseFor(tally);
        if (m_phase != Phase::Hunt || !view.playerAlive || view.targets.empty())
        {
            SetForwardHeld(input, false);
            return false;
        }

        const DirectX::XMFLOAT3* nearest = nullptr;
        float nearestDistance = 0.0f;
        for (const DirectX::XMFLOAT3& target : view.targets)
        {
            const float distance = HorizontalDistance(view.playerPosition, target);
            if (!nearest || distance < nearestDistance)
            {
                nearest = &target;
                nearestDistance = distance;
            }
        }

        // Yaw is read back from the camera's forward vector, so the error stays
        // correct whatever pitch the camera has or however Yaw() wraps its angle.
        const DirectX::XMFLOAT3 forward = camera.GetForward();
        const float currentYaw = std::atan2(forward.x, forward.z);
        const float desiredYaw = std::atan2(nearest->x - view.playerPosition.x, nearest->z - view.playerPosition.z);
        const float error = WrapRadians(desiredYaw - currentYaw);
        const float maxTurn = kTurnRate * std::max(dt, 0.0f);
        const float turn = std::clamp(error, -maxTurn, maxTurn);

        // Yaw() scales its argument by the camera's rotation speed and mouse
        // sensitivity; divide them out so `turn` is the angle actually applied.
        const SparkEngineCamera::CameraState state = camera.Console_GetState();
        const float gain = state.rotationSpeed * state.mouseSensitivity;
        if (turn != 0.0f && gain > 0.0f)
            camera.Yaw(turn / gain);

        SetForwardHeld(input, nearestDistance > kHoldDistance);
        return std::abs(error - turn) <= kAimTolerance && nearestDistance <= kEngageRange;
    }

    std::string FPSArenaAutopilot::FormatLoopLine(const Spark::GameMode& mode,
                                                  const Spark::RespawnSystem& respawn) const
    {
        const ArenaLoopTally tally = ReadTally(mode, respawn);
        char line[160];
        std::snprintf(line, sizeof(line), "Loop: kills=%d deaths=%d respawns=%d score=%d moved=%.1f autopilot=%s",
                      tally.kills, tally.deaths, tally.respawns, tally.score, m_maxDisplacement, PhaseName(m_phase));
        return line;
    }

    const char* FPSArenaAutopilot::PhaseName(Phase phase)
    {
        switch (phase)
        {
        case Phase::Off:
            return "off";
        case Phase::Hunt:
            return "hunt";
        case Phase::Yield:
            return "yield";
        case Phase::Complete:
            return "complete";
        }
        return "unknown";
    }

    void FPSArenaAutopilot::SetForwardHeld(InputManager& input, bool held)
    {
        if (held == m_forwardHeld)
            return;
        // The same message a window delivers for a physical key, so movement
        // runs through Player::UpdateMovement exactly as it does for a player.
        input.HandleMessage(held ? WM_KEYDOWN : WM_KEYUP, static_cast<WPARAM>('W'), 0);
        m_forwardHeld = held;
    }
} // namespace SparkFPS
