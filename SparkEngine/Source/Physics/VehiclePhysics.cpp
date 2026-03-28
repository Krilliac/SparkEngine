#include "../Core/Platform.h"
/**
 * @file VehiclePhysics.cpp
 * @brief Jolt vehicle physics wrapper implementation
 * @author Spark Engine Team
 * @date 2025
 */

#include "VehiclePhysics.h"
#include "PhysicsBody.h"
#include "PhysicsSystem.h"
#include "../Utils/Validate.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Vehicle/TrackedVehicleController.h>
#include <Jolt/Physics/Vehicle/MotorcycleController.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>

JPH_SUPPRESS_WARNINGS
#include "JoltWarningRestore.h"

using namespace DirectX;

// ============================================================================
// VEHICLE PHYSICS IMPLEMENTATION
// ============================================================================

VehiclePhysics::VehiclePhysics(PhysicsSystem* physicsSystem, std::shared_ptr<PhysicsBody> body, const VehicleDesc& desc)
    : m_physicsSystem(physicsSystem), m_body(body), m_desc(desc)
{
    if (!physicsSystem || !physicsSystem->GetJoltSystem() || !body)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Physics, "VehiclePhysics creation failed: null PhysicsSystem=%p, body=%p",
                        static_cast<void*>(physicsSystem), static_cast<void*>(body.get()));
        return;
    }

    SPARK_LOG_INFO(Spark::LogCategory::Physics,
                   "Creating vehicle: type=%d, %zu wheels, maxTorque=%.1f, RPM=[%.0f-%.0f], %zu gears",
                   static_cast<int>(desc.type), desc.wheels.size(), desc.maxEngineTorque, desc.minRPM, desc.maxRPM,
                   desc.gearRatios.size());

    auto* joltSystem = physicsSystem->GetJoltSystem();
    auto& bodyInterface = joltSystem->GetBodyInterface();
    JPH::BodyID bodyID(body->GetJoltBodyID());

    if (!bodyInterface.IsAdded(bodyID))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Physics, "VehiclePhysics: body not added to physics system");
        return;
    }

    // Build vehicle constraint settings
    JPH::VehicleConstraintSettings vehicleSettings;
    vehicleSettings.mUp = JPH::Vec3(0, 1, 0);
    vehicleSettings.mForward = JPH::Vec3(0, 0, 1);

    // Configure wheels
    for (const auto& wheelDesc : desc.wheels)
    {
        JPH::WheelSettingsWV* ws = new JPH::WheelSettingsWV; // Jolt takes ownership via mWheels
        ws->mPosition = JPH::Vec3(wheelDesc.position.x, wheelDesc.position.y, wheelDesc.position.z);
        ws->mSuspensionDirection =
            JPH::Vec3(wheelDesc.suspensionDir.x, wheelDesc.suspensionDir.y, wheelDesc.suspensionDir.z);
        ws->mSteeringAxis = JPH::Vec3(wheelDesc.steeringAxis.x, wheelDesc.steeringAxis.y, wheelDesc.steeringAxis.z);
        ws->mWheelForward = JPH::Vec3(wheelDesc.wheelForward.x, wheelDesc.wheelForward.y, wheelDesc.wheelForward.z);
        ws->mWheelUp = JPH::Vec3(wheelDesc.wheelUp.x, wheelDesc.wheelUp.y, wheelDesc.wheelUp.z);
        ws->mRadius = wheelDesc.radius;
        ws->mWidth = wheelDesc.width;
        ws->mSuspensionMinLength = wheelDesc.suspensionMinLength;
        ws->mSuspensionMaxLength = wheelDesc.suspensionMaxLength;
        ws->mSuspensionSpring.mFrequency = wheelDesc.suspensionFrequency;
        ws->mSuspensionSpring.mDamping = wheelDesc.suspensionDamping;
        ws->mMaxSteerAngle = wheelDesc.maxSteerAngle;
        ws->mMaxBrakeTorque = wheelDesc.maxBrakeTorque;
        ws->mMaxHandBrakeTorque = wheelDesc.maxHandBrakeTorque;
        vehicleSettings.mWheels.push_back(ws);
    }

    // Configure vehicle controller based on type
    if (desc.type == PhysicsVehicleType::Wheeled || desc.type == PhysicsVehicleType::Motorcycle)
    {
        JPH::WheeledVehicleControllerSettings* controller = nullptr;

        if (desc.type == PhysicsVehicleType::Motorcycle)
        {
            auto* mcController = new JPH::MotorcycleControllerSettings; // Jolt takes ownership via mController
            mcController->mLeanSpringConstant = desc.leanSpringConstant;
            mcController->mLeanSpringDamping = desc.leanSpringDamping;
            controller = mcController;
        }
        else
        {
            controller = new JPH::WheeledVehicleControllerSettings; // Jolt takes ownership via mController
        }

        // Engine
        controller->mEngine.mMaxTorque = desc.maxEngineTorque;
        controller->mEngine.mMinRPM = desc.minRPM;
        controller->mEngine.mMaxRPM = desc.maxRPM;

        // Transmission
        controller->mTransmission.mGearRatios.clear();
        for (float ratio : desc.gearRatios)
        {
            controller->mTransmission.mGearRatios.push_back(ratio);
        }
        controller->mTransmission.mReverseGearRatios.push_back(-desc.reverseGearRatio);
        controller->mTransmission.mClutchStrength = desc.clutchStrength;

        // Differentials — simple setup: one diff connecting front/back axles
        if (desc.wheels.size() >= 4)
        {
            JPH::VehicleDifferentialSettings diff;
            diff.mLeftWheel = 0;
            diff.mRightWheel = 1;
            diff.mDifferentialRatio = desc.differentialRatio;
            controller->mDifferentials.push_back(diff);

            JPH::VehicleDifferentialSettings rearDiff;
            rearDiff.mLeftWheel = 2;
            rearDiff.mRightWheel = 3;
            rearDiff.mDifferentialRatio = desc.differentialRatio;
            controller->mDifferentials.push_back(rearDiff);
        }
        else if (desc.wheels.size() >= 2)
        {
            JPH::VehicleDifferentialSettings diff;
            diff.mLeftWheel = 0;
            diff.mRightWheel = 1;
            diff.mDifferentialRatio = desc.differentialRatio;
            controller->mDifferentials.push_back(diff);
        }

        vehicleSettings.mController = controller;
    }
    else if (desc.type == PhysicsVehicleType::Tracked)
    {
        auto* controller = new JPH::TrackedVehicleControllerSettings; // Jolt takes ownership via mController
        controller->mEngine.mMaxTorque = desc.maxEngineTorque;
        controller->mEngine.mMinRPM = desc.minRPM;
        controller->mEngine.mMaxRPM = desc.maxRPM;
        vehicleSettings.mController = controller;
    }

    // Anti-roll bars
    if (desc.antiRollBarStiffness > 0.0f && desc.wheels.size() >= 4)
    {
        JPH::VehicleAntiRollBar frontBar;
        frontBar.mLeftWheel = 0;
        frontBar.mRightWheel = 1;
        frontBar.mStiffness = desc.antiRollBarStiffness;
        vehicleSettings.mAntiRollBars.push_back(frontBar);

        JPH::VehicleAntiRollBar rearBar;
        rearBar.mLeftWheel = 2;
        rearBar.mRightWheel = 3;
        rearBar.mStiffness = desc.antiRollBarStiffness;
        vehicleSettings.mAntiRollBars.push_back(rearBar);
    }

    // Create the vehicle constraint
    JPH::Body* joltBody = joltSystem->GetBodyLockInterface().TryGetBody(bodyID);
    if (!joltBody)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Physics, "VehiclePhysics: TryGetBody failed, cannot create constraint");
        return;
    }

    auto* constraint = new JPH::VehicleConstraint(*joltBody, vehicleSettings); // Jolt ref-counted via AddConstraint

    // Set up collision tester (raycast-based wheel ground detection)
    auto* collisionTester = new JPH::VehicleCollisionTesterRay(1 /* MOVING layer */); // Jolt ref-counted
    constraint->SetVehicleCollisionTester(collisionTester);

    joltSystem->AddConstraint(constraint);
    joltSystem->AddStepListener(constraint);

    m_joltConstraint = constraint;
    m_joltController = constraint->GetController();

    SPARK_LOG_INFO(Spark::LogCategory::Physics, "Vehicle created successfully with %zu wheels", desc.wheels.size());
}

VehiclePhysics::~VehiclePhysics()
{
    if (m_joltConstraint && m_physicsSystem && m_physicsSystem->GetJoltSystem())
    {
        auto* constraint = static_cast<JPH::VehicleConstraint*>(m_joltConstraint);
        auto* joltSystem = m_physicsSystem->GetJoltSystem();
        joltSystem->RemoveStepListener(constraint);
        joltSystem->RemoveConstraint(constraint);
    }
    m_joltConstraint = nullptr;
    m_joltController = nullptr;
}

void VehiclePhysics::SetInput(float throttle, float brake, float steerAngle, float handbrake)
{
    if (!m_joltController)
        return;

    if (m_desc.type == PhysicsVehicleType::Wheeled || m_desc.type == PhysicsVehicleType::Motorcycle)
    {
        auto* controller = static_cast<JPH::WheeledVehicleController*>(m_joltController);
        controller->SetDriverInput(throttle, steerAngle, brake, handbrake);
    }
    else if (m_desc.type == PhysicsVehicleType::Tracked)
    {
        auto* controller = static_cast<JPH::TrackedVehicleController*>(m_joltController);
        controller->SetDriverInput(throttle, steerAngle, brake, handbrake);
    }
}

float VehiclePhysics::GetEngineRPM() const
{
    if (!m_joltController)
        return 0.0f;

    if (m_desc.type == PhysicsVehicleType::Wheeled || m_desc.type == PhysicsVehicleType::Motorcycle)
    {
        auto* controller = static_cast<JPH::WheeledVehicleController*>(m_joltController);
        return controller->GetEngine().GetCurrentRPM();
    }
    else if (m_desc.type == PhysicsVehicleType::Tracked)
    {
        auto* controller = static_cast<JPH::TrackedVehicleController*>(m_joltController);
        return controller->GetEngine().GetCurrentRPM();
    }
    return 0.0f;
}

int VehiclePhysics::GetCurrentGear() const
{
    if (!m_joltController)
        return 0;

    if (m_desc.type == PhysicsVehicleType::Wheeled || m_desc.type == PhysicsVehicleType::Motorcycle)
    {
        auto* controller = static_cast<JPH::WheeledVehicleController*>(m_joltController);
        return controller->GetTransmission().GetCurrentGear();
    }
    else if (m_desc.type == PhysicsVehicleType::Tracked)
    {
        auto* controller = static_cast<JPH::TrackedVehicleController*>(m_joltController);
        return controller->GetTransmission().GetCurrentGear();
    }
    return 0;
}

float VehiclePhysics::GetSpeed() const
{
    if (!m_body || !m_physicsSystem || !m_physicsSystem->GetJoltSystem())
        return 0.0f;

    auto& bi = m_physicsSystem->GetJoltSystem()->GetBodyInterface();
    JPH::Vec3 vel = bi.GetLinearVelocity(JPH::BodyID(m_body->GetJoltBodyID()));
    return vel.Length();
}

uint32_t VehiclePhysics::GetWheelCount() const
{
    return static_cast<uint32_t>(m_desc.wheels.size());
}

XMFLOAT3 VehiclePhysics::GetWheelContactPosition(uint32_t wheelIndex) const
{
    if (!m_joltConstraint || wheelIndex >= GetWheelCount())
        return {0, 0, 0};

    auto* constraint = static_cast<JPH::VehicleConstraint*>(m_joltConstraint);
    const JPH::Wheel* wheel = constraint->GetWheel(wheelIndex);
    if (!wheel->HasContact())
        return {0, 0, 0};

    JPH::Vec3 pos = wheel->GetContactPosition();
    return XMFLOAT3(pos.GetX(), pos.GetY(), pos.GetZ());
}

XMFLOAT3 VehiclePhysics::GetWheelContactNormal(uint32_t wheelIndex) const
{
    if (!m_joltConstraint || wheelIndex >= GetWheelCount())
        return {0, 1, 0};

    auto* constraint = static_cast<JPH::VehicleConstraint*>(m_joltConstraint);
    const JPH::Wheel* wheel = constraint->GetWheel(wheelIndex);
    if (!wheel->HasContact())
        return {0, 1, 0};

    JPH::Vec3 normal = wheel->GetContactNormal();
    return XMFLOAT3(normal.GetX(), normal.GetY(), normal.GetZ());
}

float VehiclePhysics::GetWheelSuspensionLength(uint32_t wheelIndex) const
{
    if (!m_joltConstraint || wheelIndex >= GetWheelCount())
        return 0.0f;

    auto* constraint = static_cast<JPH::VehicleConstraint*>(m_joltConstraint);
    return constraint->GetWheel(wheelIndex)->GetSuspensionLength();
}

bool VehiclePhysics::IsWheelOnGround(uint32_t wheelIndex) const
{
    if (!m_joltConstraint || wheelIndex >= GetWheelCount())
        return false;

    auto* constraint = static_cast<JPH::VehicleConstraint*>(m_joltConstraint);
    return constraint->GetWheel(wheelIndex)->HasContact();
}

float VehiclePhysics::GetWheelRotationAngle(uint32_t wheelIndex) const
{
    if (!m_joltConstraint || wheelIndex >= GetWheelCount())
        return 0.0f;

    auto* constraint = static_cast<JPH::VehicleConstraint*>(m_joltConstraint);
    return constraint->GetWheel(wheelIndex)->GetRotationAngle();
}

float VehiclePhysics::GetWheelSteerAngle(uint32_t wheelIndex) const
{
    if (!m_joltConstraint || wheelIndex >= GetWheelCount())
        return 0.0f;

    auto* constraint = static_cast<JPH::VehicleConstraint*>(m_joltConstraint);
    return constraint->GetWheel(wheelIndex)->GetSteerAngle();
}
