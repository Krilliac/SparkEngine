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

struct RigidBodyComponent
{
    enum class Type
    {
        Static,
        Kinematic,
        Dynamic
    };

    Type type = Type::Dynamic;
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.1f;
    float linearDamping = 0.1f;
    float angularDamping = 0.1f;
    bool isTrigger = false;
    DirectX::XMFLOAT3 linearVelocity{0, 0, 0};
    DirectX::XMFLOAT3 angularVelocity{0, 0, 0};
    Spark::PhysicsHandle physicsBodyHandle;

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
        if (friction < 0.0f || friction > 1.0f)
        {
            ASSERT_MSG(false, "Friction must be in [0, 1]");
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

struct ColliderComponent
{
    enum class Shape
    {
        Box,
        Sphere,
        Capsule,
        Mesh
    };

    Shape shape = Shape::Box;
    DirectX::XMFLOAT3 halfExtents{0.5f, 0.5f, 0.5f};
    float radius = 0.5f;
    float height = 1.0f;
    DirectX::XMFLOAT3 offset{0, 0, 0};

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
