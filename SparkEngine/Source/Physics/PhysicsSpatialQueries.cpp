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
#include <Jolt/Physics/Body/BodyFilter.h>

JPH_SUPPRESS_WARNINGS
#include "JoltWarningRestore.h"

#include <mutex>

using namespace DirectX;

// ============================================================================
// LAYER-MASK BODY FILTER
// ============================================================================

namespace
{
    /**
     * @brief Jolt BodyFilter that applies the engine's collision-layer bitmask
     *        (PhysicsBody::GetCollisionGroup() vs. query layerMask) and an
     *        optional single-body exclusion.
     *
     * Bodies without a PhysicsBody wrapper (userData == 0) always pass the
     * layer test so raw Jolt-internal bodies (e.g. world geometry created
     * outside CreateBody) are never silently skipped.
     */
    class LayerMaskBodyFilter final : public JPH::BodyFilter
    {
      public:
        LayerMaskBodyFilter(uint16_t layerMask, const PhysicsBody* ignoreBody)
            : m_layerMask(layerMask), m_ignoreBody(ignoreBody)
        {
        }

        bool ShouldCollide(const JPH::BodyID& inBodyID) const override
        {
            // Cheap broadphase-level rejection of the ignored body.
            if (m_ignoreBody != nullptr && inBodyID.GetIndexAndSequenceNumber() == m_ignoreBody->GetJoltBodyID())
            {
                return false;
            }
            return true;
        }

        bool ShouldCollideLocked(const JPH::Body& inBody) const override
        {
            const auto* body = reinterpret_cast<const PhysicsBody*>(inBody.GetUserData());
            if (body == nullptr)
            {
                return true; // No wrapper — cannot classify; include (see class doc)
            }
            if (body == m_ignoreBody)
            {
                return false;
            }
            return (body->GetCollisionGroup() & m_layerMask) != 0;
        }

      private:
        uint16_t m_layerMask;
        const PhysicsBody* m_ignoreBody;
    };
} // namespace

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
                                   const XMFLOAT3& to, const JPH::BodyFilter& bodyFilter = {})
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
    narrowPhase.CastShape(shapeCast, JPH::ShapeCastSettings(), JPH::RVec3::sZero(), collector, {}, {}, bodyFilter);

    if (collector.HadHit())
    {
        hit.hasHit = true;

        // Contact point on the surface of the hit body (world space — base offset is zero).
        JPH::Vec3 contactPoint = collector.mHit.mContactPointOn2;
        hit.point = XMFLOAT3(contactPoint.GetX(), contactPoint.GetY(), contactPoint.GetZ());

        // Jolt convention: -mPenetrationAxis.Normalized() is the contact normal on the
        // hit surface (unit length, facing back toward the cast shape).
        JPH::Vec3 penetrationAxis = collector.mHit.mPenetrationAxis;
        if (penetrationAxis.LengthSq() > 1.0e-12f)
        {
            JPH::Vec3 normal = -penetrationAxis.Normalized();
            hit.normal = XMFLOAT3(normal.GetX(), normal.GetY(), normal.GetZ());
        }

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

RaycastHit PhysicsSystem::SphereCastFiltered(float radius, const XMFLOAT3& from, const XMFLOAT3& to, uint16_t layerMask,
                                             const PhysicsBody* ignoreBody)
{
    JPH::SphereShape sphereShape(radius);
    LayerMaskBodyFilter bodyFilter(layerMask, ignoreBody);
    return PerformShapeCast(m_joltSystem.get(), &sphereShape, from, to, bodyFilter);
}

RaycastHit PhysicsSystem::BoxCastFiltered(const XMFLOAT3& halfExtents, const XMFLOAT3& from, const XMFLOAT3& to,
                                          uint16_t layerMask, const PhysicsBody* ignoreBody)
{
    JPH::BoxShape boxShape(JPH::Vec3(halfExtents.x, halfExtents.y, halfExtents.z));
    LayerMaskBodyFilter bodyFilter(layerMask, ignoreBody);
    return PerformShapeCast(m_joltSystem.get(), &boxShape, from, to, bodyFilter);
}

RaycastHit PhysicsSystem::CapsuleCastFiltered(float radius, float height, const XMFLOAT3& from, const XMFLOAT3& to,
                                              uint16_t layerMask, const PhysicsBody* ignoreBody)
{
    JPH::CapsuleShape capsuleShape(height / 2.0f, radius);
    LayerMaskBodyFilter bodyFilter(layerMask, ignoreBody);
    return PerformShapeCast(m_joltSystem.get(), &capsuleShape, from, to, bodyFilter);
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
