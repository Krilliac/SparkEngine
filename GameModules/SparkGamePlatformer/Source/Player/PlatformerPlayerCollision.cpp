/**
 * @file PlatformerPlayerCollision.cpp
 * @brief Player collision against level platforms, the kill plane, and automatic restart
 *
 * Platformer collision is module-local: the player is an axis-aligned box that
 * is swept one axis at a time against PlatformerLevelSystem::GetActiveColliders().
 * It is not a Jolt body. Falling below the level's kill plane costs a life and
 * respawns the player at the last activated checkpoint; losing the last life
 * enters the Dead state, which FixedUpdate turns into a checkpoint restart. Platforms
 * that move into the player push them out along the axis of least penetration.
 */

#include "PlatformerPlayerController.h"
#include "Level/PlatformerLevelSystem.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>

namespace Platformer
{

    namespace
    {
        // Player collision box: position is the centre of the feet.
        constexpr float kPlayerHalfWidth = 0.4f;
        constexpr float kPlayerHeight = 1.8f;
        constexpr float kContactSkin = 0.01f;     ///< Tolerance for "was already outside this face"
        constexpr float kGroundProbe = 0.05f;     ///< Max gap between feet and a top surface to count as grounded
        constexpr float kPenetrationSlop = 1e-3f; ///< Overlap ignored by the push-out step

        bool OverlapsFootprint(const PlatformCollider& collider, float x, float z)
        {
            return x + kPlayerHalfWidth > collider.minX && x - kPlayerHalfWidth < collider.maxX &&
                   z + kPlayerHalfWidth > collider.minZ && z - kPlayerHalfWidth < collider.maxZ;
        }

        bool OverlapsHeight(const PlatformCollider& collider, float feetY)
        {
            return feetY < collider.maxY && feetY + kPlayerHeight > collider.minY;
        }
    } // namespace

    void PlatformerPlayerController::ApplyPlatformCarry(float fixedDeltaTime)
    {
        // Ride the platform stood on last step: moving/falling platforms carry the player with their
        // displacement and conveyor belts add their surface speed.
        if (!m_level || !m_grounded)
            return;

        const auto& colliders = m_level->GetActiveColliders();
        if (m_groundPlatform >= colliders.size())
            return;

        const PlatformCollider& platform = colliders[m_groundPlatform];
        m_position.x =
            SweepHorizontal(m_position.x, platform.deltaX + platform.surfaceVelocityX * fixedDeltaTime, true);
        m_position.y += platform.deltaY;
        m_position.z = SweepHorizontal(m_position.z, platform.deltaZ, false);
    }

    void PlatformerPlayerController::ResolvePlatformPenetration()
    {
        // The sweeps only stop a player who starts outside a face, so a platform that moves, rotates, or
        // reappears into the player would otherwise leave them inside it (walking through its sides and
        // falling through its top). Push the player out of each overlapping solid box along the axis of
        // least penetration before any movement is resolved this step.
        if (!m_level)
            return;

        for (const PlatformCollider& collider : m_level->GetActiveColliders())
        {
            if (!collider.solid || !OverlapsFootprint(collider, m_position.x, m_position.z) ||
                !OverlapsHeight(collider, m_position.y))
            {
                continue;
            }

            const float pushUp = collider.maxY - m_position.y;
            const float pushDown = m_position.y + kPlayerHeight - collider.minY;
            const float pushNegX = m_position.x + kPlayerHalfWidth - collider.minX;
            const float pushPosX = collider.maxX - (m_position.x - kPlayerHalfWidth);
            const float pushNegZ = m_position.z + kPlayerHalfWidth - collider.minZ;
            const float pushPosZ = collider.maxZ - (m_position.z - kPlayerHalfWidth);
            const float smallest = std::min({pushUp, pushDown, pushNegX, pushPosX, pushNegZ, pushPosZ});
            if (smallest <= kPenetrationSlop)
                continue; // Touching, or float drift from riding a platform: not a real overlap

            if (smallest == pushUp)
            {
                m_position.y = collider.maxY; // CheckGrounded then stands the player on the top
                m_velocity.y = std::max(m_velocity.y, 0.0f);
            }
            else if (smallest == pushDown)
            {
                m_position.y = collider.minY - kPlayerHeight;
                m_velocity.y = std::min(m_velocity.y, 0.0f);
            }
            else if (smallest == pushNegX)
            {
                m_position.x = collider.minX - kPlayerHalfWidth;
                m_velocity.x = std::min(m_velocity.x, 0.0f);
            }
            else if (smallest == pushPosX)
            {
                m_position.x = collider.maxX + kPlayerHalfWidth;
                m_velocity.x = std::max(m_velocity.x, 0.0f);
            }
            else if (smallest == pushNegZ)
            {
                m_position.z = collider.minZ - kPlayerHalfWidth;
                m_velocity.z = std::min(m_velocity.z, 0.0f);
            }
            else
            {
                m_position.z = collider.maxZ + kPlayerHalfWidth;
                m_velocity.z = std::max(m_velocity.z, 0.0f);
            }
        }
    }

    float PlatformerPlayerController::SweepHorizontal(float start, float delta, bool alongX)
    {
        float target = start + delta;
        if (!m_level || delta == 0.0f)
            return target;

        for (const PlatformCollider& collider : m_level->GetActiveColliders())
        {
            if (!collider.solid || !OverlapsHeight(collider, m_position.y))
                continue;

            // The perpendicular horizontal axis must overlap for the faces to meet.
            const float crossPos = alongX ? m_position.z : m_position.x;
            const float crossMin = alongX ? collider.minZ : collider.minX;
            const float crossMax = alongX ? collider.maxZ : collider.maxX;
            if (crossPos + kPlayerHalfWidth <= crossMin || crossPos - kPlayerHalfWidth >= crossMax)
                continue;

            const float faceMin = alongX ? collider.minX : collider.minZ;
            const float faceMax = alongX ? collider.maxX : collider.maxZ;
            if (delta > 0.0f && start + kPlayerHalfWidth <= faceMin + kContactSkin &&
                target + kPlayerHalfWidth > faceMin)
            {
                target = faceMin - kPlayerHalfWidth;
            }
            else if (delta < 0.0f && start - kPlayerHalfWidth >= faceMax - kContactSkin &&
                     target - kPlayerHalfWidth < faceMax)
            {
                target = faceMax + kPlayerHalfWidth;
            }
        }
        return target;
    }

    void PlatformerPlayerController::MoveAndCollide(float dx, float dy, float dz)
    {
        // Axis-separated swept AABB resolution against the level's platform boxes. Sweeps test the
        // face crossed during the step, so thin platforms cannot be tunnelled at any fall speed.
        const float resolvedX = SweepHorizontal(m_position.x, dx, true);
        const bool blockedX = resolvedX != m_position.x + dx;
        m_position.x = resolvedX;
        if (blockedX)
            m_velocity.x = 0.0f;
        m_touchingWall = blockedX && !m_grounded;

        const float resolvedZ = SweepHorizontal(m_position.z, dz, false);
        if (resolvedZ != m_position.z + dz)
            m_velocity.z = 0.0f;
        m_position.z = resolvedZ;

        const float startY = m_position.y;
        float targetY = startY + dy;
        size_t landedOn = NO_PLATFORM;
        if (m_level && dy != 0.0f)
        {
            const auto& colliders = m_level->GetActiveColliders();
            for (size_t i = 0; i < colliders.size(); ++i)
            {
                const PlatformCollider& collider = colliders[i];
                if (!collider.solid || !OverlapsFootprint(collider, m_position.x, m_position.z))
                    continue;

                if (dy < 0.0f && startY >= collider.maxY - kContactSkin && targetY <= collider.maxY)
                {
                    targetY = collider.maxY; // Highest top crossed wins
                    landedOn = i;
                }
                else if (dy > 0.0f && startY + kPlayerHeight <= collider.minY + kContactSkin &&
                         targetY + kPlayerHeight > collider.minY)
                {
                    targetY = collider.minY - kPlayerHeight; // Head bump on the underside
                    m_velocity.y = 0.0f;
                }
            }
        }

        m_position.y = targetY;
        if (landedOn != NO_PLATFORM)
            LandOn(landedOn, targetY);
        else if (dy != 0.0f)
        {
            m_grounded = false;
            m_groundPlatform = NO_PLATFORM;
        }
    }

    void PlatformerPlayerController::LandOn(size_t colliderIndex, float surfaceY)
    {
        const PlatformCollider& platform = m_level->GetActiveColliders()[colliderIndex];
        m_position.y = surfaceY;
        m_hasDoubleJumped = false;
        m_hasDashed = false;

        if (platform.bounceForce > 0.0f)
        {
            // Bouncy platforms launch the player instead of letting them stand.
            m_velocity.y = platform.bounceForce;
            m_grounded = false;
            m_groundPlatform = NO_PLATFORM;
            if (m_state != PlayerState::Dashing)
                TransitionState(PlayerState::Jumping);
            return;
        }

        m_velocity.y = 0.0f;
        m_grounded = true;
        m_groundPlatform = colliderIndex;
        m_level->NotifyPlatformStoodOn(colliderIndex);
    }

    bool PlatformerPlayerController::HandleKillPlane()
    {
        if (!m_level || m_position.y >= m_level->GetKillPlaneY())
            return false;

        // Falling out of the level always costs a life: shields, ghost mode, and invincibility frames
        // cannot keep a player with no ground beneath them alive.
        m_lives = std::max(m_lives - 1, 0);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Platformer player fell out of the level (lives remaining: %d)",
                       m_lives);
        if (m_lives == 0)
            EnterDeadState();
        else
            Respawn();
        return true;
    }

    void PlatformerPlayerController::EnterDeadState()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Platformer player died — restarting at last checkpoint");
        m_velocity = {0.0f, 0.0f, 0.0f};
        m_grounded = false;
        m_groundPlatform = NO_PLATFORM;
        m_deathRestartTimer = m_deathRestartDelay;
        TransitionState(PlayerState::Dead);
    }

    void PlatformerPlayerController::CheckGrounded()
    {
        // Downward probe against platform tops (the swept landing in MoveAndCollide handles arrival).
        m_grounded = false;
        m_groundPlatform = NO_PLATFORM;
        if (m_level && m_velocity.y <= 0.0f)
        {
            const auto& colliders = m_level->GetActiveColliders();
            float bestTop = 0.0f;
            for (size_t i = 0; i < colliders.size(); ++i)
            {
                const PlatformCollider& collider = colliders[i];
                if (!collider.solid || collider.bounceForce > 0.0f ||
                    !OverlapsFootprint(collider, m_position.x, m_position.z) ||
                    std::abs(m_position.y - collider.maxY) > kGroundProbe)
                {
                    continue;
                }
                if (m_groundPlatform == NO_PLATFORM || collider.maxY > bestTop)
                {
                    bestTop = collider.maxY;
                    m_groundPlatform = i;
                }
            }

            if (m_groundPlatform != NO_PLATFORM)
            {
                m_grounded = true;
                m_position.y = bestTop;
                m_level->NotifyPlatformStoodOn(m_groundPlatform);
            }
        }

        // Reset jump/dash when landing
        if (m_grounded && !m_wasGrounded)
        {
            m_hasDoubleJumped = false;
            m_hasDashed = false;
        }
    }

} // namespace Platformer
