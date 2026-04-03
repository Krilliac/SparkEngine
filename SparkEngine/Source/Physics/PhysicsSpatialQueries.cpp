#include "../Core/Platform.h"
/**
 * @file PhysicsSpatialQueries.cpp
 * @brief Raycasting, overlap queries, and shape cast (sweep) tests for PhysicsSystem
 * @author Spark Engine Team
 * @date 2025
 *
 * Raycast, RaycastAll, SphereOverlap, BoxOverlap, SphereCast, BoxCast, CapsuleCast.
 */

#include "PhysicsSystem.h"
#include "../Utils/ContainerUtils.h"
#include "../Utils/LogMacros.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>

JPH_SUPPRESS_WARNINGS
#include "JoltWarningRestore.h"

#include <mutex>

using namespace DirectX;

// ============================================================================
// RAYCASTING AND OVERLAP QUERIES
// ============================================================================

RaycastHit PhysicsSystem::Raycast(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance)
{
    RaycastHit hit;
    hit.hasHit = false;

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.raycastCount++;
    }

    SPARK_LOG_TRACE(Spark::LogCategory::Physics, "Raycast from (%.2f, %.2f, %.2f) dir (%.2f, %.2f, %.2f) maxDist=%.2f",
                    origin.x, origin.y, origin.z, direction.x, direction.y, direction.z, maxDistance);

    if (!m_joltSystem)
        return hit;

    JPH::RVec3 rayOrigin(origin.x, origin.y, origin.z);
    JPH::Vec3 rayDir(direction.x, direction.y, direction.z);
    if (rayDir.LengthSq() > 0.0f)
        rayDir = rayDir.Normalized();

    JPH::RRayCast ray(rayOrigin, rayDir * maxDistance);
    JPH::RayCastResult result;

    const auto& narrowPhase = m_joltSystem->GetNarrowPhaseQuery();
    if (narrowPhase.CastRay(ray, result))
    {
        hit.hasHit = true;

        JPH::RVec3 hitPoint = ray.GetPointOnRay(result.mFraction);
        hit.point = XMFLOAT3(static_cast<float>(hitPoint.GetX()), static_cast<float>(hitPoint.GetY()),
                             static_cast<float>(hitPoint.GetZ()));
        hit.distance = result.mFraction * maxDistance;

        // Get the hit body
        JPH::BodyID hitBodyID = result.mBodyID;
        auto& bodyInterface = m_joltSystem->GetBodyInterface();
        if (bodyInterface.IsAdded(hitBodyID))
        {
            // Get surface normal at hit point
            JPH::Body* joltBody = m_joltSystem->GetBodyLockInterface().TryGetBody(hitBodyID);
            if (joltBody && joltBody->GetShape())
            {
                JPH::Vec3 normal =
                    joltBody->GetShape()->GetSurfaceNormal(result.mSubShapeID2, ray.GetPointOnRay(result.mFraction));
                hit.normal = XMFLOAT3(normal.GetX(), normal.GetY(), normal.GetZ());
            }

            // Find our PhysicsBody wrapper
            uint64_t userData = bodyInterface.GetUserData(hitBodyID);
            if (userData != 0)
            {
                hit.body = reinterpret_cast<PhysicsBody*>(userData);
                if (hit.body)
                {
                    hit.userData = hit.body->GetUserData();
                    hit.entityId = hit.body->GetEntityID();
                }
            }
        }
    }

    return hit;
}

std::vector<RaycastHit> PhysicsSystem::RaycastAll(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance)
{
    std::vector<RaycastHit> hits;

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.raycastCount++;
    }

    if (!m_joltSystem)
        return hits;

    JPH::RVec3 rayOrigin(origin.x, origin.y, origin.z);
    JPH::Vec3 rayDir(direction.x, direction.y, direction.z);
    if (rayDir.LengthSq() > 0.0f)
        rayDir = rayDir.Normalized();

    JPH::RRayCast ray(rayOrigin, rayDir * maxDistance);

    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    const auto& narrowPhase = m_joltSystem->GetNarrowPhaseQuery();
    narrowPhase.CastRay(ray, JPH::RayCastSettings(), collector);

    collector.Sort();

    SPARK_LOG_TRACE(Spark::LogCategory::Physics, "RaycastAll: %zu hits", collector.mHits.size());

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    for (const auto& result : collector.mHits)
    {
        RaycastHit hit;
        hit.hasHit = true;

        JPH::RVec3 hitPoint = ray.GetPointOnRay(result.mFraction);
        hit.point = XMFLOAT3(static_cast<float>(hitPoint.GetX()), static_cast<float>(hitPoint.GetY()),
                             static_cast<float>(hitPoint.GetZ()));
        hit.distance = result.mFraction * maxDistance;
        hit.normal = {0, 1, 0}; // Default normal

        JPH::BodyID hitBodyID = result.mBodyID;
        if (bodyInterface.IsAdded(hitBodyID))
        {
            uint64_t userData = bodyInterface.GetUserData(hitBodyID);
            if (userData != 0)
            {
                hit.body = reinterpret_cast<PhysicsBody*>(userData);
                if (hit.body)
                {
                    hit.userData = hit.body->GetUserData();
                    hit.entityId = hit.body->GetEntityID();
                }
            }
        }

        hits.push_back(hit);
    }

    return hits;
}

bool PhysicsSystem::SphereOverlap(const XMFLOAT3& center, float radius, std::vector<PhysicsBody*>& results)
{
    results.clear();

    SPARK_LOG_TRACE(Spark::LogCategory::Physics, "SphereOverlap at (%.2f, %.2f, %.2f) radius=%.2f", center.x, center.y,
                    center.z, radius);

    if (!m_joltSystem)
        return false;

    // Use Jolt's CollideShape with a sphere
    JPH::SphereShape sphereShape(radius);
    JPH::RVec3 pos(center.x, center.y, center.z);

    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const auto& narrowPhase = m_joltSystem->GetNarrowPhaseQuery();
    narrowPhase.CollideShape(&sphereShape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(pos),
                             JPH::CollideShapeSettings(), JPH::RVec3::sZero(), collector);

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    for (const auto& result : collector.mHits)
    {
        if (bodyInterface.IsAdded(result.mBodyID2))
        {
            uint64_t userData = bodyInterface.GetUserData(result.mBodyID2);
            if (userData != 0)
            {
                auto* body = reinterpret_cast<PhysicsBody*>(userData);
                if (!Spark::ContainerUtils::Contains(results, body))
                {
                    results.push_back(body);
                }
            }
        }
    }

    return !results.empty();
}

bool PhysicsSystem::BoxOverlap(const XMFLOAT3& center, const XMFLOAT3& halfExtents, std::vector<PhysicsBody*>& results)
{
    results.clear();

    if (!m_joltSystem)
        return false;

    JPH::BoxShape boxShape(JPH::Vec3(halfExtents.x, halfExtents.y, halfExtents.z));
    JPH::RVec3 pos(center.x, center.y, center.z);

    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const auto& narrowPhase = m_joltSystem->GetNarrowPhaseQuery();
    narrowPhase.CollideShape(&boxShape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(pos),
                             JPH::CollideShapeSettings(), JPH::RVec3::sZero(), collector);

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    for (const auto& result : collector.mHits)
    {
        if (bodyInterface.IsAdded(result.mBodyID2))
        {
            uint64_t userData = bodyInterface.GetUserData(result.mBodyID2);
            if (userData != 0)
            {
                auto* body = reinterpret_cast<PhysicsBody*>(userData);
                if (!Spark::ContainerUtils::Contains(results, body))
                {
                    results.push_back(body);
                }
            }
        }
    }

    return !results.empty();
}

// ============================================================================
// SHAPE CASTING (SWEEP TESTS)
// ============================================================================

static RaycastHit PerformShapeCast(JPH::PhysicsSystem* joltSystem, const JPH::Shape* shape, const XMFLOAT3& from,
                                   const XMFLOAT3& to)
{
    RaycastHit hit;
    hit.hasHit = false;

    if (!joltSystem)
        return hit;

    JPH::RVec3 startPos(from.x, from.y, from.z);
    JPH::Vec3 direction(to.x - from.x, to.y - from.y, to.z - from.z);

    JPH::RShapeCast shapeCast = JPH::RShapeCast::sFromWorldTransform(shape, JPH::Vec3::sReplicate(1.0f),
                                                                     JPH::RMat44::sTranslation(startPos), direction);

    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const auto& narrowPhase = joltSystem->GetNarrowPhaseQuery();
    narrowPhase.CastShape(shapeCast, JPH::ShapeCastSettings(), JPH::RVec3::sZero(), collector);

    if (collector.HadHit())
    {
        hit.hasHit = true;

        JPH::RVec3 hitPoint = startPos + direction * collector.mHit.mFraction;
        hit.point = XMFLOAT3(static_cast<float>(hitPoint.GetX()), static_cast<float>(hitPoint.GetY()),
                             static_cast<float>(hitPoint.GetZ()));
        hit.normal = XMFLOAT3(collector.mHit.mPenetrationAxis.GetX(), collector.mHit.mPenetrationAxis.GetY(),
                              collector.mHit.mPenetrationAxis.GetZ());
        hit.distance = collector.mHit.mFraction * direction.Length();

        auto& bodyInterface = joltSystem->GetBodyInterface();
        if (bodyInterface.IsAdded(collector.mHit.mBodyID2))
        {
            uint64_t userData = bodyInterface.GetUserData(collector.mHit.mBodyID2);
            if (userData != 0)
            {
                hit.body = reinterpret_cast<PhysicsBody*>(userData);
                if (hit.body)
                {
                    hit.userData = hit.body->GetUserData();
                    hit.entityId = hit.body->GetEntityID();
                }
            }
        }
    }

    return hit;
}

RaycastHit PhysicsSystem::SphereCast(float radius, const XMFLOAT3& from, const XMFLOAT3& to)
{
    SPARK_LOG_TRACE(Spark::LogCategory::Physics, "SphereCast radius=%.2f from (%.2f,%.2f,%.2f) to (%.2f,%.2f,%.2f)",
                    radius, from.x, from.y, from.z, to.x, to.y, to.z);
    JPH::SphereShape sphereShape(radius);
    return PerformShapeCast(m_joltSystem.get(), &sphereShape, from, to);
}

RaycastHit PhysicsSystem::BoxCast(const XMFLOAT3& halfExtents, const XMFLOAT3& from, const XMFLOAT3& to)
{
    SPARK_LOG_TRACE(Spark::LogCategory::Physics,
                    "BoxCast halfExtents=(%.2f,%.2f,%.2f) from (%.2f,%.2f,%.2f) to (%.2f,%.2f,%.2f)", halfExtents.x,
                    halfExtents.y, halfExtents.z, from.x, from.y, from.z, to.x, to.y, to.z);
    JPH::BoxShape boxShape(JPH::Vec3(halfExtents.x, halfExtents.y, halfExtents.z));
    return PerformShapeCast(m_joltSystem.get(), &boxShape, from, to);
}

RaycastHit PhysicsSystem::CapsuleCast(float radius, float height, const XMFLOAT3& from, const XMFLOAT3& to)
{
    JPH::CapsuleShape capsuleShape(height / 2.0f, radius);
    return PerformShapeCast(m_joltSystem.get(), &capsuleShape, from, to);
}
