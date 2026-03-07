/**
 * @file PhysicsTypes.h
 * @brief Physics type definitions, enums, and descriptors
 * @author Spark Engine Team
 * @date 2025
 *
 * Extracted from PhysicsSystem.h so that code needing only physics types
 * (e.g. ECS components, editor panels) can include this lightweight header
 * without pulling in the full PhysicsSystem class and its Bullet dependencies.
 */

#pragma once
#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <cstdint>

using DirectX::XMFLOAT3;

// =============================================================================
// Enums
// =============================================================================

enum class PhysicsBodyType
{
    Static,
    Kinematic,
    Dynamic
};

enum class CollisionShapeType
{
    Box,
    Sphere,
    Capsule,
    Cylinder,
    Cone,
    Mesh,
    ConvexHull,
    Heightfield,
    Compound
};

enum class ConstraintType
{
    Point2Point,
    Hinge,
    Slider,
    ConeTwist,
    Generic6DOF,
    Fixed
};

// =============================================================================
// Structs
// =============================================================================

struct PhysicsMaterial
{
    float friction = 0.5f;
    float restitution = 0.1f;
    float linearDamping = 0.1f;
    float angularDamping = 0.1f;
    float density = 1.0f;
    bool isStatic = false;
    std::string name;
};

struct CollisionShapeDesc
{
    CollisionShapeType type = CollisionShapeType::Box;
    XMFLOAT3 dimensions = {1.0f, 1.0f, 1.0f};
    float radius = 0.5f;
    float height = 1.0f;
    std::string meshPath;
    std::vector<XMFLOAT3> vertices;
    std::vector<uint32_t> indices;
    XMFLOAT3 localOffset = {0, 0, 0};
    XMFLOAT3 localRotation = {0, 0, 0};
};

struct PhysicsBodyDesc
{
    PhysicsBodyType type = PhysicsBodyType::Dynamic;
    XMFLOAT3 position = {0, 0, 0};
    XMFLOAT3 rotation = {0, 0, 0};
    XMFLOAT3 linearVelocity = {0, 0, 0};
    XMFLOAT3 angularVelocity = {0, 0, 0};
    float mass = 1.0f;
    PhysicsMaterial material;
    CollisionShapeDesc shape;
    bool isTrigger = false;
    bool isKinematic = false;
    std::string name;
    void* userData = nullptr;
};

struct RaycastHit
{
    bool hasHit = false;
    XMFLOAT3 point = {0, 0, 0};
    XMFLOAT3 normal = {0, 1, 0};
    float distance = 0.0f;
    class PhysicsBody* body = nullptr;
    void* userData = nullptr;
};

struct ContactInfo
{
    class PhysicsBody* bodyA = nullptr;
    class PhysicsBody* bodyB = nullptr;
    XMFLOAT3 contactPoint = {0, 0, 0};
    XMFLOAT3 contactNormal = {0, 1, 0};
    float penetrationDepth = 0.0f;
    float appliedImpulse = 0.0f;
};
