/**
 * @file MainRaceFlow.cpp
 * @brief SparkGameRacing race-flow orchestration: binds engine input to the
 *        shared RacingRaceFlow roster/frame-step helpers and publishes
 *        camera/HUD presentation state. Split from Main.cpp per the
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
#include <vector>

void SparkGameRacingModule::SetupDefaultRaceRoster()
{
    if (!m_vehicleSystem || !m_trackSystem || !m_raceManager || !m_aiDriver)
        return;

    Racing::SetupRaceRoster({*m_vehicleSystem, *m_trackSystem, *m_raceManager, *m_aiDriver}, m_context);
}

void SparkGameRacingModule::StepRace(float deltaTime)
{
    if (!m_vehicleSystem || !m_trackSystem || !m_raceManager || !m_aiDriver)
        return;

    InputManager* input = m_context ? m_context->GetInput() : nullptr;
    Racing::PlayerDriveInput playerInput;
    if (input)
    {
        const Racing::RaceControlEdges edges = Racing::PollRaceControlEdges(
            input->IsKeyDown('R'), input->IsKeyDown('C'), m_restartHeld, m_cameraCycleHeld);
        if (edges.restartRequested)
            SetupDefaultRaceRoster();
        if (edges.cameraCycleRequested && m_cameraSystem)
            m_cameraSystem->CycleMode();

        playerInput.throttle = input->IsKeyDown('W') ? 1.0f : 0.0f;
        playerInput.brake = input->IsKeyDown('S') ? 1.0f : 0.0f;
        if (input->IsKeyDown('A'))
            playerInput.steer -= 1.0f;
        if (input->IsKeyDown('D'))
            playerInput.steer += 1.0f;
        playerInput.nitro = input->IsKeyDown('N');
        playerInput.drift = input->IsKeyDown(' ');
    }

    Racing::StepRaceFrame({*m_vehicleSystem, *m_trackSystem, *m_raceManager, *m_aiDriver},
                          input ? &playerInput : nullptr, deltaTime);
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
