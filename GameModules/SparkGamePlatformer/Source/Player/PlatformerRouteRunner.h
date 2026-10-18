/**
 * @file PlatformerRouteRunner.h
 * @brief Automated player for the level 0 ("Green Hills") route, driven through the controller's input API.
 *
 * The runner reads only what a human player sees -- player position and velocity and the loaded level's
 * platform boxes -- and writes only PlatformerPlayerController::SetMovementInput / SetJumpInput. It runs right
 * along the level 0 route, jumps at platform edges, steers in the air towards the next platform without
 * entering it from below, and waits for / rides the moving platform. Its route constants are authored for
 * level 0, so Drive() leaves the controller untouched on every other level.
 *
 * Contract: game thread only (call once per fixed step, before PlatformerLevelFlow::StepFixed). Owned by the
 * module (`platformer_autoplay`) or a test; holds no references between calls. No allocation.
 */

#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace Platformer
{
    class PlatformerLevelSystem;
    class PlatformerPlayerController;
    struct PlatformCollider;
    struct PlayerPosition;

    /// @brief Scripted level 0 player that drives the controller's input API from observable state.
    class PlatformerRouteRunner
    {
      public:
        /// Level the route constants are authored for.
        static constexpr unsigned kRouteLevel = 0;

        /// While finite, stand still at this x on the platform that contains it instead of advancing.
        float loiterX = std::nanf("");

        /**
         * @brief Set this fixed step's movement and jump input.
         * @return false (controller untouched) when level 0 is not the loaded level.
         */
        bool Drive(const PlatformerLevelSystem& level, PlatformerPlayerController& player);

      private:
        static constexpr size_t kNone = static_cast<size_t>(-1);

        static float Toward(float from, float to);
        static size_t RouteIndexUnderFeet(const std::vector<PlatformCollider>& colliders, const PlayerPosition& p);
        static size_t RouteIndexBelow(const std::vector<PlatformCollider>& colliders, const PlayerPosition& p);

        size_t m_routeIndex = 0;
        float m_lastX = 0.0f;
        float m_lastY = 0.0f;
        bool m_jumpHeld = false;
    };
} // namespace Platformer
