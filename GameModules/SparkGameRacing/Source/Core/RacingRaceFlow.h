/**
 * @file RacingRaceFlow.h
 * @brief Testable race-flow decisions shared by the module orchestration.
 */

#pragma once

#include <cstdint>

namespace Racing
{
    class RacingRaceManager;
    class RacingTrackSystem;
    class RacingVehicleSystem;
    struct TrackData;
    struct VehicleInstance;

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

    /// Neutralize and stop a finished or DNF racer's vehicle while other racers continue.
    bool StopTerminalRacer(const RacingRaceManager& raceManager, RacingVehicleSystem& vehicleSystem,
                           uint32_t vehicleId);
} // namespace Racing
