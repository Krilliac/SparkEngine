/**
 * @file RacingVehicleSystem.h
 * @brief Jolt-backed vehicle driving for the racing showcase
 * @author Spark Engine Team
 * @date 2026
 *
 * Every racer is a dynamic Jolt body with a four-wheel VehicleConstraint created
 * through the engine's shared PhysicsSystem (PhysicsSystem::CreateVehicle). The
 * per-type VehicleStats become the chassis mass, engine torque, gearing, brake
 * torque and steering lock; surface grip comes from the friction of the track
 * colliders the wheels touch (RacingTrackSystem builds them).
 *
 * ### Contract
 * - **Thread affinity:** game thread only (the PhysicsSystem is main-thread only).
 * - **Stepping:** FixedUpdate() is the Racing process's single physics stepping
 *   owner (PhysicsSystem "module-driven stepping" contract): it pushes the latched
 *   driver commands into Jolt, advances the shared world by exactly one
 *   PhysicsSystem::StepFixed() tick of the engine fixed timestep, and reads the
 *   stepped poses back into VehicleInstance. Nothing else in the module steps.
 * - **Ownership:** owns one Jolt body + vehicle constraint per active vehicle and
 *   destroys them in RemoveVehicle(), NeutralizeVehicle(), RestoreState() and
 *   Shutdown(). The PhysicsSystem itself is engine-owned and must outlive this
 *   system.
 * - **Allocation:** bodies are allocated when vehicles are created or restored;
 *   the fixed step and the input path allocate nothing.
 * - **Scalability:** one race grid (a handful of vehicles); vehicle lookup is linear.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RacingEnums.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class PhysicsBody;
class PhysicsSystem;
class VehiclePhysics;

namespace Racing
{

    /**
     * @brief Performance profile for a vehicle type
     *
     * Ratings are normalized 0.0 - 1.0 relative to the best-in-class value.
     */
    struct VehicleStats
    {
        float maxSpeed = 200.0f;   ///< Governed top speed in km/h (nitro/drift boost may exceed it by 30%)
        float acceleration = 0.7f; ///< 0-1 rating; scales engine torque
        float handling = 0.7f;     ///< 0-1 rating; scales the front-wheel steering lock
        float braking = 0.7f;      ///< 0-1 rating; scales wheel brake torque
        float weight = 1200.0f;    ///< Chassis mass in kg
        float durability = 100.0f; ///< Hit points before performance loss
        float driftBonus = 1.0f;   ///< Multiplier for drift boost charge rate
    };

    /// World placement of a vehicle: ground-contact position and yaw (0 = +Z, positive toward +X).
    struct VehiclePose
    {
        float x = 0.0f;
        float y = 0.0f; ///< Height of the ground under the vehicle
        float z = 0.0f;
        float heading = 0.0f; ///< Yaw in radians
    };

    /**
     * @brief Runtime state of a single vehicle in the race
     *
     * The pose, speed and rpm fields mirror the Jolt chassis: FixedUpdate() rewrites them
     * after every physics tick, so gameplay moves a vehicle through SetVehiclePose(), never
     * by writing them.
     */
    struct VehicleInstance
    {
        uint32_t id = 0;
        std::string name;
        VehicleType type = VehicleType::SportsCar;
        VehicleStats baseStats;

        // Chassis state read back from the physics world
        float positionX = 0.0f;  ///< World X
        float positionY = 0.0f;  ///< Ground-contact height (chassis position minus ride height)
        float positionZ = 0.0f;  ///< World Z
        float heading = 0.0f;    ///< Yaw in radians
        float speed = 0.0f;      ///< Forward speed km/h (never negative)
        float rpm = 0.0f;        ///< Engine RPM for sound mapping
        float steerAngle = 0.0f; ///< Latched normalized steering [-1, 1]

        // Driver command latched by ApplyInput*; consumed by every fixed tick
        float throttleInput = 0.0f; ///< [0, 1]
        float brakeInput = 0.0f;    ///< [0, 1]

        // Subsystems
        DriftState driftState = DriftState::None;
        float driftCharge = 0.0f;  ///< 0-1 boost charge from drifting
        float boostTimer = 0.0f;   ///< Remaining boost time in seconds
        float nitro = 1.0f;        ///< 0-1 nitro tank level
        float damage = 0.0f;       ///< Accumulated damage (0 = pristine)
        float strandedTime = 0.0f; ///< Seconds the chassis has been upside down or pinned under full throttle
        SurfaceType currentSurface = SurfaceType::Asphalt;

        bool isPlayer = false;
        bool isActive = true;
    };

    /**
     * @brief Owns the racing field's Jolt vehicles and drives the shared physics step
     */
    class RacingVehicleSystem
    {
      public:
        RacingVehicleSystem();
        ~RacingVehicleSystem();

        RacingVehicleSystem(const RacingVehicleSystem&) = delete;
        RacingVehicleSystem& operator=(const RacingVehicleSystem&) = delete;

        /// Bind to the engine's shared PhysicsSystem. Fails when the context has no live Jolt world.
        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);

        /// Push driver commands into Jolt, advance the shared world one fixed tick, read the poses back.
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();

        void RenderDebugUI();

        /// Create a vehicle resting on the ground at @p pose; returns its ID, or 0 when no chassis could be built.
        uint32_t CreateVehicle(const std::string& name, VehicleType type, bool isPlayer, const VehiclePose& pose);

        /// Remove a vehicle and its chassis by ID
        void RemoveVehicle(uint32_t id);

        /// Teleport a vehicle's chassis to @p pose at rest (grid placement and track recovery).
        bool SetVehiclePose(uint32_t id, const VehiclePose& pose);

        /// Scale a vehicle's chassis velocity (grip-loss hazards such as oil slicks).
        void ScaleVelocity(uint32_t id, float factor);

        /// Apply player input to the player vehicle
        void ApplyInput(float throttle, float brake, float steer, bool nitroPressed, bool driftPressed,
                        float deltaTime = 1.0f / 60.0f);

        /// Apply input to a specific vehicle (used by AI and tools)
        void ApplyInputToVehicle(uint32_t vehicleId, float throttle, float brake, float steer, bool nitroPressed,
                                 bool driftPressed, float deltaTime = 1.0f / 60.0f);

        /// Park a finished or retired vehicle: clear its drive state and take its chassis out of the physics
        /// world so it no longer blocks the racers still on track. It keeps its last pose for presentation.
        void NeutralizeVehicle(uint32_t vehicleId);

        /// True while the vehicle has a live Jolt chassis.
        bool HasChassis(uint32_t vehicleId) const;

        VehicleInstance* GetVehicle(uint32_t id);
        const VehicleInstance* GetVehicle(uint32_t id) const;
        VehicleInstance* GetPlayerVehicle();
        std::vector<VehicleInstance>& GetVehiclesMutable() { return m_vehicles; }
        const std::vector<VehicleInstance>& GetVehicles() const { return m_vehicles; }
        size_t GetVehicleCount() const { return m_vehicles.size(); }

        /// Physics ticks this system has advanced the shared world by since Initialize().
        uint64_t GetStepCount() const { return m_stepCount; }

        /** Check a persistence snapshot (IDs, enums, finite ranges, at most one player) without applying it. */
        static bool ValidateSnapshot(const std::vector<VehicleInstance>& vehicles);

        /** Replace every vehicle from a validated snapshot, rebuilding each chassis at its saved pose and speed. */
        bool RestoreState(const std::vector<VehicleInstance>& vehicles);

        /// Get default stats for a vehicle type
        static VehicleStats GetDefaultStats(VehicleType type);

        /// Get the collider friction for a surface type (combined with the tyre friction by Jolt)
        static float GetSurfaceGrip(SurfaceType surface);

        std::string GetVehicleListString() const;

      private:
        /// Jolt objects behind one vehicle; the constraint must be destroyed before its body.
        struct Chassis
        {
            std::shared_ptr<PhysicsBody> body;
            std::unique_ptr<VehiclePhysics> vehicle;
            float maxSteerAngle = 0.0f; ///< Front-wheel lock in radians
        };

        void ApplyInputInternal(VehicleInstance& vehicle, float throttle, float brake, float steer, bool nitroPressed,
                                bool driftPressed, float deltaTime);
        bool BuildChassis(VehicleInstance& vehicle, const VehiclePose& pose, float initialSpeedKmh);
        void DestroyChassis(uint32_t vehicleId);
        void DriveChassis(const VehicleInstance& vehicle, Chassis& chassis);
        void ReadBackChassis(VehicleInstance& vehicle, const Chassis& chassis, float dt);
        void UpdateDriftState(VehicleInstance& vehicle, float steer, bool driftPressed, float dt);

        Spark::IEngineContext* m_context{nullptr};
        PhysicsSystem* m_physics{nullptr}; ///< Engine-owned shared world; non-owning
        std::vector<VehicleInstance> m_vehicles;
        std::unordered_map<uint32_t, Chassis> m_chassis;
        uint32_t m_nextId{1};
        uint64_t m_stepCount{0};
        bool m_initialized{false};
    };

} // namespace Racing
