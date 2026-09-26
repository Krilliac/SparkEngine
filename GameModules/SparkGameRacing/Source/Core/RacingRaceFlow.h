/**
 * @file RacingRaceFlow.h
 * @brief Testable race-flow decisions shared by the module orchestration.
 */

#pragma once

#include <cstdint>

namespace Spark
{
    class IEngineContext;
}

namespace Racing
{
    class RacingAIDriver;
    class RacingRaceManager;
    class RacingTrackSystem;
    class RacingVehicleSystem;
    struct TrackData;
    struct VehicleInstance;

    /// Non-owning view of the systems one race session drives each frame.
    struct RaceSimulation
    {
        RacingVehicleSystem& vehicles;
        RacingTrackSystem& track;
        RacingRaceManager& race;
        RacingAIDriver& ai;
    };

    /// Player driving controls sampled for one frame.
    struct PlayerDriveInput
    {
        float throttle = 0.0f;
        float brake = 0.0f;
        float steer = 0.0f;
        bool nitro = false;
        bool drift = false;
    };

    struct RaceControlEdges
    {
        bool restartRequested = false;
        bool cameraCycleRequested = false;
    };

    /// Update the non-driving control latches and report newly pressed controls.
    RaceControlEdges PollRaceControlEdges(bool restartDown, bool cameraDown, bool& restartHeld, bool& cameraCycleHeld);

    /// Apply one ordered checkpoint crossing and complete a lap only at the track-authored finish line.
    bool ProcessOrderedCheckpoint(RacingRaceManager& raceManager, const TrackData& track, uint32_t vehicleId,
                                  uint32_t checkpointIndex);

    /// Compute normalized steering toward the next authored track waypoint.
    float ComputeTrackSteer(const VehicleInstance& vehicle, const RacingTrackSystem& trackSystem);

    /// Park a finished or DNF racer's vehicle (out of the physics world) while other racers continue.
    bool StopTerminalRacer(const RacingRaceManager& raceManager, RacingVehicleSystem& vehicleSystem,
                           uint32_t vehicleId);

    /// Fastest speed (km/h) from which the vehicle can still brake for every corner within its braking horizon
    /// on the authored centerline (circumradius of centerline samples against a fixed usable lateral grip).
    float ComputeCornerSpeedLimit(const VehicleInstance& vehicle, const RacingTrackSystem& trackSystem);

    /// Rebuild the player + AI roster as Jolt vehicles on the current track's starting grid and begin the
    /// countdown. Returns false when the vehicle system has no live physics world or a chassis cannot be built.
    bool SetupRaceRoster(const RaceSimulation& sim, Spark::IEngineContext* context);

    /// Advance one variable-rate race frame: race clock, track progress/checkpoints, AI, and driving inputs.
    /// @param playerInput Player controls for this frame, or nullptr when no input device is available.
    void StepRaceFrame(const RaceSimulation& sim, const PlayerDriveInput* playerInput, float deltaTime);
} // namespace Racing
