#include "../Core/Platform.h"
/**
 * @file PhysicsConstraints.cpp
 * @brief Constraint creation and removal for PhysicsSystem
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains all constraint creation methods (hinge, slider, fixed, point-to-point,
 * cone-twist, distance, cone, 6DOF, pulley, gear, rack-and-pinion, path) and
 * constraint removal. Uses Jolt Physics API.
 */

#include "PhysicsSystem.h"
#include "../Utils/LogMacros.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyInterface.h>
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

JPH_SUPPRESS_WARNINGS
#include "JoltWarningRestore.h"

#include <algorithm>

using namespace DirectX;

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

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateFixedConstraint(
    std::shared_ptr<PhysicsBody> bodyA, std::shared_ptr<PhysicsBody> bodyB, const XMMATRIX& frameA,
    [[maybe_unused]] const XMMATRIX&
        frameB) // Intentional: Jolt fixed constraint uses point1==point2; frameB reserved for future axis-aligned constraints
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
    if (!joltHingeA || !joltHingeB)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Physics, "GearConstraint: Failed to get hinge constraints");
        return nullptr;
    }

    JPH::GearConstraintSettings settings;
    settings.mRatio = ratio;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;

    JPH::Constraint* joltConstraint = settings.Create(*joltBodyA, *joltBodyB);
    if (!joltConstraint)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Physics, "GearConstraint: Failed to create constraint");
        return nullptr;
    }
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

    auto* joltSlider = static_cast<JPH::SliderConstraint*>(slider->GetJoltConstraint());
    auto* joltHinge = static_cast<JPH::HingeConstraint*>(hinge->GetJoltConstraint());
    if (!joltSlider || !joltHinge)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Physics, "RackAndPinionConstraint: Failed to get slider/hinge constraints");
        return nullptr;
    }

    JPH::Constraint* joltConstraint = settings.Create(*joltBodyA, *joltBodyB);
    if (!joltConstraint)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Physics, "RackAndPinionConstraint: Failed to create constraint");
        return nullptr;
    }
    static_cast<JPH::RackAndPinionConstraint*>(joltConstraint)->SetConstraints(joltSlider, joltHinge);
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
    auto* path = new JPH::PathConstraintPathHermite; // Jolt ref-counted; constraint settings takes ownership
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
    if (!joltConstraint)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Physics, "PathConstraint: Failed to create constraint");
        return nullptr;
    }
    m_joltSystem->AddConstraint(joltConstraint);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Path, joltConstraint);
    m_constraints.push_back(constraint);
    return constraint;
}

// ============================================================================
// CONSTRAINT REMOVAL
// ============================================================================

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
