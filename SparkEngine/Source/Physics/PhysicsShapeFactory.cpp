#include "../Core/Platform.h"
/**
 * @file PhysicsShapeFactory.cpp
 * @brief Collision shape creation and Bullet vector/quaternion conversion helpers
 * @author Spark Engine Team
 * @date 2025
 *
 * Extracted from PhysicsSystem.cpp. Contains ToBullet/FromBullet conversions,
 * all Create*Shape helpers, and the shape hash function used by the shape cache.
 */

#include "PhysicsSystem.h"
#include "../Utils/Hash.h"

#include <btBulletDynamicsCommon.h>

using namespace DirectX;

// ============================================================================
// BULLET CONVERSION HELPERS
// ============================================================================

btVector3 PhysicsSystem::ToBullet(const XMFLOAT3& vec) const
{
    return btVector3(vec.x, vec.y, vec.z);
}

XMFLOAT3 PhysicsSystem::FromBullet(const btVector3& vec) const
{
    return XMFLOAT3(vec.getX(), vec.getY(), vec.getZ());
}

btQuaternion PhysicsSystem::ToBulletQuaternion(const XMFLOAT3& euler) const
{
    btQuaternion quat;
    quat.setEulerZYX(euler.z, euler.y, euler.x);
    return quat;
}

XMFLOAT3 PhysicsSystem::FromBullet(const btQuaternion& quat) const
{
    btScalar yaw, pitch, roll;
    btMatrix3x3(quat).getEulerYPR(yaw, pitch, roll);
    return XMFLOAT3(roll, yaw, pitch);
}

// ============================================================================
// COLLISION SHAPE CREATION
// ============================================================================

btCollisionShape* PhysicsSystem::CreateCollisionShape(const CollisionShapeDesc& desc)
{
    // Check shape cache first
    size_t hash = HashShape(desc);
    auto it = m_shapeCache.find(hash);
    if (it != m_shapeCache.end())
    {
        return it->second;
    }

    btCollisionShape* shape = nullptr;

    switch (desc.type)
    {
    case CollisionShapeType::Box:
        shape = CreateBoxShape(desc.dimensions);
        break;
    case CollisionShapeType::Sphere:
        shape = CreateSphereShape(desc.radius);
        break;
    case CollisionShapeType::Capsule:
        shape = CreateCapsuleShape(desc.radius, desc.height);
        break;
    case CollisionShapeType::Cylinder:
        shape = CreateCylinderShape(desc.radius, desc.height);
        break;
    case CollisionShapeType::Cone:
        shape = CreateConeShape(desc.radius, desc.height);
        break;
    case CollisionShapeType::Mesh:
        shape = CreateMeshShape(desc.vertices, desc.indices);
        break;
    case CollisionShapeType::ConvexHull:
        shape = CreateConvexHullShape(desc.vertices);
        break;
    default:
        // Default to box shape
        shape = CreateBoxShape(desc.dimensions);
        break;
    }

    if (shape)
    {
        m_shapeCache[hash] = shape;
    }

    return shape;
}

btCollisionShape* PhysicsSystem::CreateBoxShape(const XMFLOAT3& dimensions)
{
    return new btBoxShape(btVector3(dimensions.x / 2.0f, dimensions.y / 2.0f, dimensions.z / 2.0f));
}

btCollisionShape* PhysicsSystem::CreateSphereShape(float radius)
{
    return new btSphereShape(radius);
}

btCollisionShape* PhysicsSystem::CreateCapsuleShape(float radius, float height)
{
    return new btCapsuleShape(radius, height);
}

btCollisionShape* PhysicsSystem::CreateCylinderShape(float radius, float height)
{
    return new btCylinderShape(btVector3(radius, height / 2.0f, radius));
}

btCollisionShape* PhysicsSystem::CreateConeShape(float radius, float height)
{
    return new btConeShape(radius, height);
}

btCollisionShape* PhysicsSystem::CreateMeshShape(const std::vector<XMFLOAT3>& vertices,
                                                 const std::vector<uint32_t>& indices)
{
    if (vertices.empty() || indices.empty() || indices.size() % 3 != 0)
    {
        // Fall back to a unit box if mesh data is invalid
        return CreateBoxShape({1.0f, 1.0f, 1.0f});
    }

    btTriangleMesh* triMesh = new btTriangleMesh();

    const auto vertexCount = static_cast<uint32_t>(vertices.size());
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        // Bounds-check indices to prevent out-of-range access on corrupted data
        if (indices[i] >= vertexCount || indices[i + 1] >= vertexCount || indices[i + 2] >= vertexCount)
            continue;

        const XMFLOAT3& v0 = vertices[indices[i]];
        const XMFLOAT3& v1 = vertices[indices[i + 1]];
        const XMFLOAT3& v2 = vertices[indices[i + 2]];

        triMesh->addTriangle(btVector3(v0.x, v0.y, v0.z), btVector3(v1.x, v1.y, v1.z), btVector3(v2.x, v2.y, v2.z));
    }

    // Track the triangle mesh for cleanup — btBvhTriangleMeshShape does not own it
    m_triangleMeshes.push_back(triMesh);

    btBvhTriangleMeshShape* meshShape = new btBvhTriangleMeshShape(triMesh, true);
    return meshShape;
}

btCollisionShape* PhysicsSystem::CreateConvexHullShape(const std::vector<XMFLOAT3>& vertices)
{
    if (vertices.empty())
    {
        return CreateBoxShape({1.0f, 1.0f, 1.0f});
    }

    btConvexHullShape* convexShape = new btConvexHullShape();

    for (const auto& v : vertices)
    {
        convexShape->addPoint(btVector3(v.x, v.y, v.z), false);
    }

    convexShape->recalcLocalAabb();
    return convexShape;
}

size_t PhysicsSystem::HashShape(const CollisionShapeDesc& desc) const
{
    size_t hash = std::hash<int>()(static_cast<int>(desc.type));

    Spark::CombineHash(hash, std::hash<float>()(desc.dimensions.x));
    Spark::CombineHash(hash, std::hash<float>()(desc.dimensions.y));
    Spark::CombineHash(hash, std::hash<float>()(desc.dimensions.z));
    Spark::CombineHash(hash, std::hash<float>()(desc.radius));
    Spark::CombineHash(hash, std::hash<float>()(desc.height));
    Spark::CombineHash(hash, std::hash<std::string>()(desc.meshPath));
    Spark::CombineHash(hash, std::hash<size_t>()(desc.vertices.size()));
    Spark::CombineHash(hash, std::hash<size_t>()(desc.indices.size()));

    return hash;
}
