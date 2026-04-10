/**
 * @file SpringArm.h
 * @brief Camera collision avoidance arm (spring arm / camera boom)
 * @author Spark Engine Team
 * @date 2026
 *
 * A spring arm extends from a target position and performs a sphere cast
 * to detect collisions. If an obstacle is hit, the arm shortens to prevent
 * the camera from clipping through geometry. Common in third-person games.
 *
 * @note This is an **intentional stateless utility**. `SpringArmState` is
 *       a value type with a pure-CPU `Update()` method; there is no
 *       singleton to initialize. It is referenced in `SpringArmComponent`
 *       (see `AdvancedPlacementComponents.h`), which currently duplicates
 *       the state fields — a follow-up will unify them by having the
 *       component hold a `SpringArmState` directly and having a new
 *       camera-update system call `Update()` each frame with the collision
 *       result from the physics world. Until then the helper is exercised
 *       via `Tests/TestSpringArm.cpp`.
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <algorithm>
#include <cmath>

namespace Spark::Graphics
{

    /**
     * @brief Configuration and runtime state for a spring arm.
     */
    struct SpringArmState
    {
        // Configuration
        float targetLength = 5.0f;           ///< Desired arm length (meters).
        float probeRadius = 0.2f;            ///< Sphere cast radius for collision.
        float smoothSpeed = 10.0f;           ///< Interpolation speed for arm extension.
        float minLength = 0.5f;              ///< Minimum arm length (prevents camera inside target).
        bool doCollisionTest = true;         ///< Enable collision testing.
        uint32_t collisionMask = 0xFFFFFFFF; ///< Collision layer mask.

        // Runtime state
        float currentLength = 5.0f;  ///< Current arm length after collision.
        bool isColliding = false;    ///< Whether the arm hit something this frame.
        XMFLOAT3 hitNormal{0, 0, 0}; ///< Normal of the last collision hit.

        /**
         * @brief Update the spring arm length based on collision.
         * @param hitDistance Distance to the nearest collision (FLT_MAX if none).
         * @param deltaTime Frame delta time.
         */
        void Update(float hitDistance, float deltaTime)
        {
            float desiredLength = targetLength;
            if (doCollisionTest && hitDistance < targetLength)
            {
                desiredLength = std::max(hitDistance - probeRadius, minLength);
                isColliding = true;
            }
            else
            {
                isColliding = false;
            }

            // Smooth interpolation toward desired length
            float t = 1.0f - std::exp(-smoothSpeed * deltaTime);
            currentLength = currentLength + (desiredLength - currentLength) * t;
            currentLength = std::clamp(currentLength, minLength, targetLength);
        }
    };

} // namespace Spark::Graphics
