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

        /// Put a rolled-over, pinned, or fallen chassis back on the centerline, facing the driving direction.
        void RecoverStrandedVehicle(const RaceSimulation& sim, const VehicleInstance& vehicle)
        {
            constexpr float kRecoverAfterSeconds = 3.0f;
            constexpr float kFallenDepthMeters = 4.0f;

            const TrackProjection projection =
                sim.track.ProjectOntoTrack(vehicle.positionX, vehicle.positionZ, vehicle.heading);
            const float roadHeight = sim.track.GetCenterlineHeight(projection);
            if (vehicle.strandedTime < kRecoverAfterSeconds && vehicle.positionY > roadHeight - kFallenDepthMeters)
                return;

            const TrackWaypoint& from = sim.track.GetWaypoint(projection.segment);
            const TrackWaypoint& to = sim.track.GetWaypoint(projection.segment + 1);
            const VehiclePose pose{from.x + (to.x - from.x) * projection.t, roadHeight,
                                   from.z + (to.z - from.z) * projection.t, sim.track.GetCenterlineHeading(projection)};
            sim.vehicles.SetVehiclePose(vehicle.id, pose);
        }

        /// Ordered checkpoint gate crossings, surfaces, hazards, progress distance, standings, and rubber-banding.
        void SyncRaceProgress(const RaceSimulation& sim)
        {
            const TrackData& currentTrack = sim.track.GetCurrentTrack();
            const size_t waypointCount = currentTrack.waypoints.size();
            if (waypointCount == 0)
                return;

            // Checkpoint gate entries the Jolt sensors reported during the physics ticks since the last frame, in
            // step order. Order validation rejects skipped or repeated gates, and finished or retired racers.
            for (const CheckpointCrossing& crossing : sim.track.TakeCheckpointCrossings())
            {
                const VehicleInstance* vehicle = sim.vehicles.GetVehicle(crossing.vehicleId);
                if (vehicle && vehicle->isActive)
                    ProcessOrderedCheckpoint(sim.race, currentTrack, crossing.vehicleId, crossing.checkpointIndex);
            }

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
                        sim.vehicles.ScaleVelocity(vehicle.id, 0.96f);
                    else if (hazardData.type == TrackHazard::Type::SpeedBoost)
                        vehicle.boostTimer = std::max(vehicle.boostTimer, 0.6f);
                }

                RecoverStrandedVehicle(sim, vehicle);

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

                // The AI's throttle/brake intent is capped by the braking-point planner so it arrives at every
                // corner at a speed the tyres can hold.
                const float trackSteer = ComputeTrackSteer(vehicle, sim.track);
                const float speedLimit = ComputeCornerSpeedLimit(vehicle, sim.track);
                float throttle = aiState->throttle;
                float brake = aiState->brake;
                if (vehicle.speed > speedLimit)
                {
                    throttle = 0.0f;
                    brake = std::max(brake, std::clamp((vehicle.speed - speedLimit) / 20.0f, 0.2f, 1.0f));
                }
                sim.vehicles.ApplyInputToVehicle(vehicle.id, throttle, brake, trackSteer, aiState->useNitro,
                                                 aiState->useDrift, deltaTime);
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

    float ComputeCornerSpeedLimit(const VehicleInstance& vehicle, const RacingTrackSystem& trackSystem)
    {
        constexpr float kUsableLateralAccel = 0.8f * 9.81f; ///< m/s^2 of cornering the planner relies on
        constexpr float kPlannedBrakeDecel = 5.0f;          ///< m/s^2 the planner brakes at before a corner
        constexpr float kChordMeters = 12.0f;               ///< Sample spacing for the curvature estimate
        constexpr float kStationStep = 6.0f;
        constexpr float kMaxHorizonMeters = 200.0f;
        constexpr float kUnlimited = 1000.0f; // km/h

        if (trackSystem.GetCurrentTrack().waypoints.size() < 3)
            return kUnlimited;

        const TrackProjection projection =
            trackSystem.ProjectOntoTrack(vehicle.positionX, vehicle.positionZ, vehicle.heading);
        const float speedMs = vehicle.speed / 3.6f;
        const float horizon =
            std::min(kMaxHorizonMeters, speedMs * speedMs / (2.0f * kPlannedBrakeDecel) + 2.0f * kChordMeters);

        float limitMs = kUnlimited / 3.6f;
        for (float station = 0.0f; station <= horizon; station += kStationStep)
        {
            float x0 = 0.0f, z0 = 0.0f, x1 = 0.0f, z1 = 0.0f, x2 = 0.0f, z2 = 0.0f;
            trackSystem.GetPointAhead(projection, station, x0, z0);
            trackSystem.GetPointAhead(projection, station + kChordMeters, x1, z1);
            trackSystem.GetPointAhead(projection, station + 2.0f * kChordMeters, x2, z2);

            // Circumradius of the three centerline samples: R = abc / (4 * area).
            const float a = std::hypot(x1 - x0, z1 - z0);
            const float b = std::hypot(x2 - x1, z2 - z1);
            const float c = std::hypot(x2 - x0, z2 - z0);
            const float twiceArea = std::fabs((x1 - x0) * (z2 - z0) - (z1 - z0) * (x2 - x0));
            if (twiceArea < 1.0e-3f)
                continue; // straight (or clamped at a point-to-point finish)
            const float radius = a * b * c / (2.0f * twiceArea);
            const float cornerSpeed = std::sqrt(kUsableLateralAccel * radius);

            // Fastest speed from which the car can still brake down to the corner speed by that station.
            limitMs = std::min(limitMs, std::sqrt(cornerSpeed * cornerSpeed + 2.0f * kPlannedBrakeDecel * station));
        }
        return limitMs * 3.6f;
    }

    bool StopTerminalRacer(const RacingRaceManager& raceManager, RacingVehicleSystem& vehicleSystem, uint32_t vehicleId)
    {
        const RacerState* racer = raceManager.GetRacer(vehicleId);
        if (!racer || (!racer->finished && !racer->dnf))
            return false;

        vehicleSystem.NeutralizeVehicle(vehicleId);
        return true;
    }

    bool SetupRaceRoster(const RaceSimulation& sim, Spark::IEngineContext* context)
    {
        sim.vehicles.Shutdown();
        sim.race.Shutdown();
        sim.race.Initialize(context);
        sim.ai.Shutdown();
        sim.ai.Initialize(context);
        sim.track.TakeCheckpointCrossings(); // gate entries from the previous race must not count in this one
        if (!sim.vehicles.Initialize(context))
            return false;

        struct RosterSeed
        {
            const char* name;
            VehicleType type;
            AIDifficulty difficulty;
        };

        // Grid slot 0 is the player; the AI field fills the remaining slots in order.
        constexpr std::array<RosterSeed, 6> kRoster = {{
            {"Player", VehicleType::SportsCar, AIDifficulty::Medium},
            {"Nova", VehicleType::Formula, AIDifficulty::Expert},
            {"Vex", VehicleType::SuperCar, AIDifficulty::Hard},
            {"Mara", VehicleType::SportsCar, AIDifficulty::Hard},
            {"Bolt", VehicleType::MuscleCar, AIDifficulty::Medium},
            {"Rift", VehicleType::OffRoad, AIDifficulty::Medium},
        }};

        // Two-wide staggered grid behind the first waypoint, facing the second.
        const TrackData& track = sim.track.GetCurrentTrack();
        VehiclePose gridOrigin{};
        if (!track.waypoints.empty())
        {
            gridOrigin = {track.waypoints[0].x, track.waypoints[0].y, track.waypoints[0].z, 0.0f};
            if (track.waypoints.size() >= 2)
                gridOrigin.heading =
                    std::atan2(track.waypoints[1].x - gridOrigin.x, track.waypoints[1].z - gridOrigin.z);
        }
        const float forwardX = std::sin(gridOrigin.heading);
        const float forwardZ = std::cos(gridOrigin.heading);
        const float rightX = std::cos(gridOrigin.heading);
        const float rightZ = -std::sin(gridOrigin.heading);

        for (size_t gridIndex = 0; gridIndex < kRoster.size(); ++gridIndex)
        {
            const RosterSeed& seed = kRoster[gridIndex];
            const float laneOffset = (gridIndex % 2 == 0) ? -2.25f : 2.25f;
            const float rowOffset = static_cast<float>(gridIndex / 2) * 6.0f;
            const VehiclePose pose{gridOrigin.x - forwardX * rowOffset + rightX * laneOffset, gridOrigin.y,
                                   gridOrigin.z - forwardZ * rowOffset + rightZ * laneOffset, gridOrigin.heading};

            const bool isPlayer = gridIndex == 0;
            const uint32_t vehicleId = sim.vehicles.CreateVehicle(seed.name, seed.type, isPlayer, pose);
            if (vehicleId == 0)
                return false;
            sim.race.RegisterRacer(vehicleId, seed.name, isPlayer);
            if (!isPlayer)
                sim.ai.AddDriver(MakeRosterDriverConfig(vehicleId, seed.name, seed.type, seed.difficulty));
        }

        const uint32_t totalLaps = std::max<uint32_t>(1u, track.totalLaps);
        sim.race.StartRace(RaceMode::SingleRace, totalLaps);
        return true;
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
