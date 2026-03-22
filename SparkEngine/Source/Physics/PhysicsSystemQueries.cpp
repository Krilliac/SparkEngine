#include "../Core/Platform.h"
/**
 * @file PhysicsSystemQueries.cpp
 * @brief Body/constraint management and spatial queries for PhysicsSystem
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains body creation/removal, all constraint creation methods, raycasting,
 * overlap queries, sweep tests, debug rendering, material management, and
 * metrics retrieval. Uses Jolt Physics API.
 */

#include "PhysicsSystem.h"
#include "Utils/Assert.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/Validate.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
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
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/GearConstraint.h>
#include <Jolt/Physics/Constraints/RackAndPinionConstraint.h>
#include <Jolt/Physics/Constraints/PulleyConstraint.h>
#include <Jolt/Physics/Constraints/PathConstraint.h>
#include <Jolt/Physics/Constraints/PathConstraintPathHermite.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/MotorSettings.h>
#include <Jolt/Physics/Constraints/SpringSettings.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>

JPH_SUPPRESS_WARNINGS

#include <algorithm>
#include <mutex>

using namespace DirectX;

// Legacy global removed — PhysicsBodyImpl now uses EngineContext::Get()->GetPhysics()

// ============================================================================
// BODY CREATION / REMOVAL
// ============================================================================

std::shared_ptr<PhysicsBody> PhysicsSystem::CreateBody(const PhysicsBodyDesc& desc)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
    SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Physics, m_joltSystem, nullptr);

    // PhysicsSystem is registered with EngineContext at engine startup

    // Create collision shape
    void* shapePtr = CreateCollisionShape(desc.shape);
    if (!shapePtr)
        return nullptr;

    auto* shapeRef = static_cast<JPH::ShapeRefC*>(shapePtr);

    // Determine motion type
    JPH::EMotionType motionType = JPH::EMotionType::Dynamic;
    if (desc.type == PhysicsBodyType::Static || (desc.mass <= 0.0f && !desc.isKinematic))
    {
        motionType = JPH::EMotionType::Static;
    }
    else if (desc.isKinematic || desc.type == PhysicsBodyType::Kinematic)
    {
        motionType = JPH::EMotionType::Kinematic;
    }

    // Determine object layer
    JPH::ObjectLayer objectLayer = 1; // MOVING
    if (motionType == JPH::EMotionType::Static)
        objectLayer = 0; // NON_MOVING
    if (desc.isTrigger)
        objectLayer = 2; // TRIGGER

    // Create body settings
    JPH::BodyCreationSettings bodySettings(
        *shapeRef, JPH::RVec3(desc.position.x, desc.position.y, desc.position.z),
        JPH::Quat::sEulerAngles(JPH::Vec3(desc.rotation.x, desc.rotation.y, desc.rotation.z)), motionType, objectLayer);

    // Set material properties
    bodySettings.mFriction = desc.material.friction;
    bodySettings.mRestitution = desc.material.restitution;
    bodySettings.mLinearDamping = desc.material.linearDamping;
    bodySettings.mAngularDamping = desc.material.angularDamping;

    // Set mass for dynamic bodies
    if (motionType == JPH::EMotionType::Dynamic && desc.mass > 0.0f)
    {
        bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        bodySettings.mMassPropertiesOverride.mMass = desc.mass;
    }

    // Set initial velocities
    if (motionType == JPH::EMotionType::Dynamic)
    {
        bodySettings.mLinearVelocity = JPH::Vec3(desc.linearVelocity.x, desc.linearVelocity.y, desc.linearVelocity.z);
        bodySettings.mAngularVelocity =
            JPH::Vec3(desc.angularVelocity.x, desc.angularVelocity.y, desc.angularVelocity.z);
    }

    // Set trigger (sensor) flag
    if (desc.isTrigger)
    {
        bodySettings.mIsSensor = true;
    }

    // Set CCD / motion quality
    if (desc.motionQuality == MotionQuality::LinearCast)
    {
        bodySettings.mMotionQuality = JPH::EMotionQuality::LinearCast;
    }

    // Set allowed degrees of freedom
    bodySettings.mAllowedDOFs = static_cast<JPH::EAllowedDOFs>(static_cast<uint8_t>(desc.allowedDOFs));

    // Set gravity factor
    bodySettings.mGravityFactor = desc.gravityFactor;

    // Set velocity limits
    bodySettings.mMaxLinearVelocity = desc.maxLinearVelocity;
    bodySettings.mMaxAngularVelocity = desc.maxAngularVelocity;

    // Set collision group filtering (GroupFilterTable)
    if (desc.collisionGroupDesc.groupFilterID != 0 &&
        desc.collisionGroupDesc.groupFilterID <= m_groupFilterTables.size())
    {
        auto* tableRef = static_cast<JPH::Ref<JPH::GroupFilterTable>*>(
            m_groupFilterTables[desc.collisionGroupDesc.groupFilterID - 1]);
        bodySettings.mCollisionGroup.SetGroupFilter(tableRef->GetPtr());
        bodySettings.mCollisionGroup.SetGroupID(desc.collisionGroupDesc.groupID);
        bodySettings.mCollisionGroup.SetSubGroupID(desc.collisionGroupDesc.subGroupID);
    }

    // Create the body via Jolt
    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID bodyID = bodyInterface.CreateAndAddBody(bodySettings, JPH::EActivation::Activate);

    if (bodyID.IsInvalid())
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Physics, "Failed to create Jolt body: {}", desc.name);
        return nullptr;
    }

    // Create the PhysicsBody wrapper
    auto body = std::make_shared<PhysicsBody>(desc, bodyID.GetIndexAndSequenceNumber());
    body->SetCollisionGroup(desc.collisionGroup);
    body->SetCollisionMask(desc.collisionMask);

    // Store user data on the Jolt body for collision lookup
    bodyInterface.SetUserData(bodyID, reinterpret_cast<uint64_t>(body.get()));

    m_bodies.push_back(body);
    m_bodyIDMap[bodyID.GetIndexAndSequenceNumber()] = body.get();

    if (!desc.name.empty())
    {
        m_namedBodies[desc.name] = body;
    }

    Spark::SimpleConsole::GetInstance().LogInfo("Created physics body: " + desc.name);
    return body;
}

void PhysicsSystem::RemoveBody(std::shared_ptr<PhysicsBody> body)
{
    if (!body)
        return;

    // Remove from Jolt world
    if (m_joltSystem)
    {
        auto& bodyInterface = m_joltSystem->GetBodyInterface();
        JPH::BodyID bodyID(body->GetJoltBodyID());
        if (bodyInterface.IsAdded(bodyID))
        {
            bodyInterface.RemoveBody(bodyID);
            bodyInterface.DestroyBody(bodyID);
        }
    }

    // Remove from ID map
    m_bodyIDMap.erase(body->GetJoltBodyID());

    // Remove from named bodies
    const std::string& name = body->GetName();
    if (!name.empty())
    {
        m_namedBodies.erase(name);
    }

    // Remove from bodies list
    auto it = std::find(m_bodies.begin(), m_bodies.end(), body);
    if (it != m_bodies.end())
    {
        m_bodies.erase(it);
    }
}

void PhysicsSystem::RemoveAllBodies()
{
    if (m_joltSystem)
    {
        auto& bodyInterface = m_joltSystem->GetBodyInterface();
        for (auto& body : m_bodies)
        {
            if (body)
            {
                JPH::BodyID bodyID(body->GetJoltBodyID());
                if (bodyInterface.IsAdded(bodyID))
                {
                    bodyInterface.RemoveBody(bodyID);
                    bodyInterface.DestroyBody(bodyID);
                }
            }
        }
    }

    m_bodies.clear();
    m_namedBodies.clear();
    m_bodyIDMap.clear();
}

// ============================================================================
// CONSTRAINT CREATION
// ============================================================================

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateHingeConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                        std::shared_ptr<PhysicsBody> bodyB,
                                                                        const XMFLOAT3& pivotA, const XMFLOAT3& pivotB,
                                                                        const XMFLOAT3& axisA, const XMFLOAT3& axisB)
{
    if (!m_joltSystem || !bodyA || !bodyB)
        return nullptr;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID idA(bodyA->GetJoltBodyID());
    JPH::BodyID idB(bodyB->GetJoltBodyID());

    if (!bodyInterface.IsAdded(idA) || !bodyInterface.IsAdded(idB))
        return nullptr;

    JPH::HingeConstraintSettings settings;
    settings.mPoint1 = JPH::RVec3(pivotA.x, pivotA.y, pivotA.z);
    settings.mPoint2 = JPH::RVec3(pivotB.x, pivotB.y, pivotB.z);
    settings.mHingeAxis1 = JPH::Vec3(axisA.x, axisA.y, axisA.z).Normalized();
    settings.mHingeAxis2 = JPH::Vec3(axisB.x, axisB.y, axisB.z).Normalized();
    settings.mNormalAxis1 = settings.mHingeAxis1.GetNormalizedPerpendicular();
    settings.mNormalAxis2 = settings.mHingeAxis2.GetNormalizedPerpendicular();
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;

    JPH::Body* joltBodyA = m_joltSystem->GetBodyLockInterface().TryGetBody(idA);
    JPH::Body* joltBodyB = m_joltSystem->GetBodyLockInterface().TryGetBody(idB);
    if (!joltBodyA || !joltBodyB)
        return nullptr;

    JPH::Constraint* joltConstraint = settings.Create(*joltBodyA, *joltBodyB);
    m_joltSystem->AddConstraint(joltConstraint);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Hinge, joltConstraint);
    m_constraints.push_back(constraint);

    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateSliderConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                         std::shared_ptr<PhysicsBody> bodyB,
                                                                         const XMMATRIX& frameA, const XMMATRIX& frameB)
{
    if (!m_joltSystem || !bodyA || !bodyB)
        return nullptr;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID idA(bodyA->GetJoltBodyID());
    JPH::BodyID idB(bodyB->GetJoltBodyID());

    if (!bodyInterface.IsAdded(idA) || !bodyInterface.IsAdded(idB))
        return nullptr;

    // Extract positions and slide axis from frame matrices
    XMVECTOR scaleA, rotA, transA;
    XMMatrixDecompose(&scaleA, &rotA, &transA, frameA);
    XMFLOAT3 posA;
    XMStoreFloat3(&posA, transA);

    XMVECTOR scaleB, rotB, transB;
    XMMatrixDecompose(&scaleB, &rotB, &transB, frameB);
    XMFLOAT3 posB;
    XMStoreFloat3(&posB, transB);

    JPH::SliderConstraintSettings settings;
    settings.mPoint1 = JPH::RVec3(posA.x, posA.y, posA.z);
    settings.mPoint2 = JPH::RVec3(posB.x, posB.y, posB.z);
    settings.mSliderAxis1 = JPH::Vec3(1, 0, 0); // Default slide along X
    settings.mSliderAxis2 = JPH::Vec3(1, 0, 0);
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;

    JPH::Body* joltBodyA = m_joltSystem->GetBodyLockInterface().TryGetBody(idA);
    JPH::Body* joltBodyB = m_joltSystem->GetBodyLockInterface().TryGetBody(idB);
    if (!joltBodyA || !joltBodyB)
        return nullptr;

    JPH::Constraint* joltConstraint = settings.Create(*joltBodyA, *joltBodyB);
    m_joltSystem->AddConstraint(joltConstraint);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Slider, joltConstraint);
    m_constraints.push_back(constraint);

    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateFixedConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                        std::shared_ptr<PhysicsBody> bodyB,
                                                                        const XMMATRIX& frameA, const XMMATRIX& frameB)
{
    if (!m_joltSystem || !bodyA || !bodyB)
        return nullptr;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID idA(bodyA->GetJoltBodyID());
    JPH::BodyID idB(bodyB->GetJoltBodyID());

    if (!bodyInterface.IsAdded(idA) || !bodyInterface.IsAdded(idB))
        return nullptr;

    XMVECTOR scaleA, rotA, transA;
    XMMatrixDecompose(&scaleA, &rotA, &transA, frameA);
    XMFLOAT3 posA;
    XMStoreFloat3(&posA, transA);

    JPH::FixedConstraintSettings settings;
    settings.mPoint1 = JPH::RVec3(posA.x, posA.y, posA.z);
    settings.mPoint2 = settings.mPoint1;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;

    JPH::Body* joltBodyA = m_joltSystem->GetBodyLockInterface().TryGetBody(idA);
    JPH::Body* joltBodyB = m_joltSystem->GetBodyLockInterface().TryGetBody(idB);
    if (!joltBodyA || !joltBodyB)
        return nullptr;

    JPH::Constraint* joltConstraint = settings.Create(*joltBodyA, *joltBodyB);
    m_joltSystem->AddConstraint(joltConstraint);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Fixed, joltConstraint);
    m_constraints.push_back(constraint);

    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreatePoint2PointConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                              std::shared_ptr<PhysicsBody> bodyB,
                                                                              const XMFLOAT3& pivotA,
                                                                              const XMFLOAT3& pivotB)
{
    if (!m_joltSystem || !bodyA)
        return nullptr;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID idA(bodyA->GetJoltBodyID());

    if (!bodyInterface.IsAdded(idA))
        return nullptr;

    JPH::PointConstraintSettings settings;
    settings.mPoint1 = JPH::RVec3(pivotA.x, pivotA.y, pivotA.z);
    settings.mPoint2 = JPH::RVec3(pivotB.x, pivotB.y, pivotB.z);
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;

    JPH::Body* joltBodyA = m_joltSystem->GetBodyLockInterface().TryGetBody(idA);
    if (!joltBodyA)
        return nullptr;

    JPH::Constraint* joltConstraint = nullptr;
    if (bodyB)
    {
        JPH::BodyID idB(bodyB->GetJoltBodyID());
        if (!bodyInterface.IsAdded(idB))
            return nullptr;
        JPH::Body* joltBodyB = m_joltSystem->GetBodyLockInterface().TryGetBody(idB);
        if (!joltBodyB)
            return nullptr;
        joltConstraint = settings.Create(*joltBodyA, *joltBodyB);
    }
    else
    {
        joltConstraint = settings.Create(*joltBodyA, JPH::Body::sFixedToWorld);
    }

    m_joltSystem->AddConstraint(joltConstraint);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Point2Point, joltConstraint);
    m_constraints.push_back(constraint);

    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateConeTwistConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                            std::shared_ptr<PhysicsBody> bodyB,
                                                                            const XMMATRIX& frameA,
                                                                            const XMMATRIX& frameB, float swingSpan1,
                                                                            float swingSpan2, float twistSpan)
{
    if (!m_joltSystem || !bodyA || !bodyB)
        return nullptr;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID idA(bodyA->GetJoltBodyID());
    JPH::BodyID idB(bodyB->GetJoltBodyID());

    if (!bodyInterface.IsAdded(idA) || !bodyInterface.IsAdded(idB))
        return nullptr;

    XMVECTOR scaleA, rotA, transA;
    XMMatrixDecompose(&scaleA, &rotA, &transA, frameA);
    XMFLOAT3 posA;
    XMStoreFloat3(&posA, transA);

    XMVECTOR scaleB, rotB, transB;
    XMMatrixDecompose(&scaleB, &rotB, &transB, frameB);
    XMFLOAT3 posB;
    XMStoreFloat3(&posB, transB);

    JPH::SwingTwistConstraintSettings settings;
    settings.mPosition1 = JPH::RVec3(posA.x, posA.y, posA.z);
    settings.mPosition2 = JPH::RVec3(posB.x, posB.y, posB.z);
    settings.mTwistAxis1 = JPH::Vec3(0, 0, 1);
    settings.mTwistAxis2 = JPH::Vec3(0, 0, 1);
    settings.mPlaneAxis1 = JPH::Vec3(1, 0, 0);
    settings.mPlaneAxis2 = JPH::Vec3(1, 0, 0);
    settings.mNormalHalfConeAngle = swingSpan1;
    settings.mPlaneHalfConeAngle = swingSpan2;
    settings.mTwistMinAngle = -twistSpan;
    settings.mTwistMaxAngle = twistSpan;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;

    JPH::Body* joltBodyA = m_joltSystem->GetBodyLockInterface().TryGetBody(idA);
    JPH::Body* joltBodyB = m_joltSystem->GetBodyLockInterface().TryGetBody(idB);
    if (!joltBodyA || !joltBodyB)
        return nullptr;

    JPH::Constraint* joltConstraint = settings.Create(*joltBodyA, *joltBodyB);
    m_joltSystem->AddConstraint(joltConstraint);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::ConeTwist, joltConstraint);
    m_constraints.push_back(constraint);

    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateDistanceConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                           std::shared_ptr<PhysicsBody> bodyB,
                                                                           const XMFLOAT3& pivotA,
                                                                           const XMFLOAT3& pivotB, float minDistance,
                                                                           float maxDistance)
{
    if (!m_joltSystem || !bodyA || !bodyB)
        return nullptr;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID idA(bodyA->GetJoltBodyID());
    JPH::BodyID idB(bodyB->GetJoltBodyID());

    if (!bodyInterface.IsAdded(idA) || !bodyInterface.IsAdded(idB))
        return nullptr;

    JPH::DistanceConstraintSettings settings;
    settings.mPoint1 = JPH::RVec3(pivotA.x, pivotA.y, pivotA.z);
    settings.mPoint2 = JPH::RVec3(pivotB.x, pivotB.y, pivotB.z);
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
    if (minDistance >= 0.0f)
        settings.mMinDistance = minDistance;
    if (maxDistance >= 0.0f)
        settings.mMaxDistance = maxDistance;

    JPH::Body* joltBodyA = m_joltSystem->GetBodyLockInterface().TryGetBody(idA);
    JPH::Body* joltBodyB = m_joltSystem->GetBodyLockInterface().TryGetBody(idB);
    if (!joltBodyA || !joltBodyB)
        return nullptr;

    JPH::Constraint* joltConstraint = settings.Create(*joltBodyA, *joltBodyB);
    m_joltSystem->AddConstraint(joltConstraint);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Distance, joltConstraint);
    m_constraints.push_back(constraint);
    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateConeConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                       std::shared_ptr<PhysicsBody> bodyB,
                                                                       const XMFLOAT3& pivot, const XMFLOAT3& twistAxis,
                                                                       float halfConeAngle)
{
    if (!m_joltSystem || !bodyA || !bodyB)
        return nullptr;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID idA(bodyA->GetJoltBodyID());
    JPH::BodyID idB(bodyB->GetJoltBodyID());

    if (!bodyInterface.IsAdded(idA) || !bodyInterface.IsAdded(idB))
        return nullptr;

    JPH::ConeConstraintSettings settings;
    settings.mPoint1 = JPH::RVec3(pivot.x, pivot.y, pivot.z);
    settings.mPoint2 = settings.mPoint1;
    settings.mTwistAxis1 = JPH::Vec3(twistAxis.x, twistAxis.y, twistAxis.z).Normalized();
    settings.mTwistAxis2 = settings.mTwistAxis1;
    settings.mHalfConeAngle = halfConeAngle;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;

    JPH::Body* joltBodyA = m_joltSystem->GetBodyLockInterface().TryGetBody(idA);
    JPH::Body* joltBodyB = m_joltSystem->GetBodyLockInterface().TryGetBody(idB);
    if (!joltBodyA || !joltBodyB)
        return nullptr;

    JPH::Constraint* joltConstraint = settings.Create(*joltBodyA, *joltBodyB);
    m_joltSystem->AddConstraint(joltConstraint);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Cone, joltConstraint);
    m_constraints.push_back(constraint);
    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateSixDOFConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                         std::shared_ptr<PhysicsBody> bodyB,
                                                                         const XMMATRIX& frameA, const XMMATRIX& frameB)
{
    if (!m_joltSystem || !bodyA || !bodyB)
        return nullptr;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID idA(bodyA->GetJoltBodyID());
    JPH::BodyID idB(bodyB->GetJoltBodyID());

    if (!bodyInterface.IsAdded(idA) || !bodyInterface.IsAdded(idB))
        return nullptr;

    XMVECTOR scaleA, rotA, transA;
    XMMatrixDecompose(&scaleA, &rotA, &transA, frameA);
    XMFLOAT3 posA;
    XMStoreFloat3(&posA, transA);
    XMFLOAT4 quatA;
    XMStoreFloat4(&quatA, rotA);

    XMVECTOR scaleB, rotB, transB;
    XMMatrixDecompose(&scaleB, &rotB, &transB, frameB);
    XMFLOAT3 posB;
    XMStoreFloat3(&posB, transB);
    XMFLOAT4 quatB;
    XMStoreFloat4(&quatB, rotB);

    JPH::SixDOFConstraintSettings settings;
    settings.mPosition1 = JPH::RVec3(posA.x, posA.y, posA.z);
    settings.mPosition2 = JPH::RVec3(posB.x, posB.y, posB.z);
    settings.mAxisX1 = JPH::Vec3(1, 0, 0);
    settings.mAxisY1 = JPH::Vec3(0, 1, 0);
    settings.mAxisX2 = JPH::Vec3(1, 0, 0);
    settings.mAxisY2 = JPH::Vec3(0, 1, 0);
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;

    JPH::Body* joltBodyA = m_joltSystem->GetBodyLockInterface().TryGetBody(idA);
    JPH::Body* joltBodyB = m_joltSystem->GetBodyLockInterface().TryGetBody(idB);
    if (!joltBodyA || !joltBodyB)
        return nullptr;

    JPH::Constraint* joltConstraint = settings.Create(*joltBodyA, *joltBodyB);
    m_joltSystem->AddConstraint(joltConstraint);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Generic6DOF, joltConstraint);
    m_constraints.push_back(constraint);
    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreatePulleyConstraint(
    std::shared_ptr<PhysicsBody> bodyA, std::shared_ptr<PhysicsBody> bodyB, const XMFLOAT3& fixedPointA,
    const XMFLOAT3& fixedPointB, const XMFLOAT3& bodyPointA, const XMFLOAT3& bodyPointB, float ratio)
{
    if (!m_joltSystem || !bodyA || !bodyB)
        return nullptr;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID idA(bodyA->GetJoltBodyID());
    JPH::BodyID idB(bodyB->GetJoltBodyID());

    if (!bodyInterface.IsAdded(idA) || !bodyInterface.IsAdded(idB))
        return nullptr;

    JPH::PulleyConstraintSettings settings;
    settings.mBodyPoint1 = JPH::RVec3(bodyPointA.x, bodyPointA.y, bodyPointA.z);
    settings.mBodyPoint2 = JPH::RVec3(bodyPointB.x, bodyPointB.y, bodyPointB.z);
    settings.mFixedPoint1 = JPH::RVec3(fixedPointA.x, fixedPointA.y, fixedPointA.z);
    settings.mFixedPoint2 = JPH::RVec3(fixedPointB.x, fixedPointB.y, fixedPointB.z);
    settings.mRatio = ratio;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;

    JPH::Body* joltBodyA = m_joltSystem->GetBodyLockInterface().TryGetBody(idA);
    JPH::Body* joltBodyB = m_joltSystem->GetBodyLockInterface().TryGetBody(idB);
    if (!joltBodyA || !joltBodyB)
        return nullptr;

    JPH::Constraint* joltConstraint = settings.Create(*joltBodyA, *joltBodyB);
    m_joltSystem->AddConstraint(joltConstraint);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Pulley, joltConstraint);
    m_constraints.push_back(constraint);
    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateGearConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                       std::shared_ptr<PhysicsBody> bodyB,
                                                                       std::shared_ptr<PhysicsConstraint> hingeA,
                                                                       std::shared_ptr<PhysicsConstraint> hingeB,
                                                                       float ratio)
{
    if (!m_joltSystem || !bodyA || !bodyB || !hingeA || !hingeB)
        return nullptr;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID idA(bodyA->GetJoltBodyID());
    JPH::BodyID idB(bodyB->GetJoltBodyID());
    if (!bodyInterface.IsAdded(idA) || !bodyInterface.IsAdded(idB))
        return nullptr;

    JPH::Body* joltBodyA = m_joltSystem->GetBodyLockInterface().TryGetBody(idA);
    JPH::Body* joltBodyB = m_joltSystem->GetBodyLockInterface().TryGetBody(idB);
    if (!joltBodyA || !joltBodyB)
        return nullptr;

    auto* joltHingeA = static_cast<JPH::HingeConstraint*>(hingeA->GetJoltConstraint());
    auto* joltHingeB = static_cast<JPH::HingeConstraint*>(hingeB->GetJoltConstraint());

    JPH::GearConstraintSettings settings;
    settings.mRatio = ratio;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;

    JPH::Constraint* joltConstraint = settings.Create(*joltBodyA, *joltBodyB);
    static_cast<JPH::GearConstraint*>(joltConstraint)->SetConstraints(joltHingeA, joltHingeB);
    m_joltSystem->AddConstraint(joltConstraint);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Gear, joltConstraint);
    m_constraints.push_back(constraint);
    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateRackAndPinionConstraint(
    std::shared_ptr<PhysicsBody> bodyA, std::shared_ptr<PhysicsBody> bodyB, std::shared_ptr<PhysicsConstraint> slider,
    std::shared_ptr<PhysicsConstraint> hinge, float ratio)
{
    if (!m_joltSystem || !bodyA || !bodyB || !slider || !hinge)
        return nullptr;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID idA(bodyA->GetJoltBodyID());
    JPH::BodyID idB(bodyB->GetJoltBodyID());
    if (!bodyInterface.IsAdded(idA) || !bodyInterface.IsAdded(idB))
        return nullptr;

    JPH::Body* joltBodyA = m_joltSystem->GetBodyLockInterface().TryGetBody(idA);
    JPH::Body* joltBodyB = m_joltSystem->GetBodyLockInterface().TryGetBody(idB);
    if (!joltBodyA || !joltBodyB)
        return nullptr;

    JPH::RackAndPinionConstraintSettings settings;
    settings.mRatio = ratio;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;

    JPH::Constraint* joltConstraint = settings.Create(*joltBodyA, *joltBodyB);
    static_cast<JPH::RackAndPinionConstraint*>(joltConstraint)
        ->SetConstraints(static_cast<JPH::SliderConstraint*>(slider->GetJoltConstraint()),
                         static_cast<JPH::HingeConstraint*>(hinge->GetJoltConstraint()));
    m_joltSystem->AddConstraint(joltConstraint);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::RackAndPinion, joltConstraint);
    m_constraints.push_back(constraint);
    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreatePathConstraint(std::shared_ptr<PhysicsBody> body,
                                                                       const std::vector<XMFLOAT3>& pathPoints,
                                                                       const std::vector<XMFLOAT3>& pathTangents)
{
    if (!m_joltSystem || !body || pathPoints.size() < 2)
        return nullptr;
    if (pathPoints.size() != pathTangents.size())
        return nullptr;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID bodyID(body->GetJoltBodyID());
    if (!bodyInterface.IsAdded(bodyID))
        return nullptr;

    // Build Hermite spline path
    auto* path = new JPH::PathConstraintPathHermite;
    for (size_t i = 0; i < pathPoints.size(); i++)
    {
        const auto& p = pathPoints[i];
        const auto& t = pathTangents[i];
        path->AddPoint(JPH::Vec3(p.x, p.y, p.z), JPH::Vec3(t.x, t.y, t.z), JPH::Vec3(0, 1, 0));
    }

    JPH::PathConstraintSettings settings;
    settings.mPath = path;
    settings.mPathPosition = JPH::Vec3::sZero();
    settings.mPathRotation = JPH::Quat::sIdentity();

    JPH::Body* joltBody = m_joltSystem->GetBodyLockInterface().TryGetBody(bodyID);
    if (!joltBody)
        return nullptr;

    JPH::Constraint* joltConstraint = settings.Create(*joltBody, JPH::Body::sFixedToWorld);
    m_joltSystem->AddConstraint(joltConstraint);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Path, joltConstraint);
    m_constraints.push_back(constraint);
    return constraint;
}

// ============================================================================
// CONSTRAINT MOTORS
// ============================================================================

void PhysicsSystem::SetHingeMotorVelocity(std::shared_ptr<PhysicsConstraint> constraint, float targetVelocity,
                                          float maxTorque)
{
    if (!constraint || !constraint->GetJoltConstraint())
        return;
    auto* hinge = static_cast<JPH::HingeConstraint*>(constraint->GetJoltConstraint());
    hinge->GetMotorSettings().mMaxTorqueLimit = maxTorque;
    hinge->GetMotorSettings().mMinTorqueLimit = -maxTorque;
    hinge->SetMotorState(JPH::EMotorState::Velocity);
    hinge->SetTargetAngularVelocity(targetVelocity);
}

void PhysicsSystem::SetHingeMotorPosition(std::shared_ptr<PhysicsConstraint> constraint, float targetAngle,
                                          float maxTorque)
{
    if (!constraint || !constraint->GetJoltConstraint())
        return;
    auto* hinge = static_cast<JPH::HingeConstraint*>(constraint->GetJoltConstraint());
    hinge->GetMotorSettings().mMaxTorqueLimit = maxTorque;
    hinge->GetMotorSettings().mMinTorqueLimit = -maxTorque;
    hinge->SetMotorState(JPH::EMotorState::Position);
    hinge->SetTargetAngle(targetAngle);
}

void PhysicsSystem::SetSliderMotorVelocity(std::shared_ptr<PhysicsConstraint> constraint, float targetVelocity,
                                           float maxForce)
{
    if (!constraint || !constraint->GetJoltConstraint())
        return;
    auto* slider = static_cast<JPH::SliderConstraint*>(constraint->GetJoltConstraint());
    slider->GetMotorSettings().mMaxForceLimit = maxForce;
    slider->GetMotorSettings().mMinForceLimit = -maxForce;
    slider->SetMotorState(JPH::EMotorState::Velocity);
    slider->SetTargetVelocity(targetVelocity);
}

void PhysicsSystem::SetSliderMotorPosition(std::shared_ptr<PhysicsConstraint> constraint, float targetPosition,
                                           float maxForce)
{
    if (!constraint || !constraint->GetJoltConstraint())
        return;
    auto* slider = static_cast<JPH::SliderConstraint*>(constraint->GetJoltConstraint());
    slider->GetMotorSettings().mMaxForceLimit = maxForce;
    slider->GetMotorSettings().mMinForceLimit = -maxForce;
    slider->SetMotorState(JPH::EMotorState::Position);
    slider->SetTargetPosition(targetPosition);
}

void PhysicsSystem::DisableConstraintMotor(std::shared_ptr<PhysicsConstraint> constraint)
{
    if (!constraint || !constraint->GetJoltConstraint())
        return;

    switch (constraint->GetType())
    {
    case ConstraintType::Hinge:
        static_cast<JPH::HingeConstraint*>(constraint->GetJoltConstraint())->SetMotorState(JPH::EMotorState::Off);
        break;
    case ConstraintType::Slider:
        static_cast<JPH::SliderConstraint*>(constraint->GetJoltConstraint())->SetMotorState(JPH::EMotorState::Off);
        break;
    default:
        break;
    }
}

void PhysicsSystem::RemoveConstraint(std::shared_ptr<PhysicsConstraint> constraint)
{
    if (!constraint)
        return;

    if (m_joltSystem && constraint->GetJoltConstraint())
    {
        m_joltSystem->RemoveConstraint(constraint->GetJoltConstraint());
    }

    auto it = std::find(m_constraints.begin(), m_constraints.end(), constraint);
    if (it != m_constraints.end())
    {
        m_constraints.erase(it);
    }
}

// ============================================================================
// CHARACTER CONTROLLER
// ============================================================================

std::unique_ptr<CharacterController> PhysicsSystem::CreateCharacterController(const CharacterControllerDesc& desc)
{
    return std::make_unique<CharacterController>(this, desc);
}

std::unique_ptr<VehiclePhysics> PhysicsSystem::CreateVehicle(std::shared_ptr<PhysicsBody> body, const VehicleDesc& desc)
{
    return std::make_unique<VehiclePhysics>(this, body, desc);
}

std::unique_ptr<Ragdoll> PhysicsSystem::CreateRagdoll(const RagdollDesc& desc)
{
    return std::make_unique<Ragdoll>(this, desc);
}

std::unique_ptr<SoftBody> PhysicsSystem::CreateSoftBody(const SoftBodyDesc& desc)
{
    return std::make_unique<SoftBody>(this, desc);
}

void PhysicsSystem::SetSurfaceVelocity(std::shared_ptr<PhysicsBody> body, const XMFLOAT3& velocity)
{
    if (!body)
        return;

    uint32_t bodyID = body->GetJoltBodyID();

    // Store surface velocity for lookup by the contact listener.
    // The SparkContactListener reads this map during OnContactAdded/OnContactPersisted
    // and applies it to ContactSettings::mRelativeLinearSurfaceVelocity.
    std::lock_guard<std::mutex> lock(m_surfaceVelocityMutex);

    if (velocity.x == 0.0f && velocity.y == 0.0f && velocity.z == 0.0f)
    {
        m_surfaceVelocities.erase(bodyID);
    }
    else
    {
        m_surfaceVelocities[bodyID] = velocity;
    }
}

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
            if (joltBody)
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
                if (std::find(results.begin(), results.end(), body) == results.end())
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
                if (std::find(results.begin(), results.end(), body) == results.end())
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
    JPH::SphereShape sphereShape(radius);
    return PerformShapeCast(m_joltSystem.get(), &sphereShape, from, to);
}

RaycastHit PhysicsSystem::BoxCast(const XMFLOAT3& halfExtents, const XMFLOAT3& from, const XMFLOAT3& to)
{
    JPH::BoxShape boxShape(JPH::Vec3(halfExtents.x, halfExtents.y, halfExtents.z));
    return PerformShapeCast(m_joltSystem.get(), &boxShape, from, to);
}

RaycastHit PhysicsSystem::CapsuleCast(float radius, float height, const XMFLOAT3& from, const XMFLOAT3& to)
{
    JPH::CapsuleShape capsuleShape(height / 2.0f, radius);
    return PerformShapeCast(m_joltSystem.get(), &capsuleShape, from, to);
}

// ============================================================================
// DEBUG RENDERING
// ============================================================================

void PhysicsSystem::SetDebugDrawMode(int mode)
{
    m_debugDrawEnabled = (mode != 0);
}

void PhysicsSystem::RenderDebug()
{
    if (!m_joltSystem || !m_debugDrawEnabled)
        return;

    // Iterate all bodies and collect debug visualization data.
    // The PhysicsDebugRenderer data collector (separate class) can be used by
    // the graphics engine to render wireframes, AABBs, velocity vectors, etc.
    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    for (const auto& body : m_bodies)
    {
        if (!body)
            continue;
        JPH::BodyID bodyID(body->GetJoltBodyID());
        if (!bodyInterface.IsAdded(bodyID))
            continue;

        // Get body world-space bounds via TransformedShape
        JPH::TransformedShape ts = bodyInterface.GetTransformedShape(bodyID);
        JPH::AABox aabb = ts.GetWorldSpaceBounds();
        bool isActive = bodyInterface.IsActive(bodyID);

        // Update metrics with debug info
        if (isActive)
        {
            std::lock_guard<std::mutex> lock(m_metricsMutex);
            m_metrics.activeRigidBodies++;
        }

        (void)aabb; // Consumed by the debug renderer when wired to graphics
    }
}

// ============================================================================
// MATERIAL MANAGEMENT
// ============================================================================

void PhysicsSystem::RegisterMaterial(const std::string& name, const PhysicsMaterial& material)
{
    m_materials[name] = material;
}

const PhysicsMaterial* PhysicsSystem::GetMaterial(const std::string& name) const
{
    auto it = m_materials.find(name);
    return (it != m_materials.end()) ? &it->second : nullptr;
}

// ============================================================================
// METRICS
// ============================================================================

PhysicsSystem::PhysicsMetrics PhysicsSystem::GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

// ============================================================================
// BUOYANCY
// ============================================================================

void PhysicsSystem::ApplyBuoyancy(std::shared_ptr<PhysicsBody> body, const XMFLOAT3& waterSurface,
                                  const XMFLOAT3& waterNormal, float waterDensity, float linearDrag, float angularDrag)
{
    if (!m_joltSystem || !body)
        return;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID bodyID(body->GetJoltBodyID());
    if (!bodyInterface.IsAdded(bodyID))
        return;

    JPH::Body* joltBody = const_cast<JPH::Body*>(m_joltSystem->GetBodyLockInterface().TryGetBody(bodyID));
    if (!joltBody || !joltBody->IsDynamic())
        return;

    // Use Jolt's built-in buoyancy application
    JPH::Vec3 gravity = m_joltSystem->GetGravity();
    JPH::RVec3 surfacePos(waterSurface.x, waterSurface.y, waterSurface.z);
    JPH::Vec3 surfaceNorm(waterNormal.x, waterNormal.y, waterNormal.z);

    // ApplyBuoyancyImpulse(surfacePosition, surfaceNormal, buoyancy, linearDrag, angularDrag, fluidVelocity, gravity,
    // dt)
    joltBody->ApplyBuoyancyImpulse(surfacePos, surfaceNorm, waterDensity, linearDrag, angularDrag, JPH::Vec3::sZero(),
                                   gravity, m_timeStep);
}

// ============================================================================
// STATE SERIALIZATION
// ============================================================================

bool PhysicsSystem::SaveState(std::vector<uint8_t>& outBuffer) const
{
    if (!m_joltSystem)
        return false;

    // Serialize body positions, rotations, velocities, and activation state
    outBuffer.clear();

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    uint32_t bodyCount = static_cast<uint32_t>(m_bodies.size());

    // Reserve header + per-body data
    // Header: [bodyCount:4]
    // Per body: [joltID:4][posX:4][posY:4][posZ:4][rotX:4][rotY:4][rotZ:4][rotW:4]
    //           [linVelX:4][linVelY:4][linVelZ:4][angVelX:4][angVelY:4][angVelZ:4][active:1]
    size_t perBodySize = 4 + 3 * 4 + 4 * 4 + 3 * 4 + 3 * 4 + 1; // 57 bytes
    outBuffer.resize(4 + bodyCount * perBodySize);

    uint8_t* ptr = outBuffer.data();
    std::memcpy(ptr, &bodyCount, 4);
    ptr += 4;

    for (const auto& body : m_bodies)
    {
        if (!body)
            continue;

        uint32_t id = body->GetJoltBodyID();
        std::memcpy(ptr, &id, 4);
        ptr += 4;

        JPH::BodyID bodyID(id);
        if (bodyInterface.IsAdded(bodyID))
        {
            JPH::RVec3 pos = bodyInterface.GetPosition(bodyID);
            float px = static_cast<float>(pos.GetX()), py = static_cast<float>(pos.GetY()),
                  pz = static_cast<float>(pos.GetZ());
            std::memcpy(ptr, &px, 4);
            ptr += 4;
            std::memcpy(ptr, &py, 4);
            ptr += 4;
            std::memcpy(ptr, &pz, 4);
            ptr += 4;

            JPH::Quat rot = bodyInterface.GetRotation(bodyID);
            float rx = rot.GetX(), ry = rot.GetY(), rz = rot.GetZ(), rw = rot.GetW();
            std::memcpy(ptr, &rx, 4);
            ptr += 4;
            std::memcpy(ptr, &ry, 4);
            ptr += 4;
            std::memcpy(ptr, &rz, 4);
            ptr += 4;
            std::memcpy(ptr, &rw, 4);
            ptr += 4;

            JPH::Vec3 linVel = bodyInterface.GetLinearVelocity(bodyID);
            float lvx = linVel.GetX(), lvy = linVel.GetY(), lvz = linVel.GetZ();
            std::memcpy(ptr, &lvx, 4);
            ptr += 4;
            std::memcpy(ptr, &lvy, 4);
            ptr += 4;
            std::memcpy(ptr, &lvz, 4);
            ptr += 4;

            JPH::Vec3 angVel = bodyInterface.GetAngularVelocity(bodyID);
            float avx = angVel.GetX(), avy = angVel.GetY(), avz = angVel.GetZ();
            std::memcpy(ptr, &avx, 4);
            ptr += 4;
            std::memcpy(ptr, &avy, 4);
            ptr += 4;
            std::memcpy(ptr, &avz, 4);
            ptr += 4;

            uint8_t active = bodyInterface.IsActive(bodyID) ? 1 : 0;
            *ptr++ = active;
        }
        else
        {
            std::memset(ptr, 0, perBodySize - 4);
            ptr += perBodySize - 4;
        }
    }

    return true;
}

bool PhysicsSystem::LoadState(const std::vector<uint8_t>& buffer)
{
    if (!m_joltSystem || buffer.size() < 4)
        return false;

    const uint8_t* ptr = buffer.data();
    uint32_t bodyCount = 0;
    std::memcpy(&bodyCount, ptr, 4);
    ptr += 4;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    size_t perBodySize = 4 + 3 * 4 + 4 * 4 + 3 * 4 + 3 * 4 + 1;

    for (uint32_t i = 0; i < bodyCount; i++)
    {
        if (static_cast<size_t>(ptr - buffer.data()) + perBodySize > buffer.size())
            break;

        uint32_t id;
        std::memcpy(&id, ptr, 4);
        ptr += 4;

        float px, py, pz;
        std::memcpy(&px, ptr, 4);
        ptr += 4;
        std::memcpy(&py, ptr, 4);
        ptr += 4;
        std::memcpy(&pz, ptr, 4);
        ptr += 4;

        float rx, ry, rz, rw;
        std::memcpy(&rx, ptr, 4);
        ptr += 4;
        std::memcpy(&ry, ptr, 4);
        ptr += 4;
        std::memcpy(&rz, ptr, 4);
        ptr += 4;
        std::memcpy(&rw, ptr, 4);
        ptr += 4;

        float lvx, lvy, lvz;
        std::memcpy(&lvx, ptr, 4);
        ptr += 4;
        std::memcpy(&lvy, ptr, 4);
        ptr += 4;
        std::memcpy(&lvz, ptr, 4);
        ptr += 4;

        float avx, avy, avz;
        std::memcpy(&avx, ptr, 4);
        ptr += 4;
        std::memcpy(&avy, ptr, 4);
        ptr += 4;
        std::memcpy(&avz, ptr, 4);
        ptr += 4;

        uint8_t active = *ptr++;

        // Find matching body and restore state
        JPH::BodyID bodyID(id);
        if (bodyInterface.IsAdded(bodyID))
        {
            bodyInterface.SetPositionAndRotation(bodyID, JPH::RVec3(px, py, pz), JPH::Quat(rx, ry, rz, rw),
                                                 active ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
            bodyInterface.SetLinearVelocity(bodyID, JPH::Vec3(lvx, lvy, lvz));
            bodyInterface.SetAngularVelocity(bodyID, JPH::Vec3(avx, avy, avz));
        }
    }

    return true;
}

// ============================================================================
// GROUP FILTER TABLE
// ============================================================================

uint32_t PhysicsSystem::CreateGroupFilterTable(uint32_t numSubGroups)
{
    auto* tableRef = new JPH::Ref<JPH::GroupFilterTable>(new JPH::GroupFilterTable(numSubGroups));
    m_groupFilterTables.push_back(tableRef);
    return static_cast<uint32_t>(m_groupFilterTables.size()); // 1-based ID
}

void PhysicsSystem::DisableGroupCollision(uint32_t filterID, uint32_t subGroup1, uint32_t subGroup2)
{
    if (filterID == 0 || filterID > m_groupFilterTables.size())
        return;
    auto* tableRef = static_cast<JPH::Ref<JPH::GroupFilterTable>*>(m_groupFilterTables[filterID - 1]);
    (*tableRef)->DisableCollision(subGroup1, subGroup2);
}

void PhysicsSystem::EnableGroupCollision(uint32_t filterID, uint32_t subGroup1, uint32_t subGroup2)
{
    if (filterID == 0 || filterID > m_groupFilterTables.size())
        return;
    auto* tableRef = static_cast<JPH::Ref<JPH::GroupFilterTable>*>(m_groupFilterTables[filterID - 1]);
    (*tableRef)->EnableCollision(subGroup1, subGroup2);
}

// ============================================================================
// MUTABLE COMPOUND SHAPE
// ============================================================================

std::shared_ptr<PhysicsBody> PhysicsSystem::CreateMutableCompoundBody(const PhysicsBodyDesc& desc,
                                                                      const std::vector<MutableSubShapeDesc>& subShapes)
{
    if (!m_joltSystem)
        return nullptr;

    // Build the MutableCompoundShape from sub-shapes
    JPH::MutableCompoundShapeSettings compoundSettings;
    for (const auto& sub : subShapes)
    {
        void* shapePtr = CreateCollisionShape(sub.shape);
        if (!shapePtr)
            continue;
        auto* shapeRef = static_cast<JPH::ShapeRefC*>(shapePtr);

        float pitch = sub.rotation.x * (3.14159265f / 180.0f);
        float yaw = sub.rotation.y * (3.14159265f / 180.0f);
        float roll = sub.rotation.z * (3.14159265f / 180.0f);
        JPH::Quat rot = JPH::Quat::sEulerAngles(JPH::Vec3(pitch, yaw, roll));

        compoundSettings.AddShape(JPH::Vec3(sub.position.x, sub.position.y, sub.position.z), rot, shapeRef->GetPtr());
    }

    auto compoundResult = compoundSettings.Create();
    if (compoundResult.HasError())
        return nullptr;

    // Determine body layer and motion type
    JPH::EMotionType motionType = JPH::EMotionType::Dynamic;
    JPH::ObjectLayer layer = 1; // MOVING
    if (desc.type == PhysicsBodyType::Static)
    {
        motionType = JPH::EMotionType::Static;
        layer = 0; // NON_MOVING
    }
    else if (desc.type == PhysicsBodyType::Kinematic || desc.isKinematic)
    {
        motionType = JPH::EMotionType::Kinematic;
    }

    float pitch = desc.rotation.x * (3.14159265f / 180.0f);
    float yaw = desc.rotation.y * (3.14159265f / 180.0f);
    float roll = desc.rotation.z * (3.14159265f / 180.0f);

    JPH::BodyCreationSettings bodySettings(compoundResult.Get(),
                                           JPH::RVec3(desc.position.x, desc.position.y, desc.position.z),
                                           JPH::Quat::sEulerAngles(JPH::Vec3(pitch, yaw, roll)), motionType, layer);
    if (motionType == JPH::EMotionType::Dynamic)
    {
        bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        bodySettings.mMassPropertiesOverride.mMass = desc.mass;
    }

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID bodyID = bodyInterface.CreateAndAddBody(bodySettings, JPH::EActivation::Activate);

    auto body = std::make_shared<PhysicsBody>(desc, bodyID.GetIndexAndSequenceNumber());

    m_bodies.push_back(body);
    m_bodyIDMap[bodyID.GetIndexAndSequenceNumber()] = body.get();
    if (!desc.name.empty())
        m_namedBodies[desc.name] = body;

    return body;
}

uint32_t PhysicsSystem::AddSubShape(std::shared_ptr<PhysicsBody> body, const MutableSubShapeDesc& subShape)
{
    if (!m_joltSystem || !body)
        return UINT32_MAX;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID bodyID(body->GetJoltBodyID());
    if (!bodyInterface.IsAdded(bodyID))
        return UINT32_MAX;

    JPH::Body* joltBody = const_cast<JPH::Body*>(m_joltSystem->GetBodyLockInterface().TryGetBody(bodyID));
    if (!joltBody)
        return UINT32_MAX;

    auto* compound =
        const_cast<JPH::MutableCompoundShape*>(static_cast<const JPH::MutableCompoundShape*>(joltBody->GetShape()));
    if (!compound)
        return UINT32_MAX;

    void* shapePtr = CreateCollisionShape(subShape.shape);
    if (!shapePtr)
        return UINT32_MAX;

    auto* shapeRef = static_cast<JPH::ShapeRefC*>(shapePtr);

    float pitch = subShape.rotation.x * (3.14159265f / 180.0f);
    float yaw = subShape.rotation.y * (3.14159265f / 180.0f);
    float roll = subShape.rotation.z * (3.14159265f / 180.0f);
    JPH::Quat rot = JPH::Quat::sEulerAngles(JPH::Vec3(pitch, yaw, roll));

    uint32_t index = compound->AddShape(JPH::Vec3(subShape.position.x, subShape.position.y, subShape.position.z), rot,
                                        shapeRef->GetPtr(), subShape.userData);

    compound->AdjustCenterOfMass();
    bodyInterface.NotifyShapeChanged(bodyID, JPH::Vec3::sZero(), true, JPH::EActivation::Activate);

    return index;
}

void PhysicsSystem::RemoveSubShape(std::shared_ptr<PhysicsBody> body, uint32_t index)
{
    if (!m_joltSystem || !body)
        return;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID bodyID(body->GetJoltBodyID());
    if (!bodyInterface.IsAdded(bodyID))
        return;

    JPH::Body* joltBody = const_cast<JPH::Body*>(m_joltSystem->GetBodyLockInterface().TryGetBody(bodyID));
    if (!joltBody)
        return;

    auto* compound =
        const_cast<JPH::MutableCompoundShape*>(static_cast<const JPH::MutableCompoundShape*>(joltBody->GetShape()));
    if (!compound)
        return;

    compound->RemoveShape(index);
    compound->AdjustCenterOfMass();
    bodyInterface.NotifyShapeChanged(bodyID, JPH::Vec3::sZero(), true, JPH::EActivation::Activate);
}

// ============================================================================
// OFFSET CENTER OF MASS
// ============================================================================

void PhysicsSystem::SetCenterOfMassOffset(std::shared_ptr<PhysicsBody> body, const XMFLOAT3& offset)
{
    if (!m_joltSystem || !body)
        return;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID bodyID(body->GetJoltBodyID());
    if (!bodyInterface.IsAdded(bodyID))
        return;

    // Get current shape and wrap it with OffsetCenterOfMassShape
    JPH::RefConst<JPH::Shape> currentShape = bodyInterface.GetShape(bodyID);

    JPH::OffsetCenterOfMassShapeSettings settings(JPH::Vec3(offset.x, offset.y, offset.z), currentShape.GetPtr());
    auto result = settings.Create();
    if (result.HasError())
        return;

    bodyInterface.SetShape(bodyID, result.Get(), true, JPH::EActivation::Activate);
}
