/**
 * @file Main.cpp
 * @brief SparkGameRacing DLL - IModule implementation and exports
 *
 * Implements the SparkGameRacingModule class and exports the CreateModule/
 * DestroyModule factory functions for the engine's ModuleManager.
 */

#include "SparkGameRacing.h"
#include "RacingEngineSystems.h"
#include "Vehicle/RacingVehicleSystem.h"
#include "Track/RacingTrackSystem.h"
#include "Race/RacingRaceManager.h"
#include "AI/RacingAIDriver.h"
#include "Camera/RacingCameraSystem.h"
#include "HUD/RacingHUDSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "Utils/InvalidStateDetector.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"
#include "Engine/ECS/Components/PhysicsComponents.h"
#include <algorithm>
#include <array>

#include <Spark/ModuleDllMain.h>

// =============================================================================
// Module exports
// =============================================================================

SPARK_IMPLEMENT_MODULE(SparkGameRacingModule)

// =============================================================================
// SparkGameRacingModule implementation
// =============================================================================

SparkGameRacingModule::SparkGameRacingModule() = default;

SparkGameRacingModule::~SparkGameRacingModule()
{
    if (m_initialized)
        OnUnload();
}

Spark::ModuleInfo SparkGameRacingModule::GetModuleInfo() const
{
    Spark::ModuleInfo info{};
    info.name = "Spark Racing - Engine Showcase";
    info.version = "1.0.0";
    info.sdkVersion = SPARK_SDK_VERSION;
    info.loadOrder = 1003;
    return info;
}

bool SparkGameRacingModule::OnLoad(Spark::IEngineContext* context)
{
    if (!context)
        return false;

    m_context = context;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[Racing] Loading Spark Racing module...");
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Racing module loading — initializing 7 subsystems");

    // Initialize track system first (other systems query it)
    m_trackSystem = std::make_unique<Racing::RacingTrackSystem>();
    if (!m_trackSystem->Initialize(context))
    {
        console.LogError("[Racing] Failed to initialize track system");
        return false;
    }

    // Initialize vehicle system
    m_vehicleSystem = std::make_unique<Racing::RacingVehicleSystem>();
    if (!m_vehicleSystem->Initialize(context))
    {
        console.LogError("[Racing] Failed to initialize vehicle system");
        return false;
    }

    // Initialize race manager
    m_raceManager = std::make_unique<Racing::RacingRaceManager>();
    if (!m_raceManager->Initialize(context))
    {
        console.LogError("[Racing] Failed to initialize race manager");
        return false;
    }

    // Initialize AI driver system
    m_aiDriver = std::make_unique<Racing::RacingAIDriver>();
    if (!m_aiDriver->Initialize(context))
    {
        console.LogError("[Racing] Failed to initialize AI driver system");
        return false;
    }

    // Initialize camera system
    m_cameraSystem = std::make_unique<Racing::RacingCameraSystem>();
    if (!m_cameraSystem->Initialize(context))
    {
        console.LogError("[Racing] Failed to initialize camera system");
        return false;
    }

    // Initialize HUD system
    m_hudSystem = std::make_unique<Racing::RacingHUDSystem>();
    if (!m_hudSystem->Initialize(context))
    {
        console.LogError("[Racing] Failed to initialize HUD system");
        return false;
    }

    // Initialize engine system integrations (audio, events, save, replay, weather, destruction, coroutines)
    m_engineSystems = std::make_unique<Racing::RacingEngineSystems>();
    if (!m_engineSystems->Initialize(context))
    {
        console.LogError("[Racing] Failed to initialize engine system integrations");
        return false;
    }

    RegisterConsoleCommands();
    SetupDefaultRaceRoster();

    // Register Racing-specific state validation rules
    auto& stateDetector = Spark::InvalidStateDetector::GetInstance();

    stateDetector.AddRule({"Racing.StaticVehicleMoving", "Racing", Spark::StateViolationSeverity::Warning, true,
                           [](World& w, std::vector<Spark::StateViolation>& out)
                           {
                               for (auto entity : w.GetEntitiesWith<RigidBodyComponent>())
                               {
                                   auto* rb = w.GetComponent<RigidBodyComponent>(entity);
                                   if (!rb || rb->type != RigidBodyComponent::Type::Static)
                                       continue;
                                   float speedSq = rb->linearVelocity.x * rb->linearVelocity.x +
                                                   rb->linearVelocity.y * rb->linearVelocity.y +
                                                   rb->linearVelocity.z * rb->linearVelocity.z;
                                   if (speedSq > 1.0f)
                                   {
                                       out.push_back(
                                           {"Racing.StaticVehicleMoving", static_cast<uint32_t>(entity),
                                            "Static body has velocity (speedSq=" + std::to_string(speedSq) + ")",
                                            Spark::StateViolationSeverity::Warning});
                                   }
                               }
                           }});

    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Racing module loaded successfully");
    console.LogInfo("[Racing] Spark Racing module loaded successfully (7 subsystems)");
    console.LogInfo("[Racing] Tracks: " + std::to_string(m_trackSystem->GetTrackCount()) +
                    " | Vehicles: " + std::to_string(m_vehicleSystem->GetVehicleCount()) +
                    " | AI Drivers: " + std::to_string(m_aiDriver->GetDriverCount()));
    return true;
}

void SparkGameRacingModule::OnUnload()
{
    if (!m_initialized)
        return;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[Racing] Unloading Spark Racing module...");
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Racing module shutting down");

    // Shutdown in reverse initialization order
    if (m_engineSystems)
    {
        m_engineSystems->Shutdown();
        m_engineSystems.reset();
    }
    if (m_hudSystem)
    {
        m_hudSystem->Shutdown();
        m_hudSystem.reset();
    }
    if (m_cameraSystem)
    {
        m_cameraSystem->Shutdown();
        m_cameraSystem.reset();
    }
    if (m_aiDriver)
    {
        m_aiDriver->Shutdown();
        m_aiDriver.reset();
    }
    if (m_raceManager)
    {
        m_raceManager->Shutdown();
        m_raceManager.reset();
    }
    if (m_vehicleSystem)
    {
        m_vehicleSystem->Shutdown();
        m_vehicleSystem.reset();
    }
    if (m_trackSystem)
    {
        m_trackSystem->Shutdown();
        m_trackSystem.reset();
    }

    m_context = nullptr;
    m_initialized = false;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Racing module unloaded");
    console.LogInfo("[Racing] Spark Racing module unloaded");
}

void SparkGameRacingModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_engineSystems->Update(deltaTime);
    m_trackSystem->Update(deltaTime);
    m_raceManager->Update(deltaTime);
    SyncRaceAndTrackState();
    m_aiDriver->Update(deltaTime);
    ApplyAIDriverInputs();
    m_vehicleSystem->Update(deltaTime);
    m_cameraSystem->Update(deltaTime);
    m_hudSystem->Update(deltaTime);
}

void SparkGameRacingModule::OnFixedUpdate(float fixedDeltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_vehicleSystem->FixedUpdate(fixedDeltaTime);
}

void SparkGameRacingModule::OnRender()
{
    if (!m_initialized)
        return;

    // Rendering is handled by the engine's render pipeline;
    // systems register their renderables during Update.
}

void SparkGameRacingModule::OnResize(int width, int height)
{
    (void)width;
    (void)height;
}

void SparkGameRacingModule::OnPause()
{
    m_paused = true;
}

void SparkGameRacingModule::OnResume()
{
    m_paused = false;
}

void SparkGameRacingModule::OnImGui()
{
    if (!m_initialized)
        return;

    m_vehicleSystem->RenderDebugUI();
    m_trackSystem->RenderDebugUI();
    m_raceManager->RenderDebugUI();
    m_aiDriver->RenderDebugUI();
    m_cameraSystem->RenderDebugUI();
    m_hudSystem->RenderDebugUI();
    m_engineSystems->RenderDebugUI();
}

void SparkGameRacingModule::RegisterConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    console.RegisterCommand(
        "race_status",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_raceManager)
                return "Racing module not initialized";

            std::string status = "=== Spark Racing Status ===\n";
            status += "Vehicles: " + std::to_string(m_vehicleSystem->GetVehicleCount()) + "\n";
            status += "Tracks: " + std::to_string(m_trackSystem->GetTrackCount()) + "\n";
            status += "AI Drivers: " + std::to_string(m_aiDriver->GetDriverCount()) + "\n";
            status += "Race State: " +
                      std::string(m_raceManager->GetState() == Racing::RaceState::Racing ? "Racing" : "Not Racing") +
                      "\n";
            status += "Camera: " + std::string(Racing::RacingCameraSystem::ModeToString(m_cameraSystem->GetMode()));
            return status;
        });

    console.RegisterCommand("race_vehicles", [this](const std::vector<std::string>&) -> std::string
                            { return m_vehicleSystem->GetVehicleListString(); });

    console.RegisterCommand("race_tracks", [this](const std::vector<std::string>&) -> std::string
                            { return m_trackSystem->GetTrackListString(); });

    console.RegisterCommand("race_standings", [this](const std::vector<std::string>&) -> std::string
                            { return m_raceManager->GetStandingsString(); });

    console.RegisterCommand("race_ai", [this](const std::vector<std::string>&) -> std::string
                            { return m_aiDriver->GetDriverListString(); });

    console.RegisterCommand(
        "race_camera",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
            {
                m_cameraSystem->CycleMode();
                return "Camera: " + std::string(Racing::RacingCameraSystem::ModeToString(m_cameraSystem->GetMode()));
            }
            // Named mode selection
            if (args[0] == "chase")
                m_cameraSystem->SetMode(Racing::CameraMode::Chase);
            else if (args[0] == "cockpit")
                m_cameraSystem->SetMode(Racing::CameraMode::Cockpit);
            else if (args[0] == "hood")
                m_cameraSystem->SetMode(Racing::CameraMode::Hood);
            else if (args[0] == "orbit")
                m_cameraSystem->SetMode(Racing::CameraMode::Orbit);
            else if (args[0] == "cinematic")
                m_cameraSystem->SetMode(Racing::CameraMode::Cinematic);
            else
                return "Unknown mode. Options: chase, cockpit, hood, orbit, cinematic";
            return "Camera: " + std::string(Racing::RacingCameraSystem::ModeToString(m_cameraSystem->GetMode()));
        });

    console.RegisterCommand("race_difficulty",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (args.empty())
                                    return "Usage: race_difficulty <easy|medium|hard|expert>";
                                if (args[0] == "easy")
                                    m_aiDriver->SetGlobalDifficulty(Racing::AIDifficulty::Easy);
                                else if (args[0] == "medium")
                                    m_aiDriver->SetGlobalDifficulty(Racing::AIDifficulty::Medium);
                                else if (args[0] == "hard")
                                    m_aiDriver->SetGlobalDifficulty(Racing::AIDifficulty::Hard);
                                else if (args[0] == "expert")
                                    m_aiDriver->SetGlobalDifficulty(Racing::AIDifficulty::Expert);
                                else
                                    return "Unknown difficulty. Options: easy, medium, hard, expert";
                                return "AI difficulty set to: " + args[0];
                            });

    // --- Engine system commands ---

    console.RegisterCommand("race_save",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (args.empty())
                                    return "Usage: race_save <slot_name>";
                                return m_engineSystems->SaveRaceData(args[0]);
                            });

    console.RegisterCommand("race_load",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (args.empty())
                                    return "Usage: race_load <slot_name>";
                                return m_engineSystems->LoadRaceData(args[0]);
                            });

    console.RegisterCommand("race_replay",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (args.empty())
                                    return "Usage: race_replay <record|stop|play>";
                                return m_engineSystems->ToggleReplay(args[0]);
                            });

    console.RegisterCommand("race_ghost",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (args.empty())
                                    return "Usage: race_ghost <track_name>";
                                return m_engineSystems->ToggleGhost(args[0]);
                            });

    console.RegisterCommand("race_weather",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (args.empty())
                                    return "Usage: race_weather <clear|rain|storm>";
                                return m_engineSystems->SetWeather(args[0]);
                            });

    console.RegisterCommand("race_restart",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                SetupDefaultRaceRoster();
                                return "Race roster rebuilt and race restarted";
                            });
}

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
