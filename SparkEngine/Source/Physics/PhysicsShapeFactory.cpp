#include "../Core/Platform.h"
/**
 * @file PhysicsShapeFactory.cpp
 * @brief Collision shape creation for Jolt Physics
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains all shape creation helpers and the shape hash function used by the
 * shape cache. Shapes are created as Jolt ShapeRefC (ref-counted) and stored
 * as void* in the cache for type erasure at the PhysicsSystem level.
 */

#include "PhysicsSystem.h"
#include "../Utils/Hash.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCylinderShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/EmptyShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

JPH_SUPPRESS_WARNINGS

using namespace DirectX;

// ============================================================================
// COLLISION SHAPE CREATION
// ============================================================================

void* PhysicsSystem::CreateCollisionShape(const CollisionShapeDesc& desc)
{
    size_t hash = HashShape(desc);
    auto it = m_shapeCache.find(hash);
    if (it != m_shapeCache.end())
    {
        return it->second;
    }

    void* shape = nullptr;

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
    case CollisionShapeType::Heightfield:
    {
        if (desc.heightfieldData.empty() || desc.heightfieldSamples == 0)
        {
            shape = CreateBoxShape(desc.dimensions);
        }
        else
        {
            JPH::HeightFieldShapeSettings hfSettings(desc.heightfieldData.data(), JPH::Vec3::sZero(),
                                                     JPH::Vec3(desc.dimensions.x / desc.heightfieldSamples,
                                                               desc.heightfieldScale,
                                                               desc.dimensions.z / desc.heightfieldSamples),
                                                     desc.heightfieldSamples);
            auto result = hfSettings.Create();
            if (!result.HasError())
                shape = new JPH::ShapeRefC(result.Get());
            else
                shape = CreateBoxShape(desc.dimensions);
        }
        break;
    }
    case CollisionShapeType::TaperedCapsule:
    {
        JPH::TaperedCapsuleShapeSettings tcSettings(desc.height / 2.0f, desc.radius, desc.topRadius);
        auto result = tcSettings.Create();
        if (!result.HasError())
            shape = new JPH::ShapeRefC(result.Get());
        else
            shape = CreateCapsuleShape(desc.radius, desc.height);
        break;
    }
    case CollisionShapeType::TaperedCylinder:
    {
        JPH::TaperedCylinderShapeSettings tcSettings(desc.height / 2.0f, desc.radius, desc.topRadius);
        auto result = tcSettings.Create();
        if (!result.HasError())
            shape = new JPH::ShapeRefC(result.Get());
        else
            shape = CreateCylinderShape(desc.radius, desc.height);
        break;
    }
    case CollisionShapeType::Plane:
    {
        JPH::PlaneShapeSettings planeSettings(
            JPH::Plane(JPH::Vec3(desc.planeNormal.x, desc.planeNormal.y, desc.planeNormal.z), desc.planeDistance));
        auto result = planeSettings.Create();
        if (!result.HasError())
            shape = new JPH::ShapeRefC(result.Get());
        else
            shape = CreateBoxShape({100.0f, 0.1f, 100.0f});
        break;
    }
    case CollisionShapeType::Empty:
    {
        JPH::EmptyShapeSettings emptySettings;
        auto result = emptySettings.Create();
        if (!result.HasError())
            shape = new JPH::ShapeRefC(result.Get());
        break;
    }
    case CollisionShapeType::Compound:
    case CollisionShapeType::Scaled:
    default:
        shape = CreateBoxShape(desc.dimensions);
        break;
    }

    if (shape)
    {
        m_shapeCache[hash] = shape;
    }

    return shape;
}

void* PhysicsSystem::CreateBoxShape(const XMFLOAT3& dimensions)
{
    // Jolt BoxShape takes half-extents directly
    JPH::BoxShapeSettings settings(JPH::Vec3(dimensions.x / 2.0f, dimensions.y / 2.0f, dimensions.z / 2.0f));
    auto result = settings.Create();
    if (result.HasError())
        return nullptr;

    // Store a new ref-counted shape on the heap
    auto* shapeRef = new JPH::ShapeRefC(result.Get());
    return shapeRef;
}

void* PhysicsSystem::CreateSphereShape(float radius)
{
    JPH::SphereShapeSettings settings(radius);
    auto result = settings.Create();
    if (result.HasError())
        return nullptr;

    auto* shapeRef = new JPH::ShapeRefC(result.Get());
    return shapeRef;
}

void* PhysicsSystem::CreateCapsuleShape(float radius, float height)
{
    // Jolt CapsuleShape takes half-height of the cylindrical part
    JPH::CapsuleShapeSettings settings(height / 2.0f, radius);
    auto result = settings.Create();
    if (result.HasError())
        return nullptr;

    auto* shapeRef = new JPH::ShapeRefC(result.Get());
    return shapeRef;
}

void* PhysicsSystem::CreateCylinderShape(float radius, float height)
{
    JPH::CylinderShapeSettings settings(height / 2.0f, radius);
    auto result = settings.Create();
    if (result.HasError())
        return nullptr;

    auto* shapeRef = new JPH::ShapeRefC(result.Get());
    return shapeRef;
}

void* PhysicsSystem::CreateConeShape(float radius, float height)
{
    // Jolt doesn't have a native cone shape — approximate with a convex hull
    // Generate cone vertices: apex + base circle
    JPH::Array<JPH::Vec3> points;
    points.push_back(JPH::Vec3(0, height, 0)); // Apex

    constexpr int segments = 16;
    for (int i = 0; i < segments; ++i)
    {
        float angle = (2.0f * 3.14159265f * i) / segments;
        points.push_back(JPH::Vec3(radius * std::cos(angle), 0, radius * std::sin(angle)));
    }

    JPH::ConvexHullShapeSettings settings(points);
    auto result = settings.Create();
    if (result.HasError())
        return CreateBoxShape({radius * 2, height, radius * 2}); // Fallback

    auto* shapeRef = new JPH::ShapeRefC(result.Get());
    return shapeRef;
}

void* PhysicsSystem::CreateMeshShape(const std::vector<XMFLOAT3>& vertices, const std::vector<uint32_t>& indices)
{
    if (vertices.empty() || indices.empty() || indices.size() % 3 != 0)
    {
        return CreateBoxShape({1.0f, 1.0f, 1.0f});
    }

    JPH::TriangleList triangles;
    const auto vertexCount = static_cast<uint32_t>(vertices.size());

    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        if (indices[i] >= vertexCount || indices[i + 1] >= vertexCount || indices[i + 2] >= vertexCount)
            continue;

        const XMFLOAT3& v0 = vertices[indices[i]];
        const XMFLOAT3& v1 = vertices[indices[i + 1]];
        const XMFLOAT3& v2 = vertices[indices[i + 2]];

        triangles.push_back(
            JPH::Triangle(JPH::Float3(v0.x, v0.y, v0.z), JPH::Float3(v1.x, v1.y, v1.z), JPH::Float3(v2.x, v2.y, v2.z)));
    }

    JPH::MeshShapeSettings settings(triangles);
    auto result = settings.Create();
    if (result.HasError())
        return CreateBoxShape({1.0f, 1.0f, 1.0f});

    auto* shapeRef = new JPH::ShapeRefC(result.Get());
    return shapeRef;
}

void* PhysicsSystem::CreateConvexHullShape(const std::vector<XMFLOAT3>& vertices)
{
    if (vertices.empty())
    {
        return CreateBoxShape({1.0f, 1.0f, 1.0f});
    }

    JPH::Array<JPH::Vec3> points;
    points.reserve(vertices.size());
    for (const auto& v : vertices)
    {
        points.push_back(JPH::Vec3(v.x, v.y, v.z));
    }

    JPH::ConvexHullShapeSettings settings(points);
    auto result = settings.Create();
    if (result.HasError())
        return CreateBoxShape({1.0f, 1.0f, 1.0f});

    auto* shapeRef = new JPH::ShapeRefC(result.Get());
    return shapeRef;
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
