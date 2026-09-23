/**
 * @file RacingRaceFlow.cpp
 * @brief Module-independent racing flow helpers shared by runtime orchestration and tests.
 */

#include "RacingRaceFlow.h"
#include "AI/RacingAIDriver.h"
#include "Race/RacingRaceManager.h"
#include "Track/RacingTrackSystem.h"
#include "Vehicle/RacingVehicleSystem.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace Racing
{
    namespace
    {
        AIDriverConfig MakeRosterDriverConfig(uint32_t vehicleId, const char* name, VehicleType type,
                                              AIDifficulty difficulty)
        {
            AIDriverConfig config{};
            config.vehicleId = vehicleId;
            config.name = name;
            config.preferredVehicle = type;
            config.difficulty = difficulty;

            switch (difficulty)
            {
            case AIDifficulty::Easy:
                config.speedFactor = 0.70f;
                config.lineAccuracy = 0.50f;
                config.reactionTime = 0.30f;
                config.aggressiveness = 0.2f;
                break;
            case AIDifficulty::Medium:
                config.speedFactor = 0.85f;
                config.lineAccuracy = 0.70f;
                config.reactionTime = 0.15f;
                config.aggressiveness = 0.5f;
                break;
            case AIDifficulty::Hard:
                config.speedFactor = 0.95f;
                config.lineAccuracy = 0.85f;
                config.reactionTime = 0.08f;
                config.aggressiveness = 0.7f;
                break;
            case AIDifficulty::Expert:
                config.speedFactor = 1.0f;
                config.lineAccuracy = 0.95f;
                config.reactionTime = 0.03f;
                config.aggressiveness = 0.9f;
                break;
            case AIDifficulty::Count:
                break;
            }
            return config;
        }

        /// Surfaces, hazards, progress distance, ordered checkpoints, standings, and rubber-banding.
        void SyncRaceProgress(const RaceSimulation& sim)
        {
            const TrackData& currentTrack = sim.track.GetCurrentTrack();
            const size_t waypointCount = currentTrack.waypoints.size();
            if (waypointCount == 0)
                return;

            float playerDistance = 0.0f;
            float leadDistance = 0.0f;
            float lastDistance = 0.0f;
            bool hasPlayer = false;
            bool hasDistanceRange = false;

            for (VehicleInstance& vehicle : sim.vehicles.GetVehiclesMutable())
            {
                if (!vehicle.isActive)
                    continue;

                if (StopTerminalRacer(sim.race, sim.vehicles, vehicle.id))
                    continue;

                vehicle.currentSurface = sim.track.GetSurfaceAt(vehicle.positionX, vehicle.positionZ);

                const int hazard = sim.track.CheckHazard(vehicle.positionX, vehicle.positionZ);
                if (hazard >= 0)
                {
                    const TrackHazard& hazardData = currentTrack.hazards[static_cast<size_t>(hazard)];
                    if (hazardData.type == TrackHazard::Type::OilSlick)
                        vehicle.speed *= 0.96f;
                    else if (hazardData.type == TrackHazard::Type::SpeedBoost)
                        vehicle.boostTimer = std::max(vehicle.boostTimer, 0.6f);
                }

                const uint32_t nearestWaypoint = sim.track.GetNearestWaypoint(vehicle.positionX, vehicle.positionZ);
                const RacerState* racer = sim.race.GetRacer(vehicle.id);
                const float progressDistance =
                    static_cast<float>(racer ? racer->currentLap : 0u) * static_cast<float>(waypointCount) +
                    static_cast<float>(nearestWaypoint);
                sim.race.UpdateRacerDistance(vehicle.id, progressDistance);

                if (!hasDistanceRange)
                {
                    leadDistance = progressDistance;
                    lastDistance = progressDistance;
                    hasDistanceRange = true;
                }
                else
                {
                    leadDistance = std::max(leadDistance, progressDistance);
                    lastDistance = std::min(lastDistance, progressDistance);
                }

                if (vehicle.isPlayer)
                {
                    playerDistance = progressDistance;
                    hasPlayer = true;
                }

                const int checkpoint = sim.track.CheckCheckpoint(vehicle.positionX, vehicle.positionZ);
                if (checkpoint >= 0)
                    ProcessOrderedCheckpoint(sim.race, currentTrack, vehicle.id, static_cast<uint32_t>(checkpoint));
            }

            if (!hasPlayer)
                playerDistance = lastDistance;
            if (!hasDistanceRange)
                return;

            sim.race.RefreshPositions();
            sim.ai.UpdateRubberBanding(playerDistance, leadDistance, lastDistance);
        }

        void ApplyPlayerDrive(const RaceSimulation& sim, const PlayerDriveInput* playerInput, float deltaTime)
        {
            const VehicleInstance* player = sim.vehicles.GetPlayerVehicle();
            if (player && StopTerminalRacer(sim.race, sim.vehicles, player->id))
                return;

            if (!playerInput)
                return;

            if (sim.race.GetState() != RaceState::Racing)
            {
                sim.vehicles.ApplyInput(0.0f, 0.0f, 0.0f, false, false, deltaTime);
                return;
            }

            sim.vehicles.ApplyInput(playerInput->throttle, playerInput->brake, playerInput->steer, playerInput->nitro,
                                    playerInput->drift, deltaTime);
        }

        void DriveAIRacers(const RaceSimulation& sim, float deltaTime)
        {
            for (const VehicleInstance& vehicle : sim.vehicles.GetVehicles())
            {
                if (vehicle.isPlayer)
                    continue;

                if (StopTerminalRacer(sim.race, sim.vehicles, vehicle.id))
                    continue;

                const AIDriverState* aiState = sim.ai.GetDriverState(vehicle.id);
                if (!aiState)
                    continue;

                if (sim.race.GetState() != RaceState::Racing)
                {
                    sim.vehicles.ApplyInputToVehicle(vehicle.id, 0.0f, 0.0f, 0.0f, false, false, deltaTime);
                    continue;
                }

                const float trackSteer = ComputeTrackSteer(vehicle, sim.track);
                sim.vehicles.ApplyInputToVehicle(vehicle.id, aiState->throttle, aiState->brake, trackSteer,
                                                 aiState->useNitro, aiState->useDrift, deltaTime);
            }
        }
    } // namespace

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

        // Pure pursuit on the centerline: aim at a point a speed-scaled distance ahead so the line holds
        // regardless of how far apart the authored waypoints are.
        constexpr float kMinLookAheadMeters = 12.0f;
        constexpr float kLookAheadSeconds = 0.6f;
        const float lookAhead = std::max(kMinLookAheadMeters, (vehicle.speed / 3.6f) * kLookAheadSeconds);
        const TrackProjection projection =
            trackSystem.ProjectOntoTrack(vehicle.positionX, vehicle.positionZ, vehicle.heading);
        float targetX = 0.0f;
        float targetZ = 0.0f;
        trackSystem.GetPointAhead(projection, lookAhead, targetX, targetZ);

        const float desiredHeading = std::atan2(targetX - vehicle.positionX, targetZ - vehicle.positionZ);
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

    void SetupRaceRoster(const RaceSimulation& sim, Spark::IEngineContext* context)
    {
        sim.vehicles.Shutdown();
        sim.vehicles.Initialize(context);
        sim.race.Shutdown();
        sim.race.Initialize(context);
        sim.ai.Shutdown();
        sim.ai.Initialize(context);

        const uint32_t playerId = sim.vehicles.CreateVehicle("Player", VehicleType::SportsCar, true);
        sim.race.RegisterRacer(playerId, "Player", true);

        struct AIDriverSeed
        {
            const char* name;
            VehicleType type;
            AIDifficulty difficulty;
        };

        constexpr std::array<AIDriverSeed, 5> kAIDrivers = {{
            {"Nova", VehicleType::Formula, AIDifficulty::Expert},
            {"Vex", VehicleType::SuperCar, AIDifficulty::Hard},
            {"Mara", VehicleType::SportsCar, AIDifficulty::Hard},
            {"Bolt", VehicleType::MuscleCar, AIDifficulty::Medium},
            {"Rift", VehicleType::OffRoad, AIDifficulty::Medium},
        }};

        for (const AIDriverSeed& seed : kAIDrivers)
        {
            const uint32_t vehicleId = sim.vehicles.CreateVehicle(seed.name, seed.type, false);
            sim.race.RegisterRacer(vehicleId, seed.name, false);
            sim.ai.AddDriver(MakeRosterDriverConfig(vehicleId, seed.name, seed.type, seed.difficulty));
        }

        const TrackData& track = sim.track.GetCurrentTrack();
        if (track.waypoints.size() >= 2)
        {
            const TrackWaypoint& start = track.waypoints[0];
            const TrackWaypoint& next = track.waypoints[1];
            const float heading = std::atan2(next.x - start.x, next.z - start.z);
            const float forwardX = std::sin(heading);
            const float forwardZ = std::cos(heading);
            const float rightX = std::cos(heading);
            const float rightZ = -std::sin(heading);

            size_t gridIndex = 0;
            for (VehicleInstance& vehicle : sim.vehicles.GetVehiclesMutable())
            {
                const float laneOffset = (gridIndex % 2 == 0) ? -2.25f : 2.25f;
                const float rowOffset = static_cast<float>(gridIndex / 2) * 5.0f;
                vehicle.positionX = start.x - forwardX * rowOffset + rightX * laneOffset;
                vehicle.positionY = start.y;
                vehicle.positionZ = start.z - forwardZ * rowOffset + rightZ * laneOffset;
                vehicle.heading = heading;
                ++gridIndex;
            }
        }

        const uint32_t totalLaps = std::max<uint32_t>(1u, track.totalLaps);
        sim.race.StartRace(RaceMode::SingleRace, totalLaps);
    }

    void StepRaceFrame(const RaceSimulation& sim, const PlayerDriveInput* playerInput, float deltaTime)
    {
        sim.race.Update(deltaTime);
        SyncRaceProgress(sim);
        for (const VehicleInstance& vehicle : sim.vehicles.GetVehicles())
        {
            if (!vehicle.isPlayer)
                sim.ai.SetTrackSteer(vehicle.id, ComputeTrackSteer(vehicle, sim.track));
        }
        sim.ai.Update(deltaTime);
        ApplyPlayerDrive(sim, playerInput, deltaTime);
        DriveAIRacers(sim, deltaTime);
        sim.vehicles.Update(deltaTime);
    }
} // namespace Racing
