#include "../Core/Platform.h"
/**
 * @file PhysicsSpatialQueriesCasts.cpp
 * @brief Shape cast (sweep) tests for PhysicsSystem
 * @author Spark Engine Team
 * @date 2025
 *
 * SphereCast, BoxCast, CapsuleCast and their layer-mask filtered variants.
 * Unfiltered raycast/overlap queries live in PhysicsSpatialQueries.cpp.
 * Filtered raycast/overlap queries live in PhysicsSpatialQueriesFiltered.cpp.
 */

#include "PhysicsSystem.h"
#include "PhysicsSpatialQueriesInternal.h"
#include "../Utils/LogMacros.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Body/BodyFilter.h>

JPH_SUPPRESS_WARNINGS
#include "JoltWarningRestore.h"

using namespace DirectX;

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
                hit.userData = hit.body->GetUserData();
                hit.entityId = hit.body->GetEntityID();
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
// FILTERED SHAPE CASTS (layer mask + ignore body)
// ============================================================================

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
