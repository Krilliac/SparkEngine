/**
 * @file MainRaceFlow.cpp
 * @brief SparkGameRacing race-flow orchestration: default roster setup and
 *        per-frame race/track/AI state sync. Split from Main.cpp per the
 *        repo file-size rule (MainFrame pattern — same class, feature-owned
 *        translation units); Main.cpp keeps the exports, lifecycle, and
 *        console command registration.
 */

#include "SparkGameRacing.h"
#include "RacingRaceFlow.h"
#include "Vehicle/RacingVehicleSystem.h"
#include "Track/RacingTrackSystem.h"
#include "Race/RacingRaceManager.h"
#include "AI/RacingAIDriver.h"
#include "Camera/RacingCameraSystem.h"
#include "HUD/RacingHUDSystem.h"
#include "Input/InputManager.h"
#include <algorithm>
#include <array>
#include <cmath>
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

    const auto& track = m_trackSystem->GetCurrentTrack();
    if (track.waypoints.size() >= 2)
    {
        const auto& start = track.waypoints[0];
        const auto& next = track.waypoints[1];
        const float heading = std::atan2(next.x - start.x, next.z - start.z);
        const float forwardX = std::sin(heading);
        const float forwardZ = std::cos(heading);
        const float rightX = std::cos(heading);
        const float rightZ = -std::sin(heading);

        size_t gridIndex = 0;
        for (auto& vehicle : m_vehicleSystem->GetVehiclesMutable())
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

        if (Racing::StopTerminalRacer(*m_raceManager, *m_vehicleSystem, vehicle.id))
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
            Racing::ProcessOrderedCheckpoint(*m_raceManager, currentTrack, vehicle.id,
                                             static_cast<uint32_t>(checkpoint));
    }

    if (!hasPlayer)
        playerDistance = lastDistance;
    if (!hasDistanceRange)
        return;

    m_raceManager->RefreshPositions();
    m_aiDriver->UpdateRubberBanding(playerDistance, leadDistance, lastDistance);
}

void SparkGameRacingModule::UpdatePresentationState()
{
    if (!m_vehicleSystem || !m_raceManager || !m_cameraSystem || !m_hudSystem)
        return;

    const Racing::VehicleInstance* player = m_vehicleSystem->GetPlayerVehicle();
    if (!player)
        return;

    m_cameraSystem->SetTarget(player->positionX, player->positionY, player->positionZ, player->heading, player->speed);

    Racing::HUDData hud;
    hud.speed = player->speed;
    hud.maxSpeed = player->baseStats.maxSpeed;
    hud.rpm = std::clamp(player->rpm / 8000.0f, 0.0f, 1.0f);
    hud.gear = std::clamp(
        static_cast<uint32_t>(1.0f + 5.0f * player->speed / std::max(1.0f, player->baseStats.maxSpeed)), 1u, 6u);
    hud.position = m_raceManager->GetPlayerPosition();
    hud.totalRacers = static_cast<uint32_t>(m_raceManager->GetRacerCount());
    hud.totalLaps = m_raceManager->GetTotalLaps();
    hud.totalTime = m_raceManager->GetRaceTime();
    hud.bestLapTime = m_raceManager->GetPlayerBestLap();
    hud.nitroLevel = player->nitro;
    hud.boostActive = player->boostTimer;
    hud.raceState = m_raceManager->GetState();
    hud.countdown = m_raceManager->GetCountdownTimer();
    if (const Racing::RacerState* racer = m_raceManager->GetRacer(player->id))
    {
        hud.currentLap = std::min(racer->currentLap + 1u, std::max(1u, hud.totalLaps));
        hud.lapTime = racer->currentLapTime;
    }
    m_hudSystem->SetHUDData(hud);

    std::vector<Racing::MinimapEntry> minimap;
    minimap.reserve(m_vehicleSystem->GetVehicleCount());
    for (const auto& vehicle : m_vehicleSystem->GetVehicles())
    {
        if (!vehicle.isActive)
            continue;
        const Racing::RacerState* racer = m_raceManager->GetRacer(vehicle.id);
        minimap.push_back({vehicle.positionX, vehicle.positionZ, racer ? racer->position : 0u, vehicle.isPlayer});
    }
    m_hudSystem->SetMinimapEntries(minimap);
}

void SparkGameRacingModule::ApplyPlayerInput(float deltaTime)
{
    if (!m_vehicleSystem || !m_raceManager)
        return;

    Racing::VehicleInstance* player = m_vehicleSystem->GetPlayerVehicle();
    InputManager* input = m_context ? m_context->GetInput() : nullptr;
    if (input)
    {
        const Racing::RaceControlEdges edges = Racing::PollRaceControlEdges(
            input->IsKeyDown('R'), input->IsKeyDown('C'), m_restartHeld, m_cameraCycleHeld);
        if (edges.restartRequested)
        {
            SetupDefaultRaceRoster();
            return;
        }
        if (edges.cameraCycleRequested && m_cameraSystem)
            m_cameraSystem->CycleMode();
    }

    if (player && Racing::StopTerminalRacer(*m_raceManager, *m_vehicleSystem, player->id))
        return;

    if (!input)
        return;

    if (m_raceManager->GetState() != Racing::RaceState::Racing)
    {
        m_vehicleSystem->ApplyInput(0.0f, 0.0f, 0.0f, false, false, deltaTime);
        return;
    }

    const float throttle = input->IsKeyDown('W') ? 1.0f : 0.0f;
    const float brake = input->IsKeyDown('S') ? 1.0f : 0.0f;
    float steer = 0.0f;
    if (input->IsKeyDown('A'))
        steer -= 1.0f;
    if (input->IsKeyDown('D'))
        steer += 1.0f;
    m_vehicleSystem->ApplyInput(throttle, brake, steer, input->IsKeyDown('N'), input->IsKeyDown(' '), deltaTime);
}

void SparkGameRacingModule::ApplyAIDriverInputs(float deltaTime)
{
    if (!m_vehicleSystem || !m_trackSystem || !m_raceManager || !m_aiDriver)
        return;

    for (const auto& vehicle : m_vehicleSystem->GetVehicles())
    {
        if (vehicle.isPlayer)
            continue;

        if (Racing::StopTerminalRacer(*m_raceManager, *m_vehicleSystem, vehicle.id))
            continue;

        const Racing::AIDriverState* aiState = m_aiDriver->GetDriverState(vehicle.id);
        if (!aiState)
            continue;

        if (m_raceManager->GetState() != Racing::RaceState::Racing)
        {
            m_vehicleSystem->ApplyInputToVehicle(vehicle.id, 0.0f, 0.0f, 0.0f, false, false, deltaTime);
            continue;
        }

        const float trackSteer = Racing::ComputeTrackSteer(vehicle, *m_trackSystem);

        m_vehicleSystem->ApplyInputToVehicle(vehicle.id, aiState->throttle, aiState->brake, trackSteer,
                                             aiState->useNitro, aiState->useDrift, deltaTime);
    }
}
