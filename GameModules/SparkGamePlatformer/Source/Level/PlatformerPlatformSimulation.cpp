/**
 * @file PlatformerPlatformSimulation.cpp
 * @brief PlatformerLevelSystem platform simulation and module-local AABB colliders
 *
 * Moving platforms ping-pong between their waypoints, Falling platforms
 * collapse after the player stands on them and later reset, Disappearing
 * platforms cycle between solid and hidden, and Rotating platforms spin about
 * the vertical axis (their collider is the bounds of the rotated footprint).
 * The player controller resolves against GetActiveColliders(); these are not
 * Jolt bodies.
 */

#include "PlatformerLevelSystem.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Platformer
{

    namespace
    {
        constexpr float kFallingPlatformGravity = -30.0f;
        constexpr float kFallingPlatformMaxSpeed = -20.0f;
        constexpr float kFallingPlatformResetSeconds = 3.0f; ///< Collapsed platforms return to their origin
    } // namespace

    void PlatformerLevelSystem::ResetPlatformRuntime()
    {
        m_platformRuntime.clear();
        if (m_currentLevel < m_levels.size())
        {
            for (const auto& def : m_levels[m_currentLevel].platforms)
            {
                PlatformRuntime runtime{};
                runtime.x = def.posX;
                runtime.y = def.posY;
                runtime.z = def.posZ;
                m_platformRuntime.push_back(runtime);
            }
        }

        RebuildColliders();
        for (auto& collider : m_colliders)
        {
            collider.deltaX = 0.0f;
            collider.deltaY = 0.0f;
            collider.deltaZ = 0.0f;
        }
    }

    void PlatformerLevelSystem::StepPlatforms(float fixedDeltaTime)
    {
        if (!m_initialized || !std::isfinite(fixedDeltaTime) || fixedDeltaTime <= 0.0f ||
            m_currentLevel >= m_levels.size())
        {
            return;
        }

        const auto& platforms = m_levels[m_currentLevel].platforms;
        const size_t count = std::min(platforms.size(), m_platformRuntime.size());
        for (size_t i = 0; i < count; ++i)
        {
            const PlatformDef& def = platforms[i];
            PlatformRuntime& runtime = m_platformRuntime[i];

            switch (def.type)
            {
            case PlatformType::Moving:
            {
                // Ping-pong along the start->end segment at a constant speed.
                const float segX = def.endX - def.posX;
                const float segY = def.endY - def.posY;
                const float segZ = def.endZ - def.posZ;
                const float length = std::sqrt(segX * segX + segY * segY + segZ * segZ);
                if (length <= 1e-4f || def.moveSpeed <= 0.0f)
                    break;

                runtime.travel += runtime.travelDirection * def.moveSpeed * fixedDeltaTime;
                if (runtime.travel >= length)
                {
                    runtime.travel = length - std::min(runtime.travel - length, length);
                    runtime.travelDirection = -1.0f;
                }
                else if (runtime.travel <= 0.0f)
                {
                    runtime.travel = std::min(-runtime.travel, length);
                    runtime.travelDirection = 1.0f;
                }

                const float t = runtime.travel / length;
                runtime.x = def.posX + segX * t;
                runtime.y = def.posY + segY * t;
                runtime.z = def.posZ + segZ * t;
                break;
            }
            case PlatformType::Falling:
            {
                if (!runtime.triggered)
                    break;

                runtime.collapseTimer += fixedDeltaTime;
                if (!runtime.collapsed)
                {
                    if (runtime.collapseTimer >= def.fallDelay)
                    {
                        runtime.collapsed = true;
                        runtime.collapseTimer = 0.0f;
                    }
                    break;
                }

                if (runtime.collapseTimer >= kFallingPlatformResetSeconds)
                {
                    runtime = PlatformRuntime{};
                    runtime.x = def.posX;
                    runtime.y = def.posY;
                    runtime.z = def.posZ;
                    break;
                }

                runtime.fallVelocity =
                    std::max(runtime.fallVelocity + kFallingPlatformGravity * fixedDeltaTime, kFallingPlatformMaxSpeed);
                runtime.y += runtime.fallVelocity * fixedDeltaTime;
                break;
            }
            case PlatformType::Disappearing:
            {
                const float cycle = std::max(def.visibleTime, 0.0f) + std::max(def.hiddenTime, 0.0f);
                if (cycle > 0.0f)
                    runtime.cycleTimer = std::fmod(runtime.cycleTimer + fixedDeltaTime, cycle);
                break;
            }
            case PlatformType::Rotating:
                runtime.angleDegrees = std::fmod(runtime.angleDegrees + def.rotationSpeed * fixedDeltaTime, 360.0f);
                break;
            default:
                break;
            }
        }

        RebuildColliders();
    }

    void PlatformerLevelSystem::RebuildColliders()
    {
        std::vector<PlatformCollider> previous = std::move(m_colliders);
        m_colliders.clear();
        if (m_currentLevel >= m_levels.size())
            return;

        const auto& platforms = m_levels[m_currentLevel].platforms;
        const size_t count = std::min(platforms.size(), m_platformRuntime.size());
        m_colliders.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            const PlatformDef& def = platforms[i];
            const PlatformRuntime& runtime = m_platformRuntime[i];

            float halfX = def.width * 0.5f;
            float halfZ = def.depth * 0.5f;
            if (def.type == PlatformType::Rotating)
            {
                // Bounds of the footprint rotated about the vertical axis.
                const float radians = runtime.angleDegrees * std::numbers::pi_v<float> / 180.0f;
                const float c = std::abs(std::cos(radians));
                const float sn = std::abs(std::sin(radians));
                halfX = c * def.width * 0.5f + sn * def.depth * 0.5f;
                halfZ = sn * def.width * 0.5f + c * def.depth * 0.5f;
            }

            PlatformCollider collider{};
            collider.type = def.type;
            collider.minX = runtime.x - halfX;
            collider.maxX = runtime.x + halfX;
            collider.maxY = runtime.y;
            collider.minY = runtime.y - def.height;
            collider.minZ = runtime.z - halfZ;
            collider.maxZ = runtime.z + halfZ;
            collider.solid = def.type != PlatformType::Disappearing || runtime.cycleTimer < def.visibleTime;
            if (def.type == PlatformType::Conveyor)
                collider.surfaceVelocityX = def.conveyorDirX * def.conveyorSpeed;
            if (def.type == PlatformType::Bouncy)
                collider.bounceForce = def.bounceForce;

            if (i < previous.size())
            {
                collider.deltaX = runtime.x - (previous[i].maxX + previous[i].minX) * 0.5f;
                collider.deltaY = collider.maxY - previous[i].maxY;
                collider.deltaZ = runtime.z - (previous[i].maxZ + previous[i].minZ) * 0.5f;
            }
            m_colliders.push_back(collider);
        }
    }

    float PlatformerLevelSystem::GetKillPlaneY() const
    {
        if (m_currentLevel < m_levels.size())
            return m_levels[m_currentLevel].killPlaneY;
        return -15.0f;
    }

    void PlatformerLevelSystem::NotifyPlatformStoodOn(size_t colliderIndex)
    {
        if (m_currentLevel >= m_levels.size() || colliderIndex >= m_platformRuntime.size())
            return;

        const auto& platforms = m_levels[m_currentLevel].platforms;
        if (colliderIndex < platforms.size() && platforms[colliderIndex].type == PlatformType::Falling)
            m_platformRuntime[colliderIndex].triggered = true;
    }

} // namespace Platformer
