#include "VehiclePhysics.h"
#include "../Core/Platform.h"
/**
 * @file VehiclePhysics.cpp
 * @brief Jolt vehicle physics wrapper implementation
 * @author Spark Engine Team
 * @date 2025
 */

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

#include <algorithm>
#include <cmath>

JPH_SUPPRESS_WARNINGS
#include "JoltWarningRestore.h"

using namespace DirectX;

namespace
{
    /// Tracked steering never commands an exactly-stopped track: Jolt rejects a zero track ratio.
    constexpr float kMinTrackRatio = 0.05f;

    /// Below this forward speed (m/s) a steer input with no throttle or brake pivots a tracked
    /// vehicle in place; above it the vehicle coasts through the turn instead.
    constexpr float kPivotTurnMaxSpeed = 1.0f;

    /// Fill the WheelSettings fields shared by wheeled (WV) and tracked (TV) wheels.
    void ApplyCommonWheelSettings(JPH::WheelSettings& ws, const VehicleWheelDesc& wheelDesc)
    {
        ws.mPosition = JPH::Vec3(wheelDesc.position.x, wheelDesc.position.y, wheelDesc.position.z);
        ws.mSuspensionDirection =
            JPH::Vec3(wheelDesc.suspensionDir.x, wheelDesc.suspensionDir.y, wheelDesc.suspensionDir.z);
        ws.mSteeringAxis = JPH::Vec3(wheelDesc.steeringAxis.x, wheelDesc.steeringAxis.y, wheelDesc.steeringAxis.z);
        ws.mWheelForward = JPH::Vec3(wheelDesc.wheelForward.x, wheelDesc.wheelForward.y, wheelDesc.wheelForward.z);
        ws.mWheelUp = JPH::Vec3(wheelDesc.wheelUp.x, wheelDesc.wheelUp.y, wheelDesc.wheelUp.z);
        ws.mRadius = wheelDesc.radius;
        ws.mWidth = wheelDesc.width;
        ws.mSuspensionMinLength = wheelDesc.suspensionMinLength;
        ws.mSuspensionMaxLength = wheelDesc.suspensionMaxLength;
        ws.mSuspensionSpring.mFrequency = wheelDesc.suspensionFrequency;
        ws.mSuspensionSpring.mDamping = wheelDesc.suspensionDamping;
    }

    void ApplyEngineAndTransmission(JPH::VehicleEngineSettings& engine, JPH::VehicleTransmissionSettings& transmission,
                                    const VehicleDesc& desc)
    {
        engine.mMaxTorque = desc.maxEngineTorque;
        engine.mMinRPM = desc.minRPM;
        engine.mMaxRPM = desc.maxRPM;

        transmission.mGearRatios.clear();
        for (float ratio : desc.gearRatios)
        {
            transmission.mGearRatios.push_back(ratio);
        }
        // Jolt defaults to one reverse ratio (-2.9); replace it rather than appending a second
        // gear. Jolt expects reverse ratios to be negative, so accept either sign from callers.
        transmission.mReverseGearRatios.clear();
        transmission.mReverseGearRatios.push_back(-std::fabs(desc.reverseGearRatio));
        transmission.mClutchStrength = desc.clutchStrength;
    }

    void ConfigureWheeledVehicle(JPH::VehicleConstraintSettings& vehicleSettings, const VehicleDesc& desc)
    {
        for (const auto& wheelDesc : desc.wheels)
        {
            JPH::WheelSettingsWV* ws = new JPH::WheelSettingsWV; // Jolt takes ownership via mWheels
            ApplyCommonWheelSettings(*ws, wheelDesc);
            ws->mMaxSteerAngle = wheelDesc.maxSteerAngle;
            ws->mMaxBrakeTorque = wheelDesc.maxBrakeTorque;
            ws->mMaxHandBrakeTorque = wheelDesc.maxHandBrakeTorque;
            vehicleSettings.mWheels.push_back(ws);
        }

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
        ApplyEngineAndTransmission(controller->mEngine, controller->mTransmission, desc);

        // Differentials — all-wheel drive: one diff per axle. Jolt requires the engine torque
        // ratios of all differentials to sum to 1, so the two axles split the torque evenly.
        if (desc.wheels.size() >= 4)
        {
            JPH::VehicleDifferentialSettings diff;
            diff.mLeftWheel = 0;
            diff.mRightWheel = 1;
            diff.mDifferentialRatio = desc.differentialRatio;
            diff.mEngineTorqueRatio = 0.5f;
            controller->mDifferentials.push_back(diff);

            JPH::VehicleDifferentialSettings rearDiff;
            rearDiff.mLeftWheel = 2;
            rearDiff.mRightWheel = 3;
            rearDiff.mDifferentialRatio = desc.differentialRatio;
            rearDiff.mEngineTorqueRatio = 0.5f;
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

        // Anti-roll bars pair the front and rear axles (wheels 0/1 and 2/3).
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
    }

    /**
     * Build a tracked vehicle: WheelSettingsTV wheels split into two tracks by the sign of their X
     * position, each track driven through its rearmost wheel. Engine +X is right in the engine's
     * left-handed convention, but positions reach Jolt unconverted and Jolt's right-handed frame
     * (forward +Z, up +Y) calls +X left, so +X wheels form ETrackSide::Left. Returns false (and
     * builds nothing) for a layout Jolt cannot drive: a centred wheel or an empty track.
     */
    bool ConfigureTrackedVehicle(JPH::VehicleConstraintSettings& vehicleSettings, const VehicleDesc& desc)
    {
        if (!(desc.differentialRatio > 0.0f))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Physics,
                            "VehiclePhysics: tracked vehicle needs differentialRatio > 0 (got %.3f)",
                            desc.differentialRatio);
            return false;
        }

        size_t plusXCount = 0;
        for (size_t i = 0; i < desc.wheels.size(); ++i)
        {
            const float x = desc.wheels[i].position.x;
            if (x == 0.0f || !std::isfinite(x))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Physics,
                                "VehiclePhysics: tracked wheel %zu has x=%.3f; every track wheel must sit on one "
                                "side of the hull (x != 0)",
                                i, x);
                return false;
            }
            plusXCount += x > 0.0f ? 1 : 0;
        }
        if (plusXCount == 0 || plusXCount == desc.wheels.size())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Physics,
                            "VehiclePhysics: tracked vehicle needs wheels on both sides (%zu at +X, %zu at -X)",
                            plusXCount, desc.wheels.size() - plusXCount);
            return false;
        }

        auto* controller = new JPH::TrackedVehicleControllerSettings; // Jolt takes ownership via mController
        vehicleSettings.mController = controller;
        ApplyEngineAndTransmission(controller->mEngine, controller->mTransmission, desc);

        JPH::VehicleTrackSettings& plusXTrack = controller->mTracks[static_cast<int>(JPH::ETrackSide::Left)];
        JPH::VehicleTrackSettings& minusXTrack = controller->mTracks[static_cast<int>(JPH::ETrackSide::Right)];
        for (JPH::VehicleTrackSettings* track : {&plusXTrack, &minusXTrack})
        {
            track->mWheels.clear();
            track->mDifferentialRatio = desc.differentialRatio;
            track->mMaxBrakeTorque = 0.0f; // accumulated from the track's wheels below
        }

        for (size_t i = 0; i < desc.wheels.size(); ++i)
        {
            const VehicleWheelDesc& wheelDesc = desc.wheels[i];
            auto* ws = new JPH::WheelSettingsTV; // Jolt takes ownership via mWheels
            ApplyCommonWheelSettings(*ws, wheelDesc);
            // The descriptor's friction values are multipliers; scale Jolt's track defaults (4 / 2).
            ws->mLongitudinalFriction *= wheelDesc.longitudinalFriction;
            ws->mLateralFriction *= wheelDesc.lateralFriction;
            vehicleSettings.mWheels.push_back(ws);

            JPH::VehicleTrackSettings& track = wheelDesc.position.x > 0.0f ? plusXTrack : minusXTrack;
            const auto wheelIndex = static_cast<JPH::uint>(i);
            // Jolt brakes a whole track through its driven wheel, so the track's brake torque is
            // the sum of its wheels' brakes.
            track.mMaxBrakeTorque += std::max(0.0f, wheelDesc.maxBrakeTorque);
            if (track.mWheels.empty() || wheelDesc.position.z < desc.wheels[track.mDrivenWheel].position.z)
            {
                track.mDrivenWheel = wheelIndex; // rearmost wheel drives the track, as in Jolt's tank sample
            }
            track.mWheels.push_back(wheelIndex);
        }
        return true;
    }
} // namespace

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

    if (desc.type == PhysicsVehicleType::Tracked)
    {
        if (!ConfigureTrackedVehicle(vehicleSettings, desc))
        {
            return;
        }
    }
    else
    {
        ConfigureWheeledVehicle(vehicleSettings, desc);
    }

    // Create the vehicle constraint
    JPH::Body* joltBody = joltSystem->GetBodyLockInterface().TryGetBody(bodyID);
    if (!joltBody)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Physics, "VehiclePhysics: TryGetBody failed, cannot create constraint");
        return;
    }

    auto* constraint = new JPH::VehicleConstraint(*joltBody, vehicleSettings); // Jolt ref-counted via AddConstraint

    // Set up collision tester (raycast-based wheel ground detection). The tester filters
    // against the car body's own object layer, whatever layer CreateBody assigned it.
    auto* collisionTester = new JPH::VehicleCollisionTesterRay(bodyInterface.GetObjectLayer(bodyID)); // ref-counted
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

    throttle = std::clamp(throttle, -1.0f, 1.0f);
    brake = std::clamp(brake, 0.0f, 1.0f);
    handbrake = std::clamp(handbrake, 0.0f, 1.0f);

    // A parked vehicle is put to sleep by Jolt, and a sleeping body skips the vehicle
    // constraint entirely, so driver input must wake it or the car never responds.
    if (throttle != 0.0f || brake != 0.0f || steerAngle != 0.0f || handbrake != 0.0f)
    {
        if (m_physicsSystem && m_physicsSystem->GetJoltSystem() && m_body)
        {
            m_physicsSystem->GetJoltSystem()->GetBodyInterface().ActivateBody(JPH::BodyID(m_body->GetJoltBodyID()));
        }
    }

    if (m_desc.type == PhysicsVehicleType::Wheeled || m_desc.type == PhysicsVehicleType::Motorcycle)
    {
        // Jolt takes steering as a fraction in [-1, 1] of each wheel's mMaxSteerAngle and sets
        // the wheel angle to -fraction * max. Convert the requested angle (radians) into that
        // fraction, negated so a positive angle turns toward +X (right in the engine's
        // left-handed convention) and GetWheelSteerAngle() reports the requested angle.
        float maxSteerAngle = 0.0f;
        for (const auto& wheel : m_desc.wheels)
        {
            maxSteerAngle = std::max(maxSteerAngle, std::fabs(wheel.maxSteerAngle));
        }
        const float steerFraction = maxSteerAngle > 0.0f ? std::clamp(steerAngle / maxSteerAngle, -1.0f, 1.0f) : 0.0f;

        auto* controller = static_cast<JPH::WheeledVehicleController*>(m_joltController);
        controller->SetDriverInput(throttle, -steerFraction, brake, handbrake);
    }
    else if (m_desc.type == PhysicsVehicleType::Tracked)
    {
        // Tracks steer by speed difference: the track on the side being turned toward (the inner
        // track) slows, stops and then reverses as the steer fraction grows. Positive steer turns
        // toward engine +X, and the +X wheels form Jolt's ETrackSide::Left track.
        const float fullSteer = std::fabs(m_desc.trackedFullSteerAngle);
        const float steerFraction = fullSteer > 0.0f ? std::clamp(steerAngle / fullSteer, -1.0f, 1.0f) : 0.0f;
        const float trackBrake = std::max(brake, handbrake); // tracks have no separate parking brake
        float forward = throttle;
        float innerRatio = 1.0f;

        if (steerFraction != 0.0f)
        {
            float forwardSpeed = 0.0f;
            if (m_physicsSystem && m_physicsSystem->GetJoltSystem() && m_body)
            {
                const auto& bodyInterface = m_physicsSystem->GetJoltSystem()->GetBodyInterface();
                const JPH::BodyID bodyID(m_body->GetJoltBodyID());
                forwardSpeed =
                    (bodyInterface.GetRotation(bodyID).Conjugated() * bodyInterface.GetLinearVelocity(bodyID)).GetZ();
            }

            if (forward == 0.0f && trackBrake == 0.0f && std::fabs(forwardSpeed) < kPivotTurnMaxSpeed)
            {
                // Pivot in place: counter-rotate the tracks; the steer amount sets the turn rate.
                forward = std::fabs(steerFraction);
                innerRatio = -1.0f;
            }
            else
            {
                innerRatio = 1.0f - 2.0f * std::fabs(steerFraction);
                if (std::fabs(innerRatio) < kMinTrackRatio)
                {
                    innerRatio = innerRatio < 0.0f ? -kMinTrackRatio : kMinTrackRatio;
                }
            }
        }

        const float plusXRatio = steerFraction > 0.0f ? innerRatio : 1.0f;
        const float minusXRatio = steerFraction < 0.0f ? innerRatio : 1.0f;
        auto* controller = static_cast<JPH::TrackedVehicleController*>(m_joltController);
        controller->SetDriverInput(forward, plusXRatio, minusXRatio, trackBrake);
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
