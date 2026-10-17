/**
 * @file FPSArenaAutopilot.h
 * @brief Developer driver that plays the single-player arena loop
 *        (spawn -> move -> kill -> die -> respawn -> score) through the real game.
 *
 * The autopilot plays; it does not keep score. Kills, deaths and score are read
 * from GameMode's "Player1" scoreboard row and respawns from RespawnSystem (a
 * death it scored whose pending respawn was published), so a loop line can only
 * report what the production systems recorded. It moves the player by holding W
 * through InputManager::HandleMessage, the window-message path, so
 * Player::UpdateMovement runs unmodified, and it turns through
 * SparkEngineCamera::Yaw, the mouse-look path.
 *
 * Phases are derived from the scoreboard every step:
 *   Hunt      approach and shoot the nearest living enemy;
 *   Yield     stop moving and firing (after the first kill, until the arena's
 *             enemies land a death), and while a recorded death awaits respawn;
 *   Complete  kills >= 2, deaths >= 1 and respawns >= 1.
 *
 * Contract:
 * - Thread affinity: game thread only (it writes camera and input state).
 * - Ownership: a value member of Game; holds no engine pointers between calls.
 * - Allocation: none per step; FormatLoopLine builds one string per call.
 * - Scalability: one nearest-target scan over the caller's target span.
 * - Wiring: Game::UpdateArenaAutopilot steps it each frame after input, and the
 *   developer-only `fps_autoplay on|off` command (SparkFPS::ConsolePolicy) enables it.
 */

#pragma once

#include "Core/Platform.h"

#include <optional>
#include <span>
#include <string>

class InputManager;
class SparkEngineCamera;

namespace Spark
{
    class GameMode;
    class RespawnSystem;
} // namespace Spark

namespace SparkFPS
{
    /// @brief Loop counters as the production scoreboard and respawn system report them.
    struct ArenaLoopTally
    {
        int kills = 0;    ///< GameMode kills credited to Player1.
        int deaths = 0;   ///< GameMode deaths charged to Player1.
        int respawns = 0; ///< RespawnSystem deaths whose respawn was published.
        int score = 0;    ///< GameMode total score of Player1.
    };

    /// @brief What the autopilot may see of the arena this frame.
    struct ArenaView
    {
        DirectX::XMFLOAT3 playerPosition{};         ///< Player world position.
        bool playerAlive = false;                   ///< Player health above zero.
        std::span<const DirectX::XMFLOAT3> targets; ///< Positions of living enemies.
    };

    class FPSArenaAutopilot
    {
      public:
        enum class Phase
        {
            Off,
            Hunt,
            Yield,
            Complete
        };

        /// Scoreboard row the loop is scored against (Game registers the local player under it).
        static constexpr const char* kPlayerName = "Player1";
        /// Fire only at targets this close (metres, horizontal).
        static constexpr float kEngageRange = 25.0f;
        /// Stop closing in once this near (metres, horizontal).
        static constexpr float kHoldDistance = 8.0f;
        /// Fire only when the remaining yaw error is within this (radians).
        static constexpr float kAimTolerance = 0.1f;
        /// Maximum turn rate (radians per second).
        static constexpr float kTurnRate = 4.0f;

        /// @brief Start driving; @p playerPosition anchors the first life's movement.
        void Enable(const DirectX::XMFLOAT3& playerPosition);

        /// @brief Stop driving and release any key it holds on @p input.
        void Disable(InputManager& input);

        bool IsEnabled() const { return m_phase != Phase::Off; }
        Phase GetPhase() const { return m_phase; }

        /// @brief Furthest horizontal distance the player moved from a life's start (metres).
        float GetMaxDisplacement() const { return m_maxDisplacement; }

        /**
         * @brief Re-derive the phase from the production counters (no-op while Off).
         * @return The new phase.
         */
        Phase UpdatePhase(const Spark::GameMode& mode, const Spark::RespawnSystem& respawn);

        /**
         * @brief One game-thread step: update the phase and movement record, turn
         *        toward the nearest target and hold or release W.
         * @return true when the caller should fire the equipped weapon this frame.
         */
        bool Step(const Spark::GameMode& mode, const Spark::RespawnSystem& respawn, SparkEngineCamera& camera,
                  InputManager& input, const ArenaView& view, float dt);

        /// @brief Counters read from @p mode and @p respawn (never from the autopilot).
        static ArenaLoopTally ReadTally(const Spark::GameMode& mode, const Spark::RespawnSystem& respawn);

        /// @brief `Loop: kills=K deaths=D respawns=R score=S moved=M autopilot=<phase>` for game_status.
        std::string FormatLoopLine(const Spark::GameMode& mode, const Spark::RespawnSystem& respawn) const;

        static const char* PhaseName(Phase phase);

      private:
        void SetForwardHeld(InputManager& input, bool held);

        Phase m_phase = Phase::Off;
        DirectX::XMFLOAT3 m_lifeStart{};   ///< Where the current life began.
        float m_maxDisplacement = 0.0f;    ///< Best distance from any life's start.
        std::optional<int> m_seenRespawns; ///< Respawn count at the previous step.
        bool m_forwardHeld = false;        ///< Whether this autopilot holds W down.
    };
} // namespace SparkFPS
