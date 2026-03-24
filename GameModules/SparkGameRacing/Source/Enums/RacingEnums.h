/**
 * @file RacingEnums.h
 * @brief Enumerations for racing gameplay systems
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains all enum types used by the racing game module: vehicle types,
 * surface materials, race states, camera modes, weather, and drift states.
 */

#pragma once

#include <cstdint>

namespace Racing
{

    /**
     * @brief Vehicle class archetypes with distinct handling profiles
     */
    enum class VehicleType : uint8_t
    {
        SportsCar = 0, ///< Balanced speed and handling
        MuscleCar = 1, ///< High straight-line speed, heavy
        SuperCar = 2,  ///< Top speed, fragile
        OffRoad = 3,   ///< Best grip on dirt/gravel, slower on asphalt
        Formula = 4,   ///< Best handling and braking, low durability
        Kart = 5,      ///< Lightweight, quick acceleration, low top speed
        Count = 6
    };

    /**
     * @brief Track surface materials affecting tire grip and handling
     */
    enum class SurfaceType : uint8_t
    {
        Asphalt = 0, ///< Standard road — best grip for road cars
        Dirt = 1,    ///< Loose surface — reduced grip, easier drift
        Grass = 2,   ///< Off-track — significant speed penalty
        Sand = 3,    ///< Desert — heavy drag and low grip
        Ice = 4,     ///< Frozen — minimal grip, long braking distance
        Gravel = 5,  ///< Loose stones — moderate grip, noisy
        Count = 6
    };

    /**
     * @brief Race lifecycle states
     */
    enum class RaceState : uint8_t
    {
        Countdown = 0, ///< Pre-race countdown (3, 2, 1, GO)
        Racing = 1,    ///< Active race in progress
        Paused = 2,    ///< Race paused
        Finished = 3,  ///< Race completed normally
        DNF = 4,       ///< Did Not Finish (timeout or disqualification)
        Count = 5
    };

    /**
     * @brief Camera view modes for the racing camera system
     */
    enum class CameraMode : uint8_t
    {
        Chase = 0,     ///< Third-person behind vehicle
        Cockpit = 1,   ///< First-person inside vehicle
        Hood = 2,      ///< On-hood forward view
        Orbit = 3,     ///< Free-look orbit around vehicle
        Cinematic = 4, ///< Replay and spectator angles
        Count = 5
    };

    /**
     * @brief Weather conditions affecting visibility and grip
     */
    enum class WeatherCondition : uint8_t
    {
        Clear = 0, ///< Perfect conditions
        Rain = 1,  ///< Reduced grip, spray effects
        Snow = 2,  ///< Low grip, poor visibility
        Fog = 3,   ///< Reduced draw distance
        Night = 4, ///< Headlights required, limited visibility
        Count = 5
    };

    /**
     * @brief Drift state machine for the drift/boost mechanic
     */
    enum class DriftState : uint8_t
    {
        None = 0,       ///< Normal driving
        Initiating = 1, ///< Entering a drift (brake + steer)
        Drifting = 2,   ///< Active drift — building boost charge
        Boosting = 3,   ///< Post-drift speed boost active
        Count = 4
    };

} // namespace Racing
