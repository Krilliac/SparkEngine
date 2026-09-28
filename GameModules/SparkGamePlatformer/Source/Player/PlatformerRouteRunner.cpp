/**
 * @file PlatformerRouteRunner.cpp
 * @brief Level 0 route-following automated player (see PlatformerRouteRunner.h).
 */

#include "PlatformerRouteRunner.h"

#include "Level/PlatformerLevelSystem.h"
#include "PlatformerPlayerController.h"

#include <algorithm>

namespace Platformer
{
    namespace
    {
        // Level 0 colliders are authored in route order: start, two stepping stones, moving platform, bouncy
        // pad, high platform, two falling platforms, two descent ledges, goal platform.
        constexpr size_t kMovingPlatform = 3;
        constexpr size_t kGoalPlatform = 10;
        constexpr float kHalfWidth = 0.4f; // Player AABB half width (PlatformerPlayerCollision.cpp)
        constexpr float kGravity = 30.0f;  // PlatformerPlayerController gravity magnitude
        constexpr float kRunSpeed = 12.0f; // Move speed 8 x run multiplier 1.5
    } // namespace

    bool PlatformerRouteRunner::Drive(const PlatformerLevelSystem& level, PlatformerPlayerController& player)
    {
        const auto& colliders = level.GetActiveColliders();
        if (level.GetCurrentLevelIndex() != kRouteLevel || colliders.size() <= kGoalPlatform)
            return false;

        const PlayerPosition position = player.GetPlayerPosition();

        // A checkpoint restart teleports the player: resume the route at the platform beneath them.
        if (std::abs(position.x - m_lastX) > 3.0f || std::abs(position.y - m_lastY) > 3.0f)
            m_routeIndex = RouteIndexBelow(colliders, position);
        m_lastX = position.x;
        m_lastY = position.y;

        // Route progress and footing are judged from position, not IsGrounded(): a bouncy pad never leaves
        // the player grounded, and an updraft briefly lifts a player riding the moving platform.
        const float verticalSpeed = player.GetPlayerVelocity().y;
        const size_t touching = RouteIndexUnderFeet(colliders, position);
        if (touching != kNone)
            m_routeIndex = touching;
        const size_t standingOn = std::abs(verticalSpeed) < 1.0f ? touching : kNone;

        float move = 0.0f;
        bool jump = false;
        const size_t next = std::min(m_routeIndex + 1, kGoalPlatform);
        const PlatformCollider& target = colliders[next];
        const float targetCentre = (target.minX + target.maxX) * 0.5f;

        if (standingOn != kNone)
        {
            const PlatformCollider& ground = colliders[standingOn];
            const float groundCentre = (ground.minX + ground.maxX) * 0.5f;
            if (std::isfinite(loiterX) && position.x > ground.minX - kHalfWidth &&
                position.x < ground.maxX + kHalfWidth && loiterX > ground.minX && loiterX < ground.maxX)
            {
                move = Toward(position.x, loiterX);
            }
            else if (standingOn == kGoalPlatform)
            {
                move = Toward(position.x, groundCentre);
            }
            else if (next == kMovingPlatform && target.minX > ground.maxX + 1.5f)
            {
                // Wait near the edge until the moving platform comes back within jumping range.
                move = Toward(position.x, ground.maxX - 1.0f);
            }
            else if (standingOn == kMovingPlatform && groundCentre < 38.5f)
            {
                // Ride the moving platform to the far end of its track.
                move = Toward(position.x, groundCentre);
            }
            else
            {
                move = 1.0f;
                jump = position.x >= ground.maxX - 0.6f;
            }
        }
        else
        {
            // Airborne: stay clear of the target's underside and side until above its top, and pick the
            // horizontal speed that arrives over the aim point when the fall reaches the target's top.
            float aimX = targetCentre;
            if (position.y < target.maxY + 0.05f)
                aimX = std::min(aimX, target.minX - kHalfWidth - 0.3f);
            const float discriminant = verticalSpeed * verticalSpeed + 2.0f * kGravity * (position.y - target.maxY);
            const float timeToTop = discriminant > 0.0f ? (verticalSpeed + std::sqrt(discriminant)) / kGravity : 0.25f;
            move = std::clamp((aimX - position.x) / std::max(timeToTop, 0.15f) / kRunSpeed, -1.0f, 1.0f);
            jump = verticalSpeed > 0.0f && m_jumpHeld;
        }

        // Release the button for one frame after a jump so the next press registers as a new jump.
        if (jump && m_jumpHeld && standingOn != kNone)
            jump = false;
        m_jumpHeld = jump;
        player.SetMovementInput(move, true);
        player.SetJumpInput(jump);
        return true;
    }

    float PlatformerRouteRunner::Toward(float from, float to)
    {
        const float delta = to - from;
        if (std::abs(delta) < 0.1f)
            return 0.0f;
        return std::clamp(delta, -1.0f, 1.0f);
    }

    size_t PlatformerRouteRunner::RouteIndexUnderFeet(const std::vector<PlatformCollider>& colliders,
                                                      const PlayerPosition& p)
    {
        for (size_t i = 0; i <= kGoalPlatform; ++i)
        {
            const PlatformCollider& c = colliders[i];
            if (p.x + kHalfWidth > c.minX && p.x - kHalfWidth < c.maxX && std::abs(p.y - c.maxY) < 0.1f)
                return i;
        }
        return kNone;
    }

    size_t PlatformerRouteRunner::RouteIndexBelow(const std::vector<PlatformCollider>& colliders,
                                                  const PlayerPosition& p)
    {
        for (size_t i = 0; i <= kGoalPlatform; ++i)
        {
            const PlatformCollider& c = colliders[i];
            if (p.x + kHalfWidth > c.minX && p.x - kHalfWidth < c.maxX && p.y >= c.maxY - 0.1f)
                return i > 0 ? i - 1 : 0;
        }
        return 0;
    }
} // namespace Platformer
