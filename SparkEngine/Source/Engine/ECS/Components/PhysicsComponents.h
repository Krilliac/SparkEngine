/**
 * @file PhysicsComponents.h
 * @brief ECS physics components: RigidBodyComponent, ColliderComponent
 *
 * Split from the monolithic Components.h for faster compilation and
 * clearer separation of concerns.
 */

#pragma once
#include "../../../Core/Platform.h"
#include "../../../Utils/OpaqueHandle.h"
#include "../../../Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

// =============================================================================
// RigidBodyComponent
// =============================================================================

/**
 * @brief Links an entity to a Jolt Physics rigid body.
 *
 * Dynamic bodies read position/rotation back from the physics simulation each
 * frame (physics -> Transform). Kinematic bodies push their Transform into
 * physics (Transform -> physics). Static bodies never move.
 */
struct RigidBodyComponent
{
    /// Determines how the physics engine treats this body.
    enum class Type
    {
        Static,    ///< Immovable collider (walls, floors). Zero mass.
        Kinematic, ///< Moved by game code, pushes dynamic bodies but ignores forces.
        Dynamic    ///< Fully simulated: responds to gravity, forces, and collisions.
    };

    /// Motion quality for collision detection precision.
    enum class MotionQuality
    {
        Discrete,  ///< Standard discrete detection (cheapest).
        LinearCast ///< CCD via linear cast — prevents tunneling for fast objects.
    };

    Type type = Type::Dynamic;   ///< Body simulation mode.
    float mass = 1.0f;           ///< Mass in kg (ignored for Static/Kinematic).
    float friction = 0.5f;       ///< Surface friction coefficient [0, ∞).
    float restitution = 0.1f;    ///< Bounciness [0, 1]; 0 = no bounce, 1 = perfectly elastic.
    float linearDamping = 0.1f;  ///< Velocity drag per second (simulates air resistance).
    float angularDamping = 0.1f; ///< Rotational drag per second.
    float gravityFactor = 1.0f;  ///< Per-body gravity multiplier (0 = zero-g, 2 = double).
    bool isTrigger = false;      ///< When true, detects overlaps but doesn't physically collide.
    MotionQuality motionQuality = MotionQuality::Discrete; ///< CCD setting for fast-moving objects.
    DirectX::XMFLOAT3 linearVelocity{0, 0, 0};  ///< Cached linear velocity (m/s), synced from physics each frame.
    DirectX::XMFLOAT3 angularVelocity{0, 0, 0}; ///< Cached angular velocity (rad/s), synced from physics each frame.
    Spark::PhysicsHandle physicsBodyHandle;     ///< Opaque handle to the underlying Jolt Physics body.

    /**
     * @brief Validate that all physics parameters are within sane ranges.
     * @return true if all parameters are valid.
     */
    bool Validate() const
    {
        if (type == Type::Dynamic && mass <= 0.0f)
        {
            ASSERT_MSG(false, "Dynamic rigid body must have positive mass");
            return false;
        }
        if (friction < 0.0f)
        {
            ASSERT_MSG(false, "Friction must be non-negative");
            return false;
        }
        if (restitution < 0.0f || restitution > 1.0f)
        {
            ASSERT_MSG(false, "Restitution must be in [0, 1]");
            return false;
        }
        if (linearDamping < 0.0f)
        {
            ASSERT_MSG(false, "Linear damping must be non-negative");
            return false;
        }
        if (angularDamping < 0.0f)
        {
            ASSERT_MSG(false, "Angular damping must be non-negative");
            return false;
        }
        return true;
    }
};

// =============================================================================
// ColliderComponent
// =============================================================================

/**
 * @brief Defines the collision shape attached to a rigid body.
 *
 * Only the fields relevant to the chosen `shape` are used by physics.
 * For example, `radius` is only meaningful for Sphere and Capsule shapes.
 */
struct ColliderComponent
{
    /// Primitive collision shape type.
    enum class Shape
    {
        Box,        ///< Axis-aligned box defined by halfExtents.
        Sphere,     ///< Sphere defined by radius.
        Capsule,    ///< Capsule (cylinder + hemisphere caps) defined by radius and height.
        Cylinder,   ///< Cylinder defined by radius and height.
        ConvexHull, ///< Convex hull from mesh vertices.
        Mesh,       ///< Triangle mesh loaded from the entity's MeshRenderer asset.
        Heightfield ///< Terrain heightfield (static only).
    };

    Shape shape = Shape::Box;                        ///< Collision primitive type.
    DirectX::XMFLOAT3 halfExtents{0.5f, 0.5f, 0.5f}; ///< Box half-dimensions in local space (meters).
    float radius = 0.5f;                             ///< Sphere/Capsule/Cylinder radius (meters).
    float height = 1.0f;                             ///< Capsule/Cylinder total height (meters).
    DirectX::XMFLOAT3 offset{0, 0, 0};               ///< Local-space offset from the entity's Transform origin.

    /**
     * @brief Validate that all collider dimensions are positive.
     * @return true if all parameters are valid.
     */
    bool Validate() const
    {
        if (halfExtents.x <= 0.0f || halfExtents.y <= 0.0f || halfExtents.z <= 0.0f)
        {
            ASSERT_MSG(false, "Collider half extents must be positive");
            return false;
        }
        if (radius <= 0.0f)
        {
            ASSERT_MSG(false, "Collider radius must be positive");
            return false;
        }
        if (height <= 0.0f)
        {
            ASSERT_MSG(false, "Collider height must be positive");
            return false;
        }
        return true;
    }
};
