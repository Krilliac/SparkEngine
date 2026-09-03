#include "../Core/Platform.h"
/**
 * @file PhysicsSpatialQueriesFiltered.cpp
 * @brief Layer-mask filtered raycast and overlap queries for PhysicsSystem
 * @author Spark Engine Team
 * @date 2025
 *
 * RaycastFiltered, RaycastAllFiltered, SphereOverlapFiltered, BoxOverlapFiltered.
 * Unfiltered raycast/overlap queries live in PhysicsSpatialQueries.cpp.
 * Shape casts (sweep tests) live in PhysicsSpatialQueriesCasts.cpp.
 */

#include "PhysicsSystem.h"
#include "PhysicsSpatialQueriesInternal.h"
#include "../Utils/ContainerUtils.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Body/BodyFilter.h>

JPH_SUPPRESS_WARNINGS
#include "JoltWarningRestore.h"

#include <mutex>

using namespace DirectX;

// ============================================================================
// FILTERED QUERIES (layer mask + ignore body)
// ============================================================================

RaycastHit PhysicsSystem::RaycastFiltered(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance,
                                          uint16_t layerMask, const PhysicsBody* ignoreBody)
{
    RaycastHit hit;
    hit.hasHit = false;

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.raycastCount++;
    }

    if (!m_joltSystem)
        return hit;

    JPH::RVec3 rayOrigin(origin.x, origin.y, origin.z);
    JPH::Vec3 rayDir(direction.x, direction.y, direction.z);
    if (rayDir.LengthSq() > 0.0f)
        rayDir = rayDir.Normalized();

    JPH::RRayCast ray(rayOrigin, rayDir * maxDistance);
    JPH::RayCastResult result;

    LayerMaskBodyFilter bodyFilter(layerMask, ignoreBody);
    const auto& narrowPhase = m_joltSystem->GetNarrowPhaseQuery();
    if (narrowPhase.CastRay(ray, result, {}, {}, bodyFilter))
    {
        hit.hasHit = true;

        JPH::RVec3 hitPoint = ray.GetPointOnRay(result.mFraction);
        hit.point = XMFLOAT3(static_cast<float>(hitPoint.GetX()), static_cast<float>(hitPoint.GetY()),
                             static_cast<float>(hitPoint.GetZ()));
        hit.distance = result.mFraction * maxDistance;

        JPH::BodyID hitBodyID = result.mBodyID;
        auto& bodyInterface = m_joltSystem->GetBodyInterface();
        if (bodyInterface.IsAdded(hitBodyID))
        {
            JPH::Body* joltBody = m_joltSystem->GetBodyLockInterface().TryGetBody(hitBodyID);
            if (joltBody && joltBody->GetShape())
            {
                JPH::Vec3 normal =
                    joltBody->GetShape()->GetSurfaceNormal(result.mSubShapeID2, ray.GetPointOnRay(result.mFraction));
                hit.normal = XMFLOAT3(normal.GetX(), normal.GetY(), normal.GetZ());
            }

            uint64_t userData = bodyInterface.GetUserData(hitBodyID);
            if (userData != 0)
            {
                hit.body = reinterpret_cast<PhysicsBody*>(userData);
                hit.userData = hit.body->GetUserData();
                hit.entityId = hit.body->GetEntityID();
            }
        }
    }

    return hit;
}

std::vector<RaycastHit> PhysicsSystem::RaycastAllFiltered(const XMFLOAT3& origin, const XMFLOAT3& direction,
                                                          float maxDistance, uint16_t layerMask,
                                                          const PhysicsBody* ignoreBody)
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

    LayerMaskBodyFilter bodyFilter(layerMask, ignoreBody);
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    const auto& narrowPhase = m_joltSystem->GetNarrowPhaseQuery();
    narrowPhase.CastRay(ray, JPH::RayCastSettings(), collector, {}, {}, bodyFilter);

    collector.Sort();

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    for (const auto& result : collector.mHits)
    {
        RaycastHit hit;
        hit.hasHit = true;

        JPH::RVec3 hitPoint = ray.GetPointOnRay(result.mFraction);
        hit.point = XMFLOAT3(static_cast<float>(hitPoint.GetX()), static_cast<float>(hitPoint.GetY()),
                             static_cast<float>(hitPoint.GetZ()));
        hit.distance = result.mFraction * maxDistance;
        hit.normal = {0, 1, 0}; // Default normal (matches RaycastAll)

        JPH::BodyID hitBodyID = result.mBodyID;
        if (bodyInterface.IsAdded(hitBodyID))
        {
            uint64_t userData = bodyInterface.GetUserData(hitBodyID);
            if (userData != 0)
            {
                hit.body = reinterpret_cast<PhysicsBody*>(userData);
                hit.userData = hit.body->GetUserData();
                hit.entityId = hit.body->GetEntityID();
            }
        }

        hits.push_back(hit);
    }

    return hits;
}

static bool PerformShapeOverlapFiltered(JPH::PhysicsSystem* joltSystem, const JPH::Shape* shape, const XMFLOAT3& center,
                                        const JPH::BodyFilter& bodyFilter, std::vector<PhysicsBody*>& results)
{
    results.clear();

    if (!joltSystem)
        return false;

    JPH::RVec3 pos(center.x, center.y, center.z);

    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const auto& narrowPhase = joltSystem->GetNarrowPhaseQuery();
    narrowPhase.CollideShape(shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(pos),
                             JPH::CollideShapeSettings(), JPH::RVec3::sZero(), collector, {}, {}, bodyFilter);

    auto& bodyInterface = joltSystem->GetBodyInterface();
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

bool PhysicsSystem::SphereOverlapFiltered(const XMFLOAT3& center, float radius, uint16_t layerMask,
                                          std::vector<PhysicsBody*>& results, const PhysicsBody* ignoreBody)
{
    JPH::SphereShape sphereShape(radius);
    LayerMaskBodyFilter bodyFilter(layerMask, ignoreBody);
    return PerformShapeOverlapFiltered(m_joltSystem.get(), &sphereShape, center, bodyFilter, results);
}

bool PhysicsSystem::BoxOverlapFiltered(const XMFLOAT3& center, const XMFLOAT3& halfExtents, uint16_t layerMask,
                                       std::vector<PhysicsBody*>& results, const PhysicsBody* ignoreBody)
{
    JPH::BoxShape boxShape(JPH::Vec3(halfExtents.x, halfExtents.y, halfExtents.z));
    LayerMaskBodyFilter bodyFilter(layerMask, ignoreBody);
    return PerformShapeOverlapFiltered(m_joltSystem.get(), &boxShape, center, bodyFilter, results);
}
