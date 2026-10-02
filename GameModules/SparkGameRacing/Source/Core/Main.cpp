/**
 * @file Main.cpp
 * @brief SparkGameRacing DLL - IModule implementation and exports
 *
 * Implements the SparkGameRacingModule class and exports the CreateModule/
 * DestroyModule factory functions for the engine's ModuleManager. The
 * race-flow orchestration (SetupDefaultRaceRoster / StepRace /
 * UpdatePresentationState) lives in MainRaceFlow.cpp,
 * split per the repo file-size rule (MainFrame pattern — same class,
 * feature-owned translation units).
 */

#include "SparkGameRacing.h"
#include "RacingEngineSystems.h"
#include "Vehicle/RacingVehicleSystem.h"
#include "Track/RacingTrackSystem.h"
#include "Race/RacingRaceManager.h"
#include "AI/RacingAIDriver.h"
#include "Camera/RacingCameraSystem.h"
#include "HUD/RacingHUDSystem.h"
#include <Spark/ModuleLog.h>
#include <Spark/IConsole.h>
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"
#include "Engine/ECS/Components/PhysicsComponents.h"

#include <Spark/IStateValidation.h>
#include <Spark/ModuleDllMain.h>
#include <string_view>
#include <utility>

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

    Spark::ModuleLog::Info(m_context, "[Racing] Loading Spark Racing module...");
    Spark::ModuleLog::Info(m_context, "Racing module loading — initializing 7 subsystems");

    // Initialize track system first (other systems query it)
    m_trackSystem = std::make_unique<Racing::RacingTrackSystem>();
    if (!m_trackSystem->Initialize(context))
    {
        Spark::ModuleLog::Error(m_context, "[Racing] Failed to initialize track system");
        return false;
    }

    // Initialize vehicle system (binds the engine's shared Jolt world; the module steps it in OnFixedUpdate)
    m_vehicleSystem = std::make_unique<Racing::RacingVehicleSystem>();
    if (!m_vehicleSystem->Initialize(context))
    {
        Spark::ModuleLog::Error(m_context, "[Racing] Failed to initialize vehicle system (no live Jolt physics world)");
        return false;
    }

    // Initialize race manager
    m_raceManager = std::make_unique<Racing::RacingRaceManager>();
    if (!m_raceManager->Initialize(context))
    {
        Spark::ModuleLog::Error(m_context, "[Racing] Failed to initialize race manager");
        return false;
    }

    // Initialize AI driver system
    m_aiDriver = std::make_unique<Racing::RacingAIDriver>();
    if (!m_aiDriver->Initialize(context))
    {
        Spark::ModuleLog::Error(m_context, "[Racing] Failed to initialize AI driver system");
        return false;
    }

    // Initialize camera system
    m_cameraSystem = std::make_unique<Racing::RacingCameraSystem>();
    if (!m_cameraSystem->Initialize(context))
    {
        Spark::ModuleLog::Error(m_context, "[Racing] Failed to initialize camera system");
        return false;
    }

    // Initialize HUD system
    m_hudSystem = std::make_unique<Racing::RacingHUDSystem>();
    if (!m_hudSystem->Initialize(context))
    {
        Spark::ModuleLog::Error(m_context, "[Racing] Failed to initialize HUD system");
        return false;
    }

    // Initialize engine system integrations (audio, events, save, replay, weather, destruction, coroutines)
    m_engineSystems = std::make_unique<Racing::RacingEngineSystems>();
    if (!m_engineSystems->Initialize(context, m_vehicleSystem.get(), m_trackSystem.get(), m_raceManager.get(),
                                     m_aiDriver.get()))
    {
        Spark::ModuleLog::Error(m_context, "[Racing] Failed to initialize engine system integrations");
        return false;
    }

    if (!SetupDefaultRaceRoster())
    {
        Spark::ModuleLog::Error(m_context, "[Racing] Failed to build the starting grid");
        return false;
    }
    RegisterConsoleCommands();

    // Register Racing-specific state validation rules
    Spark::IStateValidation* stateRules = m_context->GetStateValidation();
    const bool stateRulesRegistered =
        stateRules != nullptr &&
        stateRules->AddRule("Racing.StaticVehicleMoving", "Racing", Spark::StateViolationSeverity::Warning,
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
                            });
    if (!stateRulesRegistered)
    {
        Spark::ModuleLog::Warn(m_context, "[Racing] Host refused the Racing state-validation rules");
    }

    m_initialized = true;
    Spark::ModuleLog::Info(m_context, "Racing module loaded successfully");
    Spark::ModuleLog::Info(m_context, "[Racing] Spark Racing module loaded successfully (7 subsystems)");
    Spark::ModuleLog::Info(m_context, "{}",
                           "[Racing] Tracks: " + std::to_string(m_trackSystem->GetTrackCount()) +
                               " | Vehicles: " + std::to_string(m_vehicleSystem->GetVehicleCount()) +
                               " | AI Drivers: " + std::to_string(m_aiDriver->GetDriverCount()));
    return true;
}

void SparkGameRacingModule::OnUnload()
{
    if (!m_initialized)
    {
        return;
    }

    if (auto* console = m_context ? m_context->GetConsole() : nullptr)
    {
        for (const auto& command : m_registeredConsoleCommands)
        {
            console->UnregisterCommand(command);
        }
    }
    m_registeredConsoleCommands.clear();

    // Validation callbacks are std::functions implemented in this DLL. Drop
    // them before the module image is unmapped during hot unload/reload.
    if (Spark::IStateValidation* stateRules = m_context ? m_context->GetStateValidation() : nullptr)
    {
        stateRules->RemoveRulesByCategory("Racing");
    }

    Spark::ModuleLog::Info(m_context, "[Racing] Unloading Spark Racing module...");
    Spark::ModuleLog::Info(m_context, "Racing module shutting down");

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

    Spark::ModuleLog::Info(m_context, "Racing module unloaded");
    Spark::ModuleLog::Info(m_context, "[Racing] Spark Racing module unloaded");
    m_initialized = false;
    m_context = nullptr;
}

void SparkGameRacingModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_engineSystems->Update(deltaTime);
    m_trackSystem->Update(deltaTime);
    StepRace(deltaTime);
    UpdatePresentationState();
    m_cameraSystem->Update(deltaTime);
    m_hudSystem->Update(deltaTime);
}

void SparkGameRacingModule::OnFixedUpdate(float fixedDeltaTime)
{
    if (!m_initialized || m_paused)
        return;

    // Sole physics stepping owner in the Racing process: one shared-world tick per engine fixed step.
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
    auto* console = m_context ? m_context->GetConsole() : nullptr;
    if (!console)
    {
        return;
    }
    const auto registerCommand = [this, console](std::string_view name, Spark::IConsole::CommandHandler handler,
                                                 std::string_view help = {}, std::string_view category = "General",
                                                 std::string_view usage = {})
    {
        if (console->RegisterCommand(name, std::move(handler), help, category, usage))
        {
            m_registeredConsoleCommands.emplace_back(name);
        }
    };

    registerCommand(
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

    registerCommand("race_vehicles", [this](const std::vector<std::string>&) -> std::string
                    { return m_vehicleSystem->GetVehicleListString(); });

    registerCommand("race_tracks", [this](const std::vector<std::string>&) -> std::string
                    { return m_trackSystem->GetTrackListString(); });

    registerCommand("race_standings", [this](const std::vector<std::string>&) -> std::string
                    { return m_raceManager->GetStandingsString(); });

    registerCommand("race_ai", [this](const std::vector<std::string>&) -> std::string
                    { return m_aiDriver->GetDriverListString(); });

    registerCommand(
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

    registerCommand("race_difficulty",
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

    registerCommand("race_save",
                    [this](const std::vector<std::string>& args) -> std::string
                    {
                        if (args.empty())
                            return "Usage: race_save <slot_name>";
                        return m_engineSystems->SaveRaceData(args[0]);
                    });

    registerCommand("race_load",
                    [this](const std::vector<std::string>& args) -> std::string
                    {
                        if (args.empty())
                            return "Usage: race_load <slot_name>";
                        return m_engineSystems->LoadRaceData(args[0]);
                    });

    registerCommand("race_replay",
                    [this](const std::vector<std::string>& args) -> std::string
                    {
                        if (args.empty())
                            return "Usage: race_replay <record|stop|play>";
                        return m_engineSystems->ToggleReplay(args[0]);
                    });

    registerCommand("race_ghost",
                    [this](const std::vector<std::string>& args) -> std::string
                    {
                        if (args.empty())
                            return "Usage: race_ghost <track_name>";
                        return m_engineSystems->ToggleGhost(args[0]);
                    });

    registerCommand("race_weather",
                    [this](const std::vector<std::string>& args) -> std::string
                    {
                        if (args.empty())
                            return "Usage: race_weather <clear|rain|storm>";
                        return m_engineSystems->SetWeather(args[0]);
                    });

    // Automated player for packaged runs: restart the race with the player car on the production autopilot.
    registerCommand("race_autopilot",
                    [this](const std::vector<std::string>& args) -> std::string
                    {
                        if (args.size() != 1 || (args[0] != "on" && args[0] != "off"))
                        {
                            return "Usage: race_autopilot on|off";
                        }
                        if (args[0] == "off")
                        {
                            m_autopilot = false;
                            return "Autopilot off";
                        }
                        if (!SetupDefaultRaceRoster())
                        {
                            return "Autopilot failed: the race grid could not be rebuilt";
                        }
                        m_autopilot = true;
                        return "Autopilot on: race restarted with the player on the racing line";
                    });

    registerCommand("race_restart",
                    [this](const std::vector<std::string>&) -> std::string
                    {
                        SetupDefaultRaceRoster();
                        return "Race roster rebuilt and race restarted";
                    });
}
