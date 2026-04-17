/**
 * @file RacingVehicleSystem.h
 * @brief Physics-based vehicle driving model for the racing showcase
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages vehicle instances with type-specific performance stats,
 * surface-dependent tire grip, drift mechanics, nitro boost, and
 * a simple damage model affecting performance.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RacingEnums.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Racing
{

    /**
     * @brief Performance profile for a vehicle type
     *
     * Each stat is normalized 0.0 - 1.0 relative to the best-in-class value.
     */
    struct VehicleStats
    {
        float maxSpeed = 200.0f;   ///< Top speed in km/h
        float acceleration = 0.7f; ///< 0-1 acceleration rating
        float handling = 0.7f;     ///< 0-1 cornering grip
        float braking = 0.7f;      ///< 0-1 braking effectiveness
        float weight = 1200.0f;    ///< Mass in kg — affects collisions and grip
        float durability = 100.0f; ///< Hit points before performance loss
        float driftBonus = 1.0f;   ///< Multiplier for drift boost charge rate
    };

    /**
     * @brief Runtime state of a single vehicle in the race
     */
    struct VehicleInstance
    {
        uint32_t id = 0;
        std::string name;
        VehicleType type = VehicleType::SportsCar;
        VehicleStats baseStats;

        // Current physics state
        float positionX = 0.0f;  ///< World X
        float positionY = 0.0f;  ///< World Y (up)
        float positionZ = 0.0f;  ///< World Z
        float heading = 0.0f;    ///< Yaw in radians
        float speed = 0.0f;      ///< Current speed km/h
        float rpm = 0.0f;        ///< Engine RPM for sound mapping
        float steerAngle = 0.0f; ///< Current steering angle

        // Subsystems
        DriftState driftState = DriftState::None;
        float driftCharge = 0.0f; ///< 0-1 boost charge from drifting
        float boostTimer = 0.0f;  ///< Remaining boost time in seconds
        float nitro = 1.0f;       ///< 0-1 nitro tank level
        float damage = 0.0f;      ///< Accumulated damage (0 = pristine)
        SurfaceType currentSurface = SurfaceType::Asphalt;

        bool isPlayer = false;
        bool isActive = true;
    };

    /**
     * @brief Manages all vehicles, applies physics each frame
     *
     * The driving model is intentionally simplified for showcase purposes:
     * speed integrates acceleration minus drag, steering applies a yaw rate
     * proportional to handling and inversely proportional to speed, and
     * surface grip multipliers scale effective handling and braking.
     */
    class RacingVehicleSystem
    {
      public:
        RacingVehicleSystem() = default;
        ~RacingVehicleSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();

        void RenderDebugUI();

        /// Create a vehicle and return its ID
        uint32_t CreateVehicle(const std::string& name, VehicleType type, bool isPlayer = false);

        /// Remove a vehicle by ID
        void RemoveVehicle(uint32_t id);

        /// Apply player input to the player vehicle
        void ApplyInput(float throttle, float brake, float steer, bool nitroPressed, bool driftPressed);

        /// Apply input to a specific vehicle (used by AI and tools)
        void ApplyInputToVehicle(uint32_t vehicleId, float throttle, float brake, float steer, bool nitroPressed,
                                 bool driftPressed);

        VehicleInstance* GetVehicle(uint32_t id);
        const VehicleInstance* GetVehicle(uint32_t id) const;
        VehicleInstance* GetPlayerVehicle();
        std::vector<VehicleInstance>& GetVehiclesMutable() { return m_vehicles; }
        const std::vector<VehicleInstance>& GetVehicles() const { return m_vehicles; }
        size_t GetVehicleCount() const { return m_vehicles.size(); }

        /// Get default stats for a vehicle type
        static VehicleStats GetDefaultStats(VehicleType type);

        /// Get grip multiplier for a surface type
        static float GetSurfaceGrip(SurfaceType surface);

        std::string GetVehicleListString() const;

      private:
        void ApplyInputInternal(VehicleInstance& vehicle, float throttle, float brake, float steer, bool nitroPressed,
                                bool driftPressed);
        void UpdateVehiclePhysics(VehicleInstance& vehicle, float dt);
        void UpdateDriftState(VehicleInstance& vehicle, float steer, bool driftPressed, float dt);
        void ApplyDamage(VehicleInstance& vehicle, float amount);

        Spark::IEngineContext* m_context{nullptr};
        std::vector<VehicleInstance> m_vehicles;
        uint32_t m_nextId{1};
        bool m_initialized{false};
    };

} // namespace Racing
