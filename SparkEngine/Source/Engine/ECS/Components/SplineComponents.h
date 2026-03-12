/**
 * @file SplineComponents.h
 * @brief ECS components for spline paths and path following
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Provides two components:
 * - **SplineComponent** — Stores a SplinePath on an entity that other entities
 *   can follow.
 * - **SplineFollowerComponent** — Makes an entity move along a spline path
 *   defined by another entity's SplineComponent.
 */

#pragma once

#include "../../../Core/Platform.h"
#include "../../../Utils/SplinePath.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <entt/entt.hpp>

using EntityID = entt::entity;

// =============================================================================
// SplineComponent
// =============================================================================

/**
 * @struct SplineComponent
 * @brief Stores a spline path that can be followed by other entities.
 *
 * Attach this to an entity to define a path in world space. Other entities
 * with SplineFollowerComponent can reference this entity to follow the path.
 */
struct SplineComponent
{
    SplinePath path;           ///< The spline path data.
    bool debugVisible = false; ///< Whether to draw debug visualization of the path.
};

// =============================================================================
// SplineFollowerComponent
// =============================================================================

/**
 * @enum SplineLoopMode
 * @brief Controls what happens when a spline follower reaches the end of its path.
 */
enum class SplineLoopMode
{
    Once,    ///< Stop at the end of the path.
    Loop,    ///< Jump back to the start and repeat.
    PingPong ///< Reverse direction at each end.
};

/**
 * @struct SplineFollowerComponent
 * @brief Makes an entity follow a spline path at a configurable speed.
 *
 * The SplineFollowerSystem reads this component each frame and advances the
 * entity along the referenced spline path.
 */
struct SplineFollowerComponent
{
    EntityID splineEntity = entt::null;             ///< Entity holding the SplineComponent to follow.
    float speed = 1.0f;                             ///< Movement speed in world units per second.
    float currentDistance = 0.0f;                   ///< Current distance along the spline (world units).
    SplineLoopMode loopMode = SplineLoopMode::Once; ///< Behavior at path endpoints.
    bool playing = true;                            ///< Whether the follower is actively advancing.
    bool orientToPath = true;                       ///< Align entity rotation to the spline tangent.
    bool reverse = false;                           ///< Current movement direction (used by PingPong).
    bool finished = false;                          ///< True when Once mode has reached the end.
};
