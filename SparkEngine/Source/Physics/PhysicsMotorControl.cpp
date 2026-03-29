#include "../Core/Platform.h"
/**
 * @file PhysicsMotorControl.cpp
 * @brief Constraint motor control and removal for PhysicsSystem
 * @author Spark Engine Team
 * @date 2025
 *
 * Motor velocity/position targets for hinge and slider constraints,
 * motor disable, and constraint removal.
 */

#include "PhysicsSystem.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/MotorSettings.h>

JPH_SUPPRESS_WARNINGS
#include "JoltWarningRestore.h"

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

// RemoveConstraint lives in PhysicsConstraints.cpp alongside constraint creation
