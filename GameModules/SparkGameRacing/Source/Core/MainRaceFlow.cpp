/**
 * @file MainRaceFlow.cpp
 * @brief SparkGameRacing race-flow orchestration: default roster setup and
 *        per-frame race/track/AI state sync. Split from Main.cpp per the
 *        repo file-size rule (MainFrame pattern — same class, feature-owned
 *        translation units); Main.cpp keeps the exports, lifecycle, and
 *        console command registration.
 */

#include "SparkGameRacing.h"
#include "Vehicle/RacingVehicleSystem.h"
#include "Track/RacingTrackSystem.h"
#include "Race/RacingRaceManager.h"
#include "AI/RacingAIDriver.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

void SparkGameRacingModule::SetupDefaultRaceRoster()
{
    if (!m_vehicleSystem || !m_trackSystem || !m_raceManager || !m_aiDriver)
        return;

    m_vehicleSystem->Shutdown();
    m_vehicleSystem->Initialize(m_context);

    m_raceManager->Shutdown();
    m_raceManager->Initialize(m_context);

    m_aiDriver->Shutdown();
    m_aiDriver->Initialize(m_context);

    const uint32_t playerId = m_vehicleSystem->CreateVehicle("Player", Racing::VehicleType::SportsCar, true);
    m_raceManager->RegisterRacer(playerId, "Player", true);

    struct AIDriverSeed
    {
        const char* name;
        Racing::VehicleType type;
        Racing::AIDifficulty difficulty;
    };

    constexpr std::array<AIDriverSeed, 5> kAIDrivers = {{
        {"Nova", Racing::VehicleType::Formula, Racing::AIDifficulty::Expert},
        {"Vex", Racing::VehicleType::SuperCar, Racing::AIDifficulty::Hard},
        {"Mara", Racing::VehicleType::SportsCar, Racing::AIDifficulty::Hard},
        {"Bolt", Racing::VehicleType::MuscleCar, Racing::AIDifficulty::Medium},
        {"Rift", Racing::VehicleType::OffRoad, Racing::AIDifficulty::Medium},
    }};

    for (const AIDriverSeed& seed : kAIDrivers)
    {
        const uint32_t vehicleId = m_vehicleSystem->CreateVehicle(seed.name, seed.type, false);
        m_raceManager->RegisterRacer(vehicleId, seed.name, false);

        Racing::AIDriverConfig config{};
        config.vehicleId = vehicleId;
        config.name = seed.name;
        config.preferredVehicle = seed.type;
        config.difficulty = seed.difficulty;

        switch (seed.difficulty)
        {
        case Racing::AIDifficulty::Easy:
            config.speedFactor = 0.70f;
            config.lineAccuracy = 0.50f;
            config.reactionTime = 0.30f;
            config.aggressiveness = 0.2f;
            break;
        case Racing::AIDifficulty::Medium:
            config.speedFactor = 0.85f;
            config.lineAccuracy = 0.70f;
            config.reactionTime = 0.15f;
            config.aggressiveness = 0.5f;
            break;
        case Racing::AIDifficulty::Hard:
            config.speedFactor = 0.95f;
            config.lineAccuracy = 0.85f;
            config.reactionTime = 0.08f;
            config.aggressiveness = 0.7f;
            break;
        case Racing::AIDifficulty::Expert:
            config.speedFactor = 1.0f;
            config.lineAccuracy = 0.95f;
            config.reactionTime = 0.03f;
            config.aggressiveness = 0.9f;
            break;
        case Racing::AIDifficulty::Count:
            break;
        }

        m_aiDriver->AddDriver(config);
    }

    const uint32_t totalLaps = std::max<uint32_t>(1u, m_trackSystem->GetCurrentTrack().totalLaps);
    m_raceManager->StartRace(Racing::RaceMode::SingleRace, totalLaps);
}

void SparkGameRacingModule::SyncRaceAndTrackState()
{
    if (!m_vehicleSystem || !m_trackSystem || !m_raceManager || !m_aiDriver)
        return;

    const auto& currentTrack = m_trackSystem->GetCurrentTrack();
    const size_t waypointCount = currentTrack.waypoints.size();
    if (waypointCount == 0)
        return;

    float playerDistance = 0.0f;
    float leadDistance = 0.0f;
    float lastDistance = 0.0f;
    bool hasPlayer = false;
    bool hasDistanceRange = false;

    for (auto& vehicle : m_vehicleSystem->GetVehiclesMutable())
    {
        if (!vehicle.isActive)
            continue;

        vehicle.currentSurface = m_trackSystem->GetSurfaceAt(vehicle.positionX, vehicle.positionZ);

        const int hazard = m_trackSystem->CheckHazard(vehicle.positionX, vehicle.positionZ);
        if (hazard >= 0)
        {
            const auto& hazardData = currentTrack.hazards[static_cast<size_t>(hazard)];
            if (hazardData.type == Racing::TrackHazard::Type::OilSlick)
                vehicle.speed *= 0.96f;
            else if (hazardData.type == Racing::TrackHazard::Type::SpeedBoost)
                vehicle.boostTimer = std::max(vehicle.boostTimer, 0.6f);
        }

        const uint32_t nearestWaypoint = m_trackSystem->GetNearestWaypoint(vehicle.positionX, vehicle.positionZ);
        const float lapDistance = static_cast<float>(nearestWaypoint);
        const auto* racer = m_raceManager->GetRacer(vehicle.id);
        const float progressDistance =
            static_cast<float>(racer ? racer->currentLap : 0u) * static_cast<float>(waypointCount) + lapDistance;
        m_raceManager->UpdateRacerDistance(vehicle.id, progressDistance);

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

        const int checkpoint = m_trackSystem->CheckCheckpoint(vehicle.positionX, vehicle.positionZ);
        if (checkpoint >= 0)
        {
            const auto* preCheckpointState = m_raceManager->GetRacer(vehicle.id);
            const bool crossingFinishLine =
                preCheckpointState && checkpoint == 0 && preCheckpointState->lastCheckpoint > 0;
            m_raceManager->OnCheckpointCrossed(vehicle.id, static_cast<uint32_t>(checkpoint));
            if (crossingFinishLine)
                m_raceManager->OnLapCompleted(vehicle.id);
        }
    }

    if (!hasPlayer)
        playerDistance = lastDistance;
    if (!hasDistanceRange)
        return;

    m_aiDriver->UpdateRubberBanding(playerDistance, leadDistance, lastDistance);
}

void SparkGameRacingModule::ApplyAIDriverInputs()
{
    if (!m_vehicleSystem || !m_aiDriver)
        return;

    for (const auto& vehicle : m_vehicleSystem->GetVehicles())
    {
        if (vehicle.isPlayer)
            continue;

        const Racing::AIDriverState* aiState = m_aiDriver->GetDriverState(vehicle.id);
        if (!aiState)
            continue;

        m_vehicleSystem->ApplyInputToVehicle(vehicle.id, aiState->throttle, aiState->brake, aiState->steer,
                                             aiState->useNitro, aiState->useDrift);
    }
}
