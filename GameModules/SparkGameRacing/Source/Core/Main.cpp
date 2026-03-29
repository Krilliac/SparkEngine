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

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
#endif

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

    SPARK_LOG_INFO(Spark::LogCategory::Game, "[Racing] Loading Spark Racing module...");

    // Initialize track system first (other systems query it)
    m_trackSystem = std::make_unique<Racing::RacingTrackSystem>();
    if (!m_trackSystem->Initialize(context))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[Racing] Failed to initialize track system");
        return false;
    }

    // Initialize vehicle system
    m_vehicleSystem = std::make_unique<Racing::RacingVehicleSystem>();
    if (!m_vehicleSystem->Initialize(context))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[Racing] Failed to initialize vehicle system");
        return false;
    }

    // Initialize race manager
    m_raceManager = std::make_unique<Racing::RacingRaceManager>();
    if (!m_raceManager->Initialize(context))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[Racing] Failed to initialize race manager");
        return false;
    }

    // Initialize AI driver system
    m_aiDriver = std::make_unique<Racing::RacingAIDriver>();
    if (!m_aiDriver->Initialize(context))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[Racing] Failed to initialize AI driver system");
        return false;
    }

    // Initialize camera system
    m_cameraSystem = std::make_unique<Racing::RacingCameraSystem>();
    if (!m_cameraSystem->Initialize(context))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[Racing] Failed to initialize camera system");
        return false;
    }

    // Initialize HUD system
    m_hudSystem = std::make_unique<Racing::RacingHUDSystem>();
    if (!m_hudSystem->Initialize(context))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[Racing] Failed to initialize HUD system");
        return false;
    }

    // Initialize engine system integrations (audio, events, save, replay, weather, destruction, coroutines)
    m_engineSystems = std::make_unique<Racing::RacingEngineSystems>();
    if (!m_engineSystems->Initialize(context))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[Racing] Failed to initialize engine system integrations");
        return false;
    }

    RegisterConsoleCommands();

    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[Racing] Spark Racing module loaded successfully (7 subsystems)");
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[Racing] Tracks: %s | Vehicles: %s | AI Drivers: %s",
                   std::to_string(m_trackSystem->GetTrackCount()).c_str(),
                   std::to_string(m_vehicleSystem->GetVehicleCount()).c_str(),
                   std::to_string(m_aiDriver->GetDriverCount()).c_str());
    return true;
}

void SparkGameRacingModule::OnUnload()
{
    if (!m_initialized)
        return;

    SPARK_LOG_INFO(Spark::LogCategory::Game, "[Racing] Unloading Spark Racing module...");

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
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[Racing] Spark Racing module unloaded");
}

void SparkGameRacingModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_engineSystems->Update(deltaTime);
    m_trackSystem->Update(deltaTime);
    m_raceManager->Update(deltaTime);
    m_aiDriver->Update(deltaTime);
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
}
