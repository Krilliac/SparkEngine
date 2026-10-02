/**
 * @file Main.cpp
 * @brief SparkGameRTS DLL - IModule implementation and exports
 *
 * Implements the SparkGameRTSModule class and exports the CreateModule/
 * DestroyModule factory functions for the engine's ModuleManager.
 */

#include "SparkGameRTS.h"
#include "RTSEngineSystems.h"
#include "Demo/RTSDemoPresentation.h"
#include "Unit/RTSUnitSystem.h"
#include "Building/RTSBuildingSystem.h"
#include "Resource/RTSResourceSystem.h"
#include "Command/RTSCommandSystem.h"
#include "FogOfWar/RTSFogOfWarSystem.h"
#include "Match/RTSMatchSystem.h"
#include "Simulation/RTSScriptedCommander.h"
#include "Simulation/RTSSkirmishSimulation.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"
#include "Engine/ECS/Components/AIComponents.h"

#include <Spark/IStateValidation.h>
#include <Spark/ModuleDllMain.h>
#include <Spark/ModuleLog.h>

#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    const char* MatchStateName(RTS::RTSMatchState state)
    {
        constexpr std::array<const char*, static_cast<size_t>(RTS::RTSMatchState::Count)> names{
            "Setup", "Playing", "Paused", "Victory", "Defeat"};
        const auto index = static_cast<size_t>(state);
        return index < names.size() ? names[index] : "Unknown";
    }
} // namespace

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

    Spark::ModuleLog::Info(context, "[RTS] Loading Spark RTS module (7 subsystems)...");

    // Initialize unit system (faction templates, spawning, AI)
    m_unitSystem = std::make_unique<RTS::RTSUnitSystem>();
    if (!m_unitSystem->Initialize(context))
    {
        Spark::ModuleLog::Error(context, "[RTS] Failed to initialize unit system");
        return false;
    }

    // Initialize resource system (minerals, gas, supply)
    m_resourceSystem = std::make_unique<RTS::RTSResourceSystem>();
    if (!m_resourceSystem->Initialize(context, m_unitSystem.get()))
    {
        Spark::ModuleLog::Error(context, "[RTS] Failed to initialize resource system");
        return false;
    }

    // Initialize building system (tech trees, production queues)
    m_buildingSystem = std::make_unique<RTS::RTSBuildingSystem>();
    if (!m_buildingSystem->Initialize(context, m_unitSystem.get(), m_resourceSystem.get()))
    {
        Spark::ModuleLog::Error(context, "[RTS] Failed to initialize building system");
        return false;
    }

    // Initialize command system (selection, move, attack, patrol)
    m_commandSystem = std::make_unique<RTS::RTSCommandSystem>();
    if (!m_commandSystem->Initialize(context, m_unitSystem.get(), m_buildingSystem.get()))
    {
        Spark::ModuleLog::Error(context, "[RTS] Failed to initialize command system");
        return false;
    }

    // Initialize fog of war (grid-based vision)
    m_fogOfWarSystem = std::make_unique<RTS::RTSFogOfWarSystem>();
    if (!m_fogOfWarSystem->Initialize(context))
    {
        Spark::ModuleLog::Error(context, "[RTS] Failed to initialize fog of war system");
        return false;
    }

    // Initialize match system (setup, win conditions)
    m_matchSystem = std::make_unique<RTS::RTSMatchSystem>();
    if (!m_matchSystem->Initialize(context))
    {
        Spark::ModuleLog::Error(context, "[RTS] Failed to initialize match system");
        return false;
    }

    // Fixed-step skirmish tick: the only place gameplay systems are advanced
    const RTS::RTSSkirmishSystems gameplaySystems{m_unitSystem.get(),    m_buildingSystem.get(), m_resourceSystem.get(),
                                                  m_commandSystem.get(), m_fogOfWarSystem.get(), m_matchSystem.get()};
    m_simulation = std::make_unique<RTS::RTSSkirmishSimulation>();
    if (!m_simulation->Initialize(context, gameplaySystems))
    {
        Spark::ModuleLog::Error(context, "[RTS] Failed to initialize skirmish simulation");
        return false;
    }

    // Engine system integrations (AI, events, audio, weather, destruction, save, coroutines). Save/load persist
    // the full skirmish, so the bridge binds every gameplay system and the simulation clock.
    m_engineSystems = std::make_unique<RTS::RTSEngineSystems>();
    if (!m_engineSystems->Initialize(context, gameplaySystems, m_simulation.get()))
    {
        Spark::ModuleLog::Warn(context, "[RTS] Engine system integrations partially unavailable (non-fatal)");
    }

    m_demoPresentation = std::make_unique<RTS::RTSDemoPresentation>();
    if (!m_demoPresentation->Initialize(context, m_unitSystem.get(), m_buildingSystem.get(), m_resourceSystem.get(),
                                        m_commandSystem.get(), m_fogOfWarSystem.get(), m_matchSystem.get(),
                                        m_simulation.get()))
    {
        Spark::ModuleLog::Error(context, "[RTS] Failed to initialize playable demo");
        return false;
    }

    RegisterConsoleCommands();

    // Register RTS-specific state validation rules
    Spark::IStateValidation* stateRules = m_context->GetStateValidation();
    const bool stateRulesRegistered =
        stateRules != nullptr &&
        stateRules->AddRule("RTS.DeadUnitAttacking", "RTS", Spark::StateViolationSeverity::Error,
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
                            }) &&
        stateRules->AddRule("RTS.IdleWithTarget", "RTS", Spark::StateViolationSeverity::Warning,
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
                            });
    if (!stateRulesRegistered)
    {
        Spark::ModuleLog::Warn(m_context, "[RTS] Host refused the RTS state-validation rules");
    }

    m_initialized = true;
    Spark::ModuleLog::Info(context, "[RTS] Spark RTS module loaded successfully (7 subsystems)");
    Spark::ModuleLog::Info(context, "[RTS] Units: {} | Buildings: {} | Nodes: {}", m_unitSystem->GetUnitCount(),
                           m_buildingSystem->GetBuildingCount(), m_resourceSystem->GetNodeCount());
    return true;
}

void SparkGameRTSModule::OnUnload()
{
    if (!m_initialized)
        return;

    // Validation callbacks are std::functions implemented in this DLL. Drop
    // them before the module image is unmapped during hot unload/reload.
    if (Spark::IStateValidation* stateRules = m_context ? m_context->GetStateValidation() : nullptr)
    {
        stateRules->RemoveRulesByCategory("RTS");
    }

    // Command handlers are std::functions in this DLL too, and they reference the systems torn down below.
    if (Spark::IConsole* console = m_context ? m_context->GetConsole() : nullptr)
    {
        for (const std::string& name : m_consoleCommands)
        {
            console->UnregisterCommand(name);
        }
    }
    m_consoleCommands.clear();

    Spark::ModuleLog::Info(m_context, "[RTS] Unloading Spark RTS module...");

    // Shutdown in reverse initialization order
    if (m_demoPresentation)
    {
        m_demoPresentation->Shutdown();
        m_demoPresentation.reset();
    }
    if (m_engineSystems)
    {
        m_engineSystems->Shutdown();
        m_engineSystems.reset();
    }
    if (m_simulation)
    {
        m_simulation->Shutdown();
        m_simulation.reset();
    }
    m_scriptedCommander.reset();
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
    if (m_buildingSystem)
    {
        // Buildings release supply reserved by queued production through the
        // resource system, so their dependency must remain alive here.
        m_buildingSystem->Shutdown();
        m_buildingSystem.reset();
    }
    if (m_resourceSystem)
    {
        m_resourceSystem->Shutdown();
        m_resourceSystem.reset();
    }
    if (m_unitSystem)
    {
        m_unitSystem->Shutdown();
        m_unitSystem.reset();
    }

    Spark::ModuleLog::Info(m_context, "[RTS] Spark RTS module unloaded");
    m_context = nullptr;
    m_initialized = false;
}

void SparkGameRTSModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_demoPresentation->UpdateInput();
    // Frame time only decides how many whole fixed ticks run; the simulated outcome never depends on it.
    m_simulation->Advance(deltaTime);
    m_demoPresentation->SyncKitProps();
    if (m_engineSystems)
        m_engineSystems->Update(deltaTime);
}

void SparkGameRTSModule::OnFixedUpdate(float fixedDeltaTime)
{
    if (!m_initialized || m_paused)
        return;

    // The skirmish owns its own fixed tick (RTSSkirmishSimulation::TICK_SECONDS) so its outcome cannot change
    // with the host's fixed-step rate, which differs between the client and dedicated-server loops.
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
    m_demoPresentation->RenderUI();
}

void SparkGameRTSModule::RegisterConsoleCommands()
{
    Spark::IConsole* host = m_context->GetConsole();
    if (!host)
    {
        Spark::ModuleLog::Warn(m_context, "[RTS] Host has no console; RTS commands are unavailable");
        return;
    }
    // Remember each accepted name so OnUnload removes exactly what this module registered.
    auto registerCommand = [this, host](std::string_view name, Spark::IConsole::CommandHandler handler)
    {
        if (host->RegisterCommand(name, std::move(handler), "", "RTS", ""))
        {
            m_consoleCommands.emplace_back(name);
        }
        else
        {
            Spark::ModuleLog::Warn(m_context, "[RTS] Console command '{}' was not registered", name);
        }
    };

    registerCommand("rts_status",
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
                        status += "Tick: " + std::to_string(m_simulation->GetTick()) + "\n";
                        status += std::string("Match: ") + MatchStateName(m_matchSystem->GetMatchState()) + "\n";
                        status += std::format("State hash: {:016x}\n", m_simulation->ComputeStateHash());
                        return status;
                    });

    // Automated player for packaged runs. Its orders are scheduled by simulation tick, so turning it on restarts
    // the default skirmish and the whole match -- win or loss and final state hash -- depends only on the tick.
    registerCommand("rts_autoplay",
                    [this](const std::vector<std::string>& args) -> std::string
                    {
                        if (args.size() != 1 || (args[0] != "on" && args[0] != "off"))
                        {
                            return "Usage: rts_autoplay on|off";
                        }
                        if (args[0] == "off")
                        {
                            m_simulation->SetScriptedCommander(nullptr);
                            m_scriptedCommander.reset();
                            return "Autoplay off";
                        }
                        if (!m_simulation->StartDefaultSkirmish())
                        {
                            return "Autoplay failed: the default skirmish did not restart";
                        }
                        m_scriptedCommander = std::make_unique<RTS::RTSScriptedCommander>();
                        m_simulation->SetScriptedCommander(m_scriptedCommander.get());
                        return "Autoplay on: default skirmish restarted with the scripted Human commander";
                    });

    registerCommand("rts_units", [this](const std::vector<std::string>&) -> std::string
                    { return m_unitSystem->GetUnitListString(); });

    registerCommand("rts_buildings", [this](const std::vector<std::string>&) -> std::string
                    { return m_buildingSystem->GetBuildingListString(); });

    registerCommand("rts_resources", [this](const std::vector<std::string>&) -> std::string
                    { return m_resourceSystem->GetResourceListString(); });

    registerCommand("rts_demo_reset", [this](const std::vector<std::string>&) -> std::string
                    { return m_demoPresentation->Reset() ? "RTS demo reset" : "RTS demo reset failed"; });

    registerCommand("rts_select",
                    [this](const std::vector<std::string>& args) -> std::string
                    {
                        if (args.empty())
                            return "Usage: rts_select <workers|marines|tanks|army>";
                        if (args[0] == "workers")
                            m_demoPresentation->SelectUnitType(RTS::RTSUnitType::Worker);
                        else if (args[0] == "marines")
                            m_demoPresentation->SelectUnitType(RTS::RTSUnitType::Marine);
                        else if (args[0] == "tanks")
                            m_demoPresentation->SelectUnitType(RTS::RTSUnitType::Tank);
                        else if (args[0] == "army")
                            m_demoPresentation->SelectArmy();
                        else
                            return "Unknown group: " + args[0];
                        return "Selected " + std::to_string(m_commandSystem->GetSelectionCount()) + " units";
                    });

    registerCommand("rts_move",
                    [this](const std::vector<std::string>& args) -> std::string
                    {
                        if (args.size() < 2)
                            return "Usage: rts_move <x> <y> [queue]";
                        try
                        {
                            const bool queued = args.size() >= 3 && args[2] == "queue";
                            return m_demoPresentation->MoveSelection(std::stof(args[0]), std::stof(args[1]), queued)
                                       ? "Move order issued"
                                       : "Move order rejected";
                        }
                        catch (const std::exception&)
                        {
                            return "Invalid move coordinates";
                        }
                    });

    registerCommand("rts_hold", [this](const std::vector<std::string>&) -> std::string
                    { return m_demoPresentation->HoldSelection() ? "Hold order issued" : "No units selected"; });
    registerCommand("rts_stop", [this](const std::vector<std::string>&) -> std::string
                    { return m_demoPresentation->StopSelection() ? "Stop order issued" : "No units selected"; });
    registerCommand("rts_train_marine", [this](const std::vector<std::string>&) -> std::string
                    { return m_demoPresentation->TrainMarine() ? "Marine queued" : "Marine queue rejected"; });

    // Engine system commands
    registerCommand("rts_save",
                    [this](const std::vector<std::string>& args) -> std::string
                    {
                        if (!m_engineSystems)
                            return "Engine systems not initialized";
                        std::string slot = args.empty() ? "rts_quicksave" : args[0];
                        if (!RTS::RTSEngineSystems::IsValidSlotName(slot))
                            return "Invalid slot name: use 1-64 letters, digits, '_' or '-'";
                        return m_engineSystems->SaveMatch(slot) ? "Saved to: " + slot : "Save failed";
                    });

    registerCommand("rts_load",
                    [this](const std::vector<std::string>& args) -> std::string
                    {
                        if (!m_engineSystems)
                            return "Engine systems not initialized";
                        std::string slot = args.empty() ? "rts_quicksave" : args[0];
                        if (!RTS::RTSEngineSystems::IsValidSlotName(slot))
                            return "Invalid slot name: use 1-64 letters, digits, '_' or '-'";
                        return m_engineSystems->LoadMatch(slot) ? "Loaded from: " + slot : "Load failed";
                    });

    registerCommand("rts_weather",
                    [this](const std::vector<std::string>& args) -> std::string
                    {
                        if (!m_engineSystems)
                            return "Engine systems not initialized";
                        if (args.empty())
                            return "Usage: rts_weather <clear|rain|fog|storm|snow|cloudy>";
                        m_engineSystems->SetWeather(args[0]);
                        return "Weather set to: " + args[0];
                    });

    registerCommand("rts_time",
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
