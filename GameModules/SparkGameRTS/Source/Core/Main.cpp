/**
 * @file Main.cpp
 * @brief SparkGameRTS DLL - IModule implementation and exports
 *
 * Implements the SparkGameRTSModule class and exports the CreateModule/
 * DestroyModule factory functions for the engine's ModuleManager.
 */

#include "SparkGameRTS.h"
#include "RTSEngineSystems.h"
#include "Unit/RTSUnitSystem.h"
#include "Building/RTSBuildingSystem.h"
#include "Resource/RTSResourceSystem.h"
#include "Command/RTSCommandSystem.h"
#include "FogOfWar/RTSFogOfWarSystem.h"
#include "Match/RTSMatchSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "Utils/InvalidStateDetector.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"
#include "Engine/ECS/Components/AIComponents.h"

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

SPARK_IMPLEMENT_MODULE(SparkGameRTSModule)

// =============================================================================
// SparkGameRTSModule implementation
// =============================================================================

SparkGameRTSModule::SparkGameRTSModule() = default;

SparkGameRTSModule::~SparkGameRTSModule()
{
    if (m_initialized)
        OnUnload();
}

Spark::ModuleInfo SparkGameRTSModule::GetModuleInfo() const
{
    Spark::ModuleInfo info{};
    info.name = "Spark RTS - Engine Showcase";
    info.version = "1.0.0";
    info.sdkVersion = SPARK_SDK_VERSION;
    info.loadOrder = 1003; // Load after SparkGame, SparkGameMMO, and SparkGameRPG
    return info;
}

bool SparkGameRTSModule::OnLoad(Spark::IEngineContext* context)
{
    if (!context)
        return false;

    m_context = context;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[RTS] Loading Spark RTS module...");
    SPARK_LOG_INFO(Spark::LogCategory::Game, "RTS module loading — initializing 7 subsystems");

    // Initialize unit system (faction templates, spawning, AI)
    m_unitSystem = std::make_unique<RTS::RTSUnitSystem>();
    if (!m_unitSystem->Initialize(context))
    {
        console.LogError("[RTS] Failed to initialize unit system");
        return false;
    }

    // Initialize building system (tech trees, production queues)
    m_buildingSystem = std::make_unique<RTS::RTSBuildingSystem>();
    if (!m_buildingSystem->Initialize(context))
    {
        console.LogError("[RTS] Failed to initialize building system");
        return false;
    }

    // Initialize resource system (minerals, gas, supply)
    m_resourceSystem = std::make_unique<RTS::RTSResourceSystem>();
    if (!m_resourceSystem->Initialize(context))
    {
        console.LogError("[RTS] Failed to initialize resource system");
        return false;
    }

    // Initialize command system (selection, move, attack, patrol)
    m_commandSystem = std::make_unique<RTS::RTSCommandSystem>();
    if (!m_commandSystem->Initialize(context))
    {
        console.LogError("[RTS] Failed to initialize command system");
        return false;
    }

    // Initialize fog of war (grid-based vision)
    m_fogOfWarSystem = std::make_unique<RTS::RTSFogOfWarSystem>();
    if (!m_fogOfWarSystem->Initialize(context))
    {
        console.LogError("[RTS] Failed to initialize fog of war system");
        return false;
    }

    // Initialize match system (setup, win conditions)
    m_matchSystem = std::make_unique<RTS::RTSMatchSystem>();
    if (!m_matchSystem->Initialize(context))
    {
        console.LogError("[RTS] Failed to initialize match system");
        return false;
    }

    // Initialize engine system integrations (AI, events, audio, weather, destruction, save, coroutines)
    m_engineSystems = std::make_unique<RTS::RTSEngineSystems>();
    if (!m_engineSystems->Initialize(context))
    {
        console.LogWarning("[RTS] Engine system integrations partially unavailable (non-fatal)");
    }

    RegisterConsoleCommands();

    // Register RTS-specific state validation rules
    auto& stateDetector = Spark::InvalidStateDetector::GetInstance();

    stateDetector.AddRule({"RTS.DeadUnitAttacking", "RTS", Spark::StateViolationSeverity::Error, true,
                           [](World& w, std::vector<Spark::StateViolation>& out)
                           {
                               for (auto entity : w.GetEntitiesWith<HealthComponent, AIComponent>())
                               {
                                   auto* h = w.GetComponent<HealthComponent>(entity);
                                   auto* ai = w.GetComponent<AIComponent>(entity);
                                   if (h && ai && h->isDead && ai->state == AIComponent::State::Combat)
                                   {
                                       out.push_back({"RTS.DeadUnitAttacking", static_cast<uint32_t>(entity),
                                                      "Dead RTS unit still in combat state",
                                                      Spark::StateViolationSeverity::Error});
                                   }
                               }
                           }});

    stateDetector.AddRule({"RTS.IdleWithTarget", "RTS", Spark::StateViolationSeverity::Warning, true,
                           [](World& w, std::vector<Spark::StateViolation>& out)
                           {
                               for (auto entity : w.GetEntitiesWith<AIComponent>())
                               {
                                   auto* ai = w.GetComponent<AIComponent>(entity);
                                   if (ai && ai->state == AIComponent::State::Idle && ai->targetEntity != entt::null)
                                   {
                                       out.push_back(
                                           {"RTS.IdleWithTarget", static_cast<uint32_t>(entity),
                                            "Idle unit has target assigned, should be attacking or clearing target",
                                            Spark::StateViolationSeverity::Warning});
                                   }
                               }
                           }});

    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "RTS module loaded successfully — 7 subsystems active");
    console.LogInfo("[RTS] Spark RTS module loaded successfully (7 subsystems)");
    console.LogInfo("[RTS] Units: " + std::to_string(m_unitSystem->GetUnitCount()) +
                    " | Buildings: " + std::to_string(m_buildingSystem->GetBuildingCount()) +
                    " | Nodes: " + std::to_string(m_resourceSystem->GetNodeCount()));
    return true;
}

void SparkGameRTSModule::OnUnload()
{
    if (!m_initialized)
        return;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[RTS] Unloading Spark RTS module...");
    SPARK_LOG_INFO(Spark::LogCategory::Game, "RTS module shutting down");

    // Shutdown in reverse initialization order
    if (m_engineSystems)
    {
        m_engineSystems->Shutdown();
        m_engineSystems.reset();
    }
    if (m_matchSystem)
    {
        m_matchSystem->Shutdown();
        m_matchSystem.reset();
    }
    if (m_fogOfWarSystem)
    {
        m_fogOfWarSystem->Shutdown();
        m_fogOfWarSystem.reset();
    }
    if (m_commandSystem)
    {
        m_commandSystem->Shutdown();
        m_commandSystem.reset();
    }
    if (m_resourceSystem)
    {
        m_resourceSystem->Shutdown();
        m_resourceSystem.reset();
    }
    if (m_buildingSystem)
    {
        m_buildingSystem->Shutdown();
        m_buildingSystem.reset();
    }
    if (m_unitSystem)
    {
        m_unitSystem->Shutdown();
        m_unitSystem.reset();
    }

    m_context = nullptr;
    m_initialized = false;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "RTS module unloaded");
    console.LogInfo("[RTS] Spark RTS module unloaded");
}

void SparkGameRTSModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_matchSystem->Update(deltaTime);
    m_resourceSystem->Update(deltaTime);
    m_buildingSystem->Update(deltaTime);
    m_unitSystem->Update(deltaTime);
    m_commandSystem->Update(deltaTime);
    m_fogOfWarSystem->Update(deltaTime);
    if (m_engineSystems)
        m_engineSystems->Update(deltaTime);
}

void SparkGameRTSModule::OnFixedUpdate(float fixedDeltaTime)
{
    if (!m_initialized || m_paused)
        return;

    // Physics-rate updates would go here (unit collision, projectiles)
    (void)fixedDeltaTime;
}

void SparkGameRTSModule::OnRender()
{
    if (!m_initialized)
        return;
}

void SparkGameRTSModule::OnResize(int width, int height)
{
    (void)width;
    (void)height;
}

void SparkGameRTSModule::OnPause()
{
    m_paused = true;
}

void SparkGameRTSModule::OnResume()
{
    m_paused = false;
}

void SparkGameRTSModule::OnImGui()
{
    if (!m_initialized)
        return;

    m_unitSystem->RenderDebugUI();
    m_buildingSystem->RenderDebugUI();
    m_resourceSystem->RenderDebugUI();
    m_commandSystem->RenderDebugUI();
    m_fogOfWarSystem->RenderDebugUI();
    m_matchSystem->RenderDebugUI();
}

void SparkGameRTSModule::RegisterConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    console.RegisterCommand("rts_status",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                if (!m_unitSystem)
                                    return "RTS module not initialized";

                                std::string status = "=== Spark RTS Status ===\n";
                                status += "Units: " + std::to_string(m_unitSystem->GetUnitCount()) + "\n";
                                status += "Buildings: " + std::to_string(m_buildingSystem->GetBuildingCount()) + "\n";
                                status += "Resource nodes: " + std::to_string(m_resourceSystem->GetNodeCount()) + "\n";
                                status += "Selected: " + std::to_string(m_commandSystem->GetSelectionCount()) + "\n";
                                status += "Map: " + std::to_string(m_fogOfWarSystem->GetMapWidth()) + "x" +
                                          std::to_string(m_fogOfWarSystem->GetMapHeight()) + "\n";
                                status += "Match players: " + std::to_string(m_matchSystem->GetPlayerCount()) + "\n";
                                return status;
                            });

    console.RegisterCommand("rts_units", [this](const std::vector<std::string>&) -> std::string
                            { return m_unitSystem->GetUnitListString(); });

    console.RegisterCommand("rts_buildings", [this](const std::vector<std::string>&) -> std::string
                            { return m_buildingSystem->GetBuildingListString(); });

    console.RegisterCommand("rts_resources", [this](const std::vector<std::string>&) -> std::string
                            { return m_resourceSystem->GetResourceListString(); });

    // Engine system commands
    console.RegisterCommand("rts_save",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (!m_engineSystems)
                                    return "Engine systems not initialized";
                                std::string slot = args.empty() ? "rts_quicksave" : args[0];
                                return m_engineSystems->SaveMatch(slot) ? "Saved to: " + slot : "Save failed";
                            });

    console.RegisterCommand("rts_load",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (!m_engineSystems)
                                    return "Engine systems not initialized";
                                std::string slot = args.empty() ? "rts_quicksave" : args[0];
                                return m_engineSystems->LoadMatch(slot) ? "Loaded from: " + slot : "Load failed";
                            });

    console.RegisterCommand("rts_weather",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (!m_engineSystems)
                                    return "Engine systems not initialized";
                                if (args.empty())
                                    return "Usage: rts_weather <clear|rain|fog|storm|snow|cloudy>";
                                m_engineSystems->SetWeather(args[0]);
                                return "Weather set to: " + args[0];
                            });

    console.RegisterCommand("rts_time",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (!m_engineSystems)
                                    return "Engine systems not initialized";
                                if (args.empty())
                                    return "Usage: rts_time <0-24>";
                                float hour;
                                try
                                {
                                    hour = std::stof(args[0]);
                                }
                                catch (const std::exception&)
                                {
                                    return "Invalid time value: " + args[0];
                                }
                                m_engineSystems->SetTimeOfDay(hour);
                                return "Time set to: " + args[0];
                            });
}
