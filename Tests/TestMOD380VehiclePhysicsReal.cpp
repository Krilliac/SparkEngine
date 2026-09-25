/**
 * @file TestMOD380VehiclePhysicsReal.cpp
 * @brief MOD-380: runtime tests for the engine's Jolt VehiclePhysics wrapper.
 *
 * Every test builds a real Jolt world through PhysicsSystem (static ground box,
 * dynamic car body), attaches four wheels via PhysicsSystem::CreateVehicle(),
 * and advances the world one shared fixed tick at a time with StepFixed(). The
 * assertions cover the behaviour a racing module needs from the shared runtime:
 * throttle accelerates the car forward, brake slows it, steering yaws it in the
 * engine's left-handed convention (+X is right when looking down +Z), a sleeping
 * car wakes when driven, and identical input scripts produce identical poses.
 * A tracked hull covers the TrackedVehicleController path: driving on both
 * tracks, steering by track-speed difference, pivot turns in place, and
 * rejection of layouts Jolt cannot split into a left and a right track.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_PHYSICS

#include "Core/EngineContext.h"
#include "Physics/PhysicsBody.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/VehiclePhysics.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
    constexpr float kFrontSteerLimit = 0.5f; // radians
    constexpr float kMinEngineRPM = 1000.0f;

    struct VehicleInput
    {
        float throttle = 0.0f;
        float brake = 0.0f;
        float steer = 0.0f;
        float handbrake = 0.0f;
    };

    /// Pose + velocity sample used for determinism comparison.
    struct VehicleSample
    {
        XMFLOAT3 position;
        XMFLOAT3 rotation;
        XMFLOAT3 linearVelocity;
        XMFLOAT3 angularVelocity;
    };

    VehicleWheelDesc MakeWheel(float x, float z, float maxSteer, float handBrakeTorque)
    {
        VehicleWheelDesc wheel;
        wheel.position = {x, -0.18f, z};
        wheel.radius = 0.3f;
        wheel.width = 0.1f;
        wheel.suspensionMinLength = 0.3f;
        wheel.suspensionMaxLength = 0.5f;
        wheel.maxSteerAngle = maxSteer;
        wheel.maxHandBrakeTorque = handBrakeTorque;
        return wheel;
    }

    VehicleDesc MakeCarDesc()
    {
        // Engine convention (left-handed): +X is right, +Z is forward.
        VehicleDesc desc;
        desc.type = PhysicsVehicleType::Wheeled;
        desc.minRPM = kMinEngineRPM;
        desc.wheels = {
            MakeWheel(-0.9f, 1.4f, kFrontSteerLimit, 0.0f), // FL
            MakeWheel(0.9f, 1.4f, kFrontSteerLimit, 0.0f),  // FR
            MakeWheel(-0.9f, -1.4f, 0.0f, 4000.0f),         // RL
            MakeWheel(0.9f, -1.4f, 0.0f, 4000.0f),          // RR
        };
        return desc;
    }

    /// Five road wheels per side, like a small tracked hull. No wheel steers.
    VehicleDesc MakeTankDesc()
    {
        VehicleDesc desc;
        desc.type = PhysicsVehicleType::Tracked;
        desc.minRPM = kMinEngineRPM;
        for (const float x : {-0.9f, 0.9f})
        {
            for (const float z : {1.6f, 0.8f, 0.0f, -0.8f, -1.6f})
            {
                desc.wheels.push_back(MakeWheel(x, z, 0.0f, 0.0f));
            }
        }
        return desc;
    }

    /**
     * A real Jolt world with a 400 m ground slab and one vehicle (a four-wheeled car by default).
     * Heap-allocated PhysicsSystem and explicit teardown: PhysicsSystem logs
     * through SimpleConsole and must not outlive it (see TestPhysicsTeardownGuard).
     * PhysicsBody resolves its world through EngineContext, so the rig registers
     * its PhysicsSystem there for its lifetime and restores the previous one after.
     */
    struct VehicleRig
    {
        std::unique_ptr<PhysicsSystem> physics = std::make_unique<PhysicsSystem>();
        std::shared_ptr<PhysicsBody> ground;
        std::shared_ptr<PhysicsBody> car;
        std::unique_ptr<VehiclePhysics> vehicle;
        EngineContext* context = nullptr;
        PhysicsSystem* previousPhysics = nullptr;
        bool ready = false;

        explicit VehicleRig(const VehicleDesc& desc = MakeCarDesc())
        {
            // Same pattern as TestEngineDiagnostics: a standalone test run has no
            // engine-owned context yet, so create the process-wide one on demand.
            if (!EngineContext::Get())
            {
                EngineContext::SetOwned(std::make_unique<EngineContext>());
            }
            context = EngineContext::Get();
            if (!context || FAILED(physics->Initialize()))
            {
                return;
            }
            previousPhysics = context->GetPhysics();
            context->SetPhysics(physics.get());
            physics->SetDeterministicSimulation(true);

            PhysicsBodyDesc groundDesc;
            groundDesc.name = "mod380_ground";
            groundDesc.type = PhysicsBodyType::Static;
            groundDesc.mass = 0.0f;
            groundDesc.position = {0.0f, -1.0f, 0.0f};
            groundDesc.shape.type = CollisionShapeType::Box;
            groundDesc.shape.dimensions = {200.0f, 1.0f, 200.0f}; // top face at y = 0
            ground = physics->CreateBody(groundDesc);

            PhysicsBodyDesc carDesc;
            carDesc.name = "mod380_car";
            carDesc.type = PhysicsBodyType::Dynamic;
            carDesc.mass = 1500.0f;
            carDesc.position = {0.0f, 1.2f, 0.0f};
            carDesc.shape.type = CollisionShapeType::Box;
            carDesc.shape.dimensions = {0.9f, 0.2f, 2.0f};
            car = physics->CreateBody(carDesc);
            if (!ground || !car)
            {
                return;
            }

            vehicle = physics->CreateVehicle(car, desc);
            ready = vehicle != nullptr;
        }

        ~VehicleRig()
        {
            vehicle.reset(); // removes the Jolt constraint while the world is alive
            physics->Shutdown();
            if (context)
            {
                context->SetPhysics(previousPhysics);
            }
        }

        VehicleRig(const VehicleRig&) = delete;
        VehicleRig& operator=(const VehicleRig&) = delete;

        /// Advance the shared runtime by @p ticks fixed steps of GetTimeStep() each.
        void Step(uint32_t ticks)
        {
            for (uint32_t i = 0; i < ticks; ++i)
            {
                physics->StepFixed(1);
            }
        }

        void Drive(const VehicleInput& input, uint32_t ticks)
        {
            vehicle->SetInput(input.throttle, input.brake, input.steer, input.handbrake);
            Step(ticks);
        }

        float Yaw() const { return car->GetRotation().y; }

        /// Drive and return the heading change (radians, positive toward +X) integrated from the
        /// body's angular velocity. Unlike Yaw(), which reads the Euler angle, this stays correct
        /// once the hull has turned more than a quarter turn.
        float DriveMeasuringTurn(const VehicleInput& input, uint32_t ticks)
        {
            vehicle->SetInput(input.throttle, input.brake, input.steer, input.handbrake);
            float turned = 0.0f;
            for (uint32_t i = 0; i < ticks; ++i)
            {
                physics->StepFixed(1);
                turned += car->GetAngularVelocity().y * physics->GetTimeStep();
            }
            return turned;
        }

        /// Signed speed along the car's current heading (m/s).
        float ForwardSpeed() const
        {
            const float yaw = Yaw();
            const XMFLOAT3 v = car->GetLinearVelocity();
            return v.x * std::sin(yaw) + v.z * std::cos(yaw);
        }

        bool AllWheelsOnGround() const
        {
            for (uint32_t i = 0; i < vehicle->GetWheelCount(); ++i)
            {
                if (!vehicle->IsWheelOnGround(i))
                {
                    return false;
                }
            }
            return true;
        }

        /// Drop onto the ground and let the suspension settle (2 s).
        void Settle() { Drive({}, 120); }
    };

    bool SamplesBitwiseEqual(const std::vector<VehicleSample>& a, const std::vector<VehicleSample>& b)
    {
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(VehicleSample)) == 0;
    }

    /// Scripted lap fragment: launch, sweep right, sweep left, brake, reverse.
    std::vector<VehicleSample> RunScriptedDrive()
    {
        VehicleRig rig;
        std::vector<VehicleSample> samples;
        if (!rig.ready)
        {
            return samples;
        }

        const VehicleInput script[] = {
            {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, {0.8f, 0.0f, 0.3f, 0.0f},  {0.8f, 0.0f, -0.3f, 0.0f},
            {0.0f, 1.0f, 0.1f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {-0.6f, 0.0f, 0.2f, 0.0f},
        };
        for (const VehicleInput& segment : script)
        {
            for (int chunk = 0; chunk < 3; ++chunk)
            {
                rig.Drive(segment, 20);
                samples.push_back({rig.car->GetPosition(), rig.car->GetRotation(), rig.car->GetLinearVelocity(),
                                   rig.car->GetAngularVelocity()});
            }
        }
        return samples;
    }
} // namespace

TEST(VehiclePhysics_JoltVehicleAcceleratesBrakesAndSteers)
{
    // Throttle, brake, reverse.
    {
        VehicleRig rig;
        ASSERT_TRUE(rig.ready);
        EXPECT_EQ(rig.vehicle->GetWheelCount(), 4u);

        rig.Settle();
        EXPECT_TRUE(rig.AllWheelsOnGround());
        EXPECT_LT(rig.vehicle->GetSpeed(), 0.1f);
        const XMFLOAT3 start = rig.car->GetPosition();

        rig.Drive({1.0f, 0.0f, 0.0f, 0.0f}, 180); // 3 s full throttle
        const float cruiseSpeed = rig.ForwardSpeed();
        EXPECT_GT(cruiseSpeed, 5.0f);
        EXPECT_GT(rig.car->GetPosition().z - start.z, 5.0f);
        EXPECT_LT(std::fabs(rig.car->GetPosition().x - start.x), 0.5f); // no steer -> straight line
        EXPECT_GE(rig.vehicle->GetCurrentGear(), 1);
        EXPECT_GT(rig.vehicle->GetEngineRPM(), kMinEngineRPM);
        EXPECT_TRUE(rig.AllWheelsOnGround());

        // 0.5 s full brake. Grip is tyre friction (1.0) combined with the ground's default
        // 0.5 as sqrt(1.0 * 0.5), so the tyre-limited ceiling is ~0.7 g (~6.9 m/s^2). Require
        // a hard stop well above that of a coasting car (measured in the next block).
        rig.Drive({0.0f, 1.0f, 0.0f, 0.0f}, 30);
        const float brakingDecel = (cruiseSpeed - rig.ForwardSpeed()) / 0.5f;
        EXPECT_GT(brakingDecel, 4.0f);
        {
            VehicleRig coastRig;
            ASSERT_TRUE(coastRig.ready);
            coastRig.Settle();
            coastRig.Drive({1.0f, 0.0f, 0.0f, 0.0f}, 180);
            const float coastStart = coastRig.ForwardSpeed();
            coastRig.Drive({}, 30); // 0.5 s off throttle, no brake
            const float coastDecel = (coastStart - coastRig.ForwardSpeed()) / 0.5f;
            EXPECT_GT(brakingDecel, 2.0f * coastDecel);
        }
        rig.Drive({0.0f, 1.0f, 0.0f, 0.0f}, 240);
        EXPECT_LT(std::fabs(rig.ForwardSpeed()), 0.25f);

        // Negative throttle selects reverse through the automatic transmission.
        const float zBeforeReverse = rig.car->GetPosition().z;
        rig.Drive({-1.0f, 0.0f, 0.0f, 0.0f}, 120);
        EXPECT_LT(rig.ForwardSpeed(), -1.0f);
        EXPECT_LT(rig.car->GetPosition().z, zBeforeReverse - 1.0f);
        EXPECT_EQ(rig.vehicle->GetCurrentGear(), -1);
    }

    // Steering: the requested angle (radians) reaches the front wheels and
    // positive steer turns toward +X (right) in the engine's convention.
    for (const float steer : {0.25f, -0.25f})
    {
        VehicleRig rig;
        ASSERT_TRUE(rig.ready);
        rig.Settle();
        const float startYaw = rig.Yaw();
        const XMFLOAT3 start = rig.car->GetPosition();

        rig.Drive({0.5f, 0.0f, steer, 0.0f}, 1);
        EXPECT_NEAR(rig.vehicle->GetWheelSteerAngle(0), steer, 1e-4f);
        EXPECT_NEAR(rig.vehicle->GetWheelSteerAngle(1), steer, 1e-4f);
        EXPECT_NEAR(rig.vehicle->GetWheelSteerAngle(2), 0.0f, 1e-6f);
        EXPECT_NEAR(rig.vehicle->GetWheelSteerAngle(3), 0.0f, 1e-6f);

        rig.Step(119); // 2 s total under steer
        const float yawDelta = rig.Yaw() - startYaw;
        const float lateral = rig.car->GetPosition().x - start.x;
        EXPECT_GT(yawDelta * steer, 0.2f * std::fabs(steer)); // yaw sign follows steer sign
        EXPECT_GT(lateral * steer, 0.0f);
        EXPECT_GT(std::fabs(lateral), 0.5f);
        EXPECT_GT(rig.car->GetPosition().z - start.z, 1.0f);

        // Steering beyond the wheel limit clamps to it rather than wrapping.
        rig.Drive({0.0f, 0.0f, steer * 10.0f, 0.0f}, 1);
        EXPECT_NEAR(rig.vehicle->GetWheelSteerAngle(0), steer > 0.0f ? kFrontSteerLimit : -kFrontSteerLimit, 1e-4f);
    }
}

TEST(VehiclePhysics_JoltVehicleWakesFromSleepOnInput)
{
    VehicleRig rig;
    ASSERT_TRUE(rig.ready);
    rig.Settle();

    // Let Jolt put the parked car to sleep.
    for (int i = 0; i < 600 && rig.car->IsActive(); ++i)
    {
        rig.Step(1);
    }
    ASSERT_FALSE(rig.car->IsActive());

    const float zAsleep = rig.car->GetPosition().z;
    rig.Drive({1.0f, 0.0f, 0.0f, 0.0f}, 120);
    EXPECT_TRUE(rig.car->IsActive());
    EXPECT_GT(rig.ForwardSpeed(), 3.0f);
    EXPECT_GT(rig.car->GetPosition().z - zAsleep, 2.0f);
}

TEST(VehiclePhysics_JoltVehicleIsDeterministicUnderStepFixed)
{
    const std::vector<VehicleSample> first = RunScriptedDrive();
    const std::vector<VehicleSample> second = RunScriptedDrive();
    ASSERT_EQ(first.size(), static_cast<size_t>(21));
    EXPECT_TRUE(SamplesBitwiseEqual(first, second));

    // The script must actually move the car, or equality proves nothing.
    const VehicleSample& last = first.back();
    const float travelled = std::sqrt(last.position.x * last.position.x + last.position.z * last.position.z);
    EXPECT_GT(travelled, 5.0f);
}

TEST(VehiclePhysics_JoltTrackedVehicleDrivesAndSteersByTrackSpeed)
{
    // Both tracks driven: straight-line acceleration, then braking to a stop.
    {
        VehicleRig rig(MakeTankDesc());
        ASSERT_TRUE(rig.ready);
        EXPECT_EQ(rig.vehicle->GetWheelCount(), 10u);
        rig.Settle();
        EXPECT_TRUE(rig.AllWheelsOnGround());
        const XMFLOAT3 start = rig.car->GetPosition();

        rig.Drive({1.0f, 0.0f, 0.0f, 0.0f}, 180); // 3 s full throttle
        const float cruiseSpeed = rig.ForwardSpeed();
        EXPECT_GT(cruiseSpeed, 2.0f);
        EXPECT_GT(rig.car->GetPosition().z - start.z, 2.0f);
        EXPECT_LT(std::fabs(rig.car->GetPosition().x - start.x), 0.25f); // equal track speeds -> straight
        EXPECT_LT(std::fabs(rig.Yaw()), 0.05f);
        EXPECT_GE(rig.vehicle->GetCurrentGear(), 1);
        EXPECT_GT(rig.vehicle->GetEngineRPM(), kMinEngineRPM);

        rig.Drive({0.0f, 1.0f, 0.0f, 0.0f}, 120);
        EXPECT_LT(std::fabs(rig.ForwardSpeed()), 0.25f);
    }

    // Steering while driving: the inner track slows, so the hull yaws toward the steer side
    // (+X is right in the engine's convention) and drifts that way while still moving forward.
    for (const float steer : {0.2f, -0.2f})
    {
        VehicleRig rig(MakeTankDesc());
        ASSERT_TRUE(rig.ready);
        rig.Settle();
        const XMFLOAT3 start = rig.car->GetPosition();

        const float turned = rig.DriveMeasuringTurn({1.0f, 0.0f, steer, 0.0f}, 180);
        const float lateral = rig.car->GetPosition().x - start.x;
        EXPECT_GT(turned * steer, 0.0f);
        EXPECT_GT(std::fabs(turned), 0.2f);
        EXPECT_GT(lateral * steer, 0.0f);
        EXPECT_GT(rig.car->GetPosition().z - start.z, 1.0f);
    }

    // Pivot turn: steer with no throttle or brake while stopped counter-rotates the tracks,
    // turning the hull in place; the steer amount sets the turn rate.
    for (const float direction : {1.0f, -1.0f})
    {
        float turnedBySteer[2] = {};
        const float steers[2] = {0.1f * direction, 0.5f * direction}; // 20% and 100% of trackedFullSteerAngle
        for (int i = 0; i < 2; ++i)
        {
            VehicleRig rig(MakeTankDesc());
            ASSERT_TRUE(rig.ready);
            rig.Settle();
            const XMFLOAT3 start = rig.car->GetPosition();

            turnedBySteer[i] = rig.DriveMeasuringTurn({0.0f, 0.0f, steers[i], 0.0f}, 30); // 0.5 s
            EXPECT_GT(turnedBySteer[i] * direction, 0.0f);
            const float dx = rig.car->GetPosition().x - start.x;
            const float dz = rig.car->GetPosition().z - start.z;
            EXPECT_LT(std::sqrt(dx * dx + dz * dz), 0.5f);
        }
        EXPECT_GT(std::fabs(turnedBySteer[1]), 0.3f);
        EXPECT_GT(std::fabs(turnedBySteer[1]), 1.5f * std::fabs(turnedBySteer[0]));
    }
}

TEST(VehiclePhysics_TrackedVehicleRejectsLayoutsWithoutTwoTracks)
{
    // Every wheel on one side leaves the other track empty.
    {
        VehicleDesc oneSided = MakeTankDesc();
        for (VehicleWheelDesc& wheel : oneSided.wheels)
        {
            wheel.position.x = std::fabs(wheel.position.x);
        }
        VehicleRig rig(oneSided);
        EXPECT_FALSE(rig.ready);
        EXPECT_TRUE(rig.vehicle == nullptr);
    }

    // A centred wheel belongs to neither track.
    {
        VehicleDesc centred = MakeTankDesc();
        centred.wheels[2].position.x = 0.0f;
        VehicleRig rig(centred);
        EXPECT_FALSE(rig.ready);
        EXPECT_TRUE(rig.vehicle == nullptr);
    }

    // The same rig with a valid layout still builds, so the failures above are the layouts'.
    VehicleRig valid(MakeTankDesc());
    EXPECT_TRUE(valid.ready);
}

#endif // SPARK_TEST_HAS_PHYSICS
