/**
 * @file RacingVehicleChassis.cpp
 * @brief RacingVehicleSystem's Jolt chassis: construction from VehicleStats, per-tick drive forces, and pose
 *        read-back. Split from RacingVehicleSystem.cpp (same class, feature-owned translation unit).
 *
 * Rotation convention: PhysicsBody Get/SetRotation and PhysicsBodyDesc::rotation reach
 * JPH::Quat::sEulerAngles unconverted, i.e. radians with R = Rz * Ry * Rx. The chassis basis
 * is rebuilt from those angles explicitly (same convention as the engine vehicle tests).
 */

#include "RacingVehicleSystem.h"
#include "Physics/PhysicsBody.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/VehiclePhysics.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace Racing
{
    namespace
    {
        // Chassis footprint shared by every vehicle type (metres). Engine convention: +X right, +Y up, +Z forward.
        constexpr float kChassisHalfWidth = 0.9f;
        constexpr float kChassisHalfHeight = 0.3f;
        constexpr float kChassisHalfLength = 2.1f;
        constexpr float kCenterOfMassDrop = 0.3f; ///< Lowered centre of mass keeps the car planted in corners

        constexpr float kWheelRadius = 0.34f;
        constexpr float kWheelWidth = 0.22f;
        constexpr float kWheelTrackX = 0.85f;
        constexpr float kWheelBaseZ = 1.35f;
        constexpr float kWheelMountY = -0.1f;
        constexpr float kSuspensionMin = 0.15f;
        constexpr float kSuspensionMax = 0.45f;

        /// Chassis origin height above the ground at rest; VehicleInstance::positionY is origin minus this.
        constexpr float kRideHeight = -kWheelMountY + 0.3f + kWheelRadius;
        constexpr float kSpawnLift = 0.1f; ///< Spawn slightly high so the suspension settles instead of popping out

        // Drivetrain. Gearing puts maxRPM in top gear at 1.3x the governed top speed, leaving boost headroom.
        constexpr float kMinRPM = 1000.0f;
        constexpr float kMaxRPM = 7000.0f;
        constexpr float kFirstGear = 2.66f;
        constexpr float kTopGear = 0.74f;
        constexpr float kBoostSpeedFactor = 1.3f;
        constexpr float kEngineDamping = 0.2f;     ///< Jolt VehicleEngineSettings::mAngularDamping default, N*m*s/rad
        constexpr float kBoostAcceleration = 4.0f; ///< m/s^2 of extra thrust while nitro or drift boost runs

        constexpr float kSteerFalloffSpeed = 20.0f; ///< m/s at which the steering lock has halved
        constexpr float kDriftHandbrake = 0.3f;     ///< Rear handbrake fraction that breaks traction in a drift
        constexpr float kStrandedUpDot = 0.3f;      ///< Chassis up-vector Y below this counts as rolled over
        constexpr float kStrandedSpeedKmh = 2.0f;

        constexpr float kMsToKmh = 3.6f;
        constexpr float kGravity = 9.81f;
        constexpr float kPi = 3.14159265f;

        struct ChassisBasis
        {
            float forward[3];
            float up[3];
        };

        /// Columns of R = Rz * Ry * Rx for the Jolt Euler angles PhysicsBody::GetRotation() returns.
        ChassisBasis BasisFromEuler(const XMFLOAT3& euler)
        {
            const float cx = std::cos(euler.x), sx = std::sin(euler.x);
            const float cy = std::cos(euler.y), sy = std::sin(euler.y);
            const float cz = std::cos(euler.z), sz = std::sin(euler.z);
            return {{cx * sy * cz + sx * sz, cx * sy * sz - sx * cz, cx * cy},
                    {sx * sy * cz - cx * sz, sx * sy * sz + cx * cz, sx * cy}};
        }

        /// Off-track rolling resistance (1/s): grass and sand bleed speed so a car that runs wide slows down.
        float SurfaceRollingResistance(SurfaceType surface)
        {
            switch (surface)
            {
            case SurfaceType::Grass:
                return 0.1f;
            case SurfaceType::Sand:
                return 0.2f;
            default:
                return 0.0f;
            }
        }

        VehicleWheelDesc MakeWheel(float x, float z, float maxSteer, float brakeTorque, float handBrakeTorque)
        {
            VehicleWheelDesc wheel;
            wheel.position = {x, kWheelMountY, z};
            wheel.radius = kWheelRadius;
            wheel.width = kWheelWidth;
            wheel.suspensionMinLength = kSuspensionMin;
            wheel.suspensionMaxLength = kSuspensionMax;
            wheel.suspensionFrequency = 1.8f;
            wheel.suspensionDamping = 0.5f;
            wheel.maxSteerAngle = maxSteer;
            wheel.maxBrakeTorque = brakeTorque;
            wheel.maxHandBrakeTorque = handBrakeTorque;
            return wheel;
        }
    } // namespace

    bool RacingVehicleSystem::BuildChassis(VehicleInstance& vehicle, const VehiclePose& pose, float initialSpeedKmh)
    {
        if (!m_physics)
            return false;
        DestroyChassis(vehicle.id);

        const VehicleStats& stats = vehicle.baseStats;
        const float mass = std::max(stats.weight, 50.0f);
        const float forwardX = std::sin(pose.heading);
        const float forwardZ = std::cos(pose.heading);
        const float initialSpeed = std::max(initialSpeedKmh, 0.0f) / kMsToKmh;

        PhysicsBodyDesc bodyDesc;
        bodyDesc.name = "Racing_Vehicle_" + std::to_string(vehicle.id);
        bodyDesc.type = PhysicsBodyType::Dynamic;
        bodyDesc.mass = mass;
        bodyDesc.position = {pose.x, pose.y + kRideHeight + kSpawnLift, pose.z};
        bodyDesc.rotation = {0.0f, pose.heading, 0.0f}; // radians (see file header)
        bodyDesc.linearVelocity = {forwardX * initialSpeed, 0.0f, forwardZ * initialSpeed};
        bodyDesc.shape.type = CollisionShapeType::OffsetCenterOfMass;
        // CreateBody's box dimensions are full extents (the shape factory halves them for Jolt).
        bodyDesc.shape.dimensions = {2.0f * kChassisHalfWidth, 2.0f * kChassisHalfHeight, 2.0f * kChassisHalfLength};
        bodyDesc.shape.localOffset = {0.0f, -kCenterOfMassDrop, 0.0f};
        bodyDesc.material.friction = 0.5f;
        bodyDesc.material.restitution = 0.1f;
        bodyDesc.material.linearDamping = 0.01f; // Jolt damping is linear in speed; keep it to light air drag
        bodyDesc.material.angularDamping = 0.05f;
        bodyDesc.entityId = vehicle.id;

        std::shared_ptr<PhysicsBody> body = m_physics->CreateBody(bodyDesc);
        if (!body)
            return false;

        // Gearing: maxRPM in top gear at the boosted top speed. Torque: the rated launch acceleration in first gear.
        const float boostedTopSpeed = std::max(stats.maxSpeed, 20.0f) / kMsToKmh * kBoostSpeedFactor;
        const float differentialRatio = kMaxRPM * 2.0f * kPi * kWheelRadius / (60.0f * kTopGear * boostedTopSpeed);
        const float launchAcceleration = 2.5f + 6.0f * std::clamp(stats.acceleration, 0.0f, 1.0f);
        const float brakeDeceleration = 4.0f + 6.0f * std::clamp(stats.braking, 0.0f, 1.0f);
        const float wheelBrakeTorque = mass * brakeDeceleration * kWheelRadius / 4.0f;
        const float handBrakeTorque = mass * kGravity * kWheelRadius;
        const float maxSteer = 0.3f + 0.25f * std::clamp(stats.handling, 0.0f, 1.0f);

        VehicleDesc vehicleDesc;
        vehicleDesc.type = PhysicsVehicleType::Wheeled;
        vehicleDesc.minRPM = kMinRPM;
        vehicleDesc.maxRPM = kMaxRPM;
        vehicleDesc.gearRatios = {kFirstGear, 1.78f, 1.30f, 1.0f, kTopGear};
        vehicleDesc.differentialRatio = differentialRatio;
        // Jolt's engine loses kEngineDamping * omega to internal friction; add it back at the typical shift RPM so
        // light cars still reach their rated acceleration.
        const float frictionTorque = kEngineDamping * (0.6f * kMaxRPM * 2.0f * kPi / 60.0f);
        vehicleDesc.maxEngineTorque =
            mass * launchAcceleration * kWheelRadius / (kFirstGear * differentialRatio) + frictionTorque;
        vehicleDesc.antiRollBarStiffness = mass * 2.0f;
        vehicleDesc.wheels = {
            MakeWheel(-kWheelTrackX, kWheelBaseZ, maxSteer, wheelBrakeTorque, 0.0f),         // FL
            MakeWheel(kWheelTrackX, kWheelBaseZ, maxSteer, wheelBrakeTorque, 0.0f),          // FR
            MakeWheel(-kWheelTrackX, -kWheelBaseZ, 0.0f, wheelBrakeTorque, handBrakeTorque), // RL
            MakeWheel(kWheelTrackX, -kWheelBaseZ, 0.0f, wheelBrakeTorque, handBrakeTorque),  // RR
        };

        std::unique_ptr<VehiclePhysics> joltVehicle = m_physics->CreateVehicle(body, vehicleDesc);
        if (!joltVehicle)
        {
            m_physics->RemoveBody(body);
            return false;
        }

        Chassis chassis;
        chassis.body = std::move(body);
        chassis.vehicle = std::move(joltVehicle);
        chassis.maxSteerAngle = maxSteer;
        m_chassis.emplace(vehicle.id, std::move(chassis));

        vehicle.positionX = pose.x;
        vehicle.positionY = pose.y;
        vehicle.positionZ = pose.z;
        vehicle.heading = pose.heading;
        vehicle.strandedTime = 0.0f;
        return true;
    }

    void RacingVehicleSystem::DestroyChassis(uint32_t vehicleId)
    {
        auto it = m_chassis.find(vehicleId);
        if (it == m_chassis.end())
            return;
        it->second.vehicle.reset(); // removes the Jolt constraint while its body still exists
        if (m_physics && it->second.body)
            m_physics->RemoveBody(it->second.body);
        m_chassis.erase(it);
    }

    void RacingVehicleSystem::DriveChassis(const VehicleInstance& vehicle, Chassis& chassis)
    {
        PhysicsBody& body = *chassis.body;
        const ChassisBasis basis = BasisFromEuler(body.GetRotation());
        const XMFLOAT3 velocity = body.GetLinearVelocity();
        const float forwardSpeed =
            velocity.x * basis.forward[0] + velocity.y * basis.forward[1] + velocity.z * basis.forward[2];
        const bool boosting = vehicle.boostTimer > 0.0f;

        // Damage saps drive; the governor holds the rated top speed unless a boost is running.
        const float durability = std::max(vehicle.baseStats.durability, 1.0f);
        const float damagePenalty = 1.0f - std::min(vehicle.damage / durability, 0.5f);
        const float governedSpeed = vehicle.baseStats.maxSpeed * (boosting ? kBoostSpeedFactor : 1.0f);
        const float throttle = forwardSpeed * kMsToKmh >= governedSpeed ? 0.0f : vehicle.throttleInput * damagePenalty;

        // Speed-sensitive steering lock: full lock when parked, progressively less at speed so a full-lock input
        // at racing speed stays inside the tyres' grip.
        const float steerLock = chassis.maxSteerAngle / (1.0f + std::max(forwardSpeed, 0.0f) / kSteerFalloffSpeed);
        const bool drifting =
            vehicle.driftState == DriftState::Initiating || vehicle.driftState == DriftState::Drifting;
        chassis.vehicle->SetInput(throttle, vehicle.brakeInput, vehicle.steerAngle * steerLock,
                                  drifting ? kDriftHandbrake : 0.0f);

        const float mass = body.GetMass();
        if (boosting && vehicle.throttleInput > 0.0f)
        {
            const float thrust = mass * kBoostAcceleration;
            body.ApplyForce({basis.forward[0] * thrust, basis.forward[1] * thrust, basis.forward[2] * thrust});
        }

        const float resistance = SurfaceRollingResistance(vehicle.currentSurface);
        if (resistance > 0.0f)
            body.ApplyForce({-velocity.x * mass * resistance, 0.0f, -velocity.z * mass * resistance});
    }

    void RacingVehicleSystem::ReadBackChassis(VehicleInstance& vehicle, const Chassis& chassis, float dt)
    {
        const PhysicsBody& body = *chassis.body;
        const XMFLOAT3 position = body.GetPosition();
        const ChassisBasis basis = BasisFromEuler(body.GetRotation());
        const XMFLOAT3 velocity = body.GetLinearVelocity();

        vehicle.positionX = position.x;
        vehicle.positionY = position.y - kRideHeight;
        vehicle.positionZ = position.z;
        const float horizontal = std::hypot(basis.forward[0], basis.forward[2]);
        if (horizontal > 1.0e-3f)
            vehicle.heading = std::atan2(basis.forward[0], basis.forward[2]);

        const float forwardSpeed =
            velocity.x * basis.forward[0] + velocity.y * basis.forward[1] + velocity.z * basis.forward[2];
        vehicle.speed = std::max(forwardSpeed, 0.0f) * kMsToKmh;
        vehicle.rpm = chassis.vehicle->GetEngineRPM();

        const bool rolledOver = basis.up[1] < kStrandedUpDot;
        const bool pinned = vehicle.throttleInput > 0.5f && vehicle.speed < kStrandedSpeedKmh;
        vehicle.strandedTime = (rolledOver || pinned) ? vehicle.strandedTime + dt : 0.0f;
    }

    bool RacingVehicleSystem::SetVehiclePose(uint32_t id, const VehiclePose& pose)
    {
        VehicleInstance* vehicle = GetVehicle(id);
        auto it = m_chassis.find(id);
        if (!vehicle || it == m_chassis.end() || !std::isfinite(pose.x) || !std::isfinite(pose.y) ||
            !std::isfinite(pose.z) || !std::isfinite(pose.heading))
        {
            return false;
        }

        PhysicsBody& body = *it->second.body;
        body.SetPosition({pose.x, pose.y + kRideHeight + kSpawnLift, pose.z});
        body.SetRotation({0.0f, pose.heading, 0.0f});
        body.SetLinearVelocity({0.0f, 0.0f, 0.0f});
        body.SetAngularVelocity({0.0f, 0.0f, 0.0f});
        vehicle->positionX = pose.x;
        vehicle->positionY = pose.y;
        vehicle->positionZ = pose.z;
        vehicle->heading = pose.heading;
        vehicle->speed = 0.0f;
        vehicle->strandedTime = 0.0f;
        return true;
    }

    void RacingVehicleSystem::ScaleVelocity(uint32_t id, float factor)
    {
        auto it = m_chassis.find(id);
        if (it == m_chassis.end() || !std::isfinite(factor))
            return;
        factor = std::clamp(factor, 0.0f, 1.0f);
        PhysicsBody& body = *it->second.body;
        const XMFLOAT3 velocity = body.GetLinearVelocity();
        body.SetLinearVelocity({velocity.x * factor, velocity.y, velocity.z * factor});
    }

} // namespace Racing
