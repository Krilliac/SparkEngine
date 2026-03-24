/**
 * @file RacingAIDriver.h
 * @brief AI racer behavior with difficulty scaling and rubber-banding
 * @author Spark Engine Team
 * @date 2026
 *
 * Each AI driver follows the track spline with look-ahead, adjusts speed
 * for corners, avoids collisions with nearby vehicles, and attempts
 * overtaking maneuvers. Difficulty levels control speed cap, cornering
 * accuracy, and reaction time. Rubber-banding adjusts AI speed based
 * on distance to the player.
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
     * @brief AI difficulty presets
     */
    enum class AIDifficulty : uint8_t
    {
        Easy = 0,   ///< 70% speed cap, wide racing line
        Medium = 1, ///< 85% speed cap, reasonable lines
        Hard = 2,   ///< 95% speed cap, tight racing line
        Expert = 3, ///< 100% speed cap, optimal racing line
        Count = 4
    };

    /**
     * @brief Configuration for a single AI driver
     */
    struct AIDriverConfig
    {
        uint32_t vehicleId = 0;
        std::string name;
        AIDifficulty difficulty = AIDifficulty::Medium;
        VehicleType preferredVehicle = VehicleType::SportsCar;

        float speedFactor = 0.85f;   ///< Fraction of vehicle max speed
        float lineAccuracy = 0.7f;   ///< How closely AI follows ideal line (0-1)
        float reactionTime = 0.15f;  ///< Delay before responding to events (seconds)
        float aggressiveness = 0.5f; ///< Willingness to overtake (0-1)
    };

    /**
     * @brief Runtime state for an AI driver
     */
    struct AIDriverState
    {
        uint32_t vehicleId = 0;
        uint32_t targetWaypoint = 0; ///< Next waypoint to steer toward
        uint32_t lookAheadCount = 3; ///< How many waypoints ahead to plan

        float throttle = 0.0f;
        float brake = 0.0f;
        float steer = 0.0f;
        bool useNitro = false;
        bool useDrift = false;

        float rubberBandFactor = 1.0f; ///< Speed adjustment from rubber-banding
        float reactionDelay = 0.0f;    ///< Remaining reaction delay

        // Overtaking
        bool isOvertaking = false;
        float overtakeOffset = 0.0f; ///< Lateral offset for passing maneuver
        float overtakeTimer = 0.0f;
    };

    /**
     * @brief Controls all AI racers each frame
     *
     * For each AI driver, computes steering toward the next waypoint,
     * adjusts throttle/brake for upcoming corners, and applies
     * rubber-banding to keep races competitive.
     */
    class RacingAIDriver
    {
      public:
        RacingAIDriver() = default;
        ~RacingAIDriver() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();

        void RenderDebugUI();

        /// Add an AI driver for a vehicle
        void AddDriver(const AIDriverConfig& config);

        /// Remove an AI driver by vehicle ID
        void RemoveDriver(uint32_t vehicleId);

        /// Set global difficulty for all AI drivers
        void SetGlobalDifficulty(AIDifficulty difficulty);

        /// Update rubber-banding based on player distance to each AI
        void UpdateRubberBanding(float playerDistance, float leadDistance, float lastDistance);

        /// Get the computed input for a specific AI vehicle
        const AIDriverState* GetDriverState(uint32_t vehicleId) const;

        const std::vector<AIDriverConfig>& GetDrivers() const { return m_drivers; }
        size_t GetDriverCount() const { return m_drivers.size(); }
        AIDifficulty GetGlobalDifficulty() const { return m_globalDifficulty; }

        std::string GetDriverListString() const;

      private:
        void UpdateDriver(AIDriverConfig& config, AIDriverState& state, float dt);
        void ComputeSteering(const AIDriverConfig& config, AIDriverState& state);
        void ComputeThrottle(const AIDriverConfig& config, AIDriverState& state);
        void ComputeOvertaking(const AIDriverConfig& config, AIDriverState& state, float dt);

        static AIDriverConfig MakeConfigForDifficulty(AIDifficulty difficulty);

        Spark::IEngineContext* m_context{nullptr};
        std::vector<AIDriverConfig> m_drivers;
        std::vector<AIDriverState> m_states;
        AIDifficulty m_globalDifficulty = AIDifficulty::Medium;
        bool m_initialized{false};
    };

} // namespace Racing
