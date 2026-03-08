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
};
