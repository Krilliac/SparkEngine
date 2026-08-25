/**
 * @file RacingRaceFlow.cpp
 * @brief Module-independent racing flow helpers shared by runtime orchestration and tests.
 */

#include "RacingRaceFlow.h"
#include "Race/RacingRaceManager.h"
#include "Track/RacingTrackSystem.h"
#include "Vehicle/RacingVehicleSystem.h"
#include <algorithm>
#include <cmath>

namespace Racing
{
    RaceControlEdges PollRaceControlEdges(bool restartDown, bool cameraDown, bool& restartHeld, bool& cameraCycleHeld)
    {
        RaceControlEdges edges;
        edges.restartRequested = restartDown && !restartHeld;
        edges.cameraCycleRequested = cameraDown && !cameraCycleHeld;
        restartHeld = restartDown;
        cameraCycleHeld = cameraDown;
        return edges;
    }

    bool ProcessOrderedCheckpoint(RacingRaceManager& raceManager, const TrackData& track, uint32_t vehicleId,
                                  uint32_t checkpointIndex)
    {
        if (checkpointIndex >= track.checkpoints.size() || track.checkpoints.empty())
            return false;

        const RacerState* racer = raceManager.GetRacer(vehicleId);
        if (!racer || racer->finished || racer->dnf)
            return false;

        const uint32_t predecessor =
            checkpointIndex == 0 ? static_cast<uint32_t>(track.checkpoints.size() - 1) : checkpointIndex - 1;
        if (racer->lastCheckpoint != predecessor || (checkpointIndex == 0 && racer->lastCheckpoint == 0))
            return false;

        raceManager.OnCheckpointCrossed(vehicleId, checkpointIndex);
        const RacerState* updated = raceManager.GetRacer(vehicleId);
        if (!updated || updated->lastCheckpoint != checkpointIndex)
            return false;

        if (!track.checkpoints[checkpointIndex].isFinishLine)
            return false;

        raceManager.OnLapCompleted(vehicleId);
        return true;
    }

    float ComputeTrackSteer(const VehicleInstance& vehicle, const RacingTrackSystem& trackSystem)
    {
        if (trackSystem.GetCurrentTrack().waypoints.empty())
            return 0.0f;

        const uint32_t nearestWaypoint = trackSystem.GetNearestWaypoint(vehicle.positionX, vehicle.positionZ);
        const auto& target = trackSystem.GetWaypoint(nearestWaypoint + 2);
        const float desiredHeading = std::atan2(target.x - vehicle.positionX, target.z - vehicle.positionZ);
        const float headingError =
            std::atan2(std::sin(desiredHeading - vehicle.heading), std::cos(desiredHeading - vehicle.heading));
        return std::clamp(headingError * 1.5f, -1.0f, 1.0f);
    }

    bool StopTerminalRacer(const RacingRaceManager& raceManager, RacingVehicleSystem& vehicleSystem, uint32_t vehicleId)
    {
        const RacerState* racer = raceManager.GetRacer(vehicleId);
        if (!racer || (!racer->finished && !racer->dnf))
            return false;

        vehicleSystem.NeutralizeVehicle(vehicleId);
        return true;
    }
} // namespace Racing
