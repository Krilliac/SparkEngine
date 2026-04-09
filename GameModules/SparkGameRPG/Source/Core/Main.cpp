/**
 * @file Main.cpp
 * @brief SparkGameRPG DLL - IModule implementation and exports
 *
 * Implements the SparkGameRPGModule class and exports the CreateModule/
 * DestroyModule factory functions for the engine's ModuleManager.
 */

#include "SparkGameRPG.h"
#include "RPGEngineSystems.h"
#include "Gameplay/RPGGameplayBridge.h"
#include "World/RPGWorldSetup.h"
#include "Character/RPGCharacterSystem.h"
#include "Combat/RPGCombatSystem.h"
#include "Inventory/RPGInventorySystem.h"
#include "NPC/RPGNPCSystem.h"
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

SPARK_IMPLEMENT_MODULE(SparkGameRPGModule)

// =============================================================================
// SparkGameRPGModule implementation
// =============================================================================

SparkGameRPGModule::SparkGameRPGModule() = default;

SparkGameRPGModule::~SparkGameRPGModule()
{
    if (m_initialized)
        OnUnload();
}

Spark::ModuleInfo SparkGameRPGModule::GetModuleInfo() const
{
    Spark::ModuleInfo info{};
    info.name = "Spark RPG - Engine Showcase";
    info.version = "1.0.0";
    info.sdkVersion = SPARK_SDK_VERSION;
    info.loadOrder = 1002; // Load after SparkGame and SparkGameMMO if present
    return info;
}

bool SparkGameRPGModule::OnLoad(Spark::IEngineContext* context)
{
    if (!context)
        return false;

    m_context = context;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[RPG] Loading Spark RPG module...");
    SPARK_LOG_INFO(Spark::LogCategory::Game, "RPG module loading — initializing 8 subsystems");

    // Initialize the world area setup (registers areas with streaming)
    m_worldSetup = std::make_unique<RPG::RPGWorldSetup>();
    if (!m_worldSetup->Initialize(context))
    {
        console.LogError("[RPG] Failed to initialize world setup");
        return false;
    }

    // Initialize character system (classes, stats, leveling)
    m_characterSystem = std::make_unique<RPG::RPGCharacterSystem>();
    if (!m_characterSystem->Initialize(context))
    {
        console.LogError("[RPG] Failed to initialize character system");
        return false;
    }

    // Initialize combat system (damage, cooldowns, combos)
    m_combatSystem = std::make_unique<RPG::RPGCombatSystem>();
    if (!m_combatSystem->Initialize(context))
    {
        console.LogError("[RPG] Failed to initialize combat system");
        return false;
    }

    // Initialize bridge to engine quest/dialogue systems with RPG policies/data
    m_gameplayBridge = std::make_unique<RPG::RPGGameplayBridge>();
    if (!m_gameplayBridge->Initialize(context, m_characterSystem.get()))
    {
        console.LogError("[RPG] Failed to initialize gameplay bridge");
        return false;
    }

    // Initialize inventory system (items, equipment, weight)
    m_inventorySystem = std::make_unique<RPG::RPGInventorySystem>();
    if (!m_inventorySystem->Initialize(context))
    {
        console.LogError("[RPG] Failed to initialize inventory system");
        return false;
    }

    // Initialize NPC system (AI, schedules, disposition)
    m_npcSystem = std::make_unique<RPG::RPGNPCSystem>();
    if (!m_npcSystem->Initialize(context))
    {
        console.LogError("[RPG] Failed to initialize NPC system");
        return false;
    }

    // Initialize engine system integrations (save, animation, AI, cinematic, etc.)
    m_engineSystems = std::make_unique<RPG::RPGEngineSystems>();
    if (!m_engineSystems->Initialize(context))
    {
        console.LogWarning("[RPG] Engine systems integration partially failed (non-fatal)");
    }

    RegisterConsoleCommands();

    // Register RPG-specific state validation rules
    auto& stateDetector = Spark::InvalidStateDetector::GetInstance();

    stateDetector.AddRule({"RPG.DeadAIPatrolling", "RPG", Spark::StateViolationSeverity::Error, true,
                           [](World& w, std::vector<Spark::StateViolation>& out)
                           {
                               for (auto entity : w.GetEntitiesWith<HealthComponent, AIComponent>())
                               {
                                   auto* h = w.GetComponent<HealthComponent>(entity);
                                   auto* ai = w.GetComponent<AIComponent>(entity);
                                   if (h && ai && h->isDead && ai->state == AIComponent::State::Patrolling)
                                   {
                                       out.push_back({"RPG.DeadAIPatrolling", static_cast<uint32_t>(entity),
                                                      "Dead NPC is still patrolling",
                                                      Spark::StateViolationSeverity::Error});
                                   }
                               }
                           }});

    stateDetector.AddRule({"RPG.NegativeHealth", "RPG", Spark::StateViolationSeverity::Warning, true,
                           [](World& w, std::vector<Spark::StateViolation>& out)
                           {
                               for (auto entity : w.GetEntitiesWith<HealthComponent>())
                               {
                                   auto* h = w.GetComponent<HealthComponent>(entity);
                                   if (h && h->health < 0.0f)
                                   {
                                       out.push_back({"RPG.NegativeHealth", static_cast<uint32_t>(entity),
                                                      "health=" + std::to_string(h->health) + " is negative",
                                                      Spark::StateViolationSeverity::Warning});
                                   }
                               }
                           }});

    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "RPG module loaded successfully — 7 subsystems active");
    console.LogInfo("[RPG] Spark RPG module loaded successfully (7 subsystems)");
    console.LogInfo("[RPG] Areas: " + std::to_string(m_worldSetup->GetAreaCount()) +
                    " | Classes: " + std::to_string(m_characterSystem->GetClassCount()) +
                    " | Items: " + std::to_string(m_inventorySystem->GetItemCount()) +
                    " | Quests: " + std::to_string(m_gameplayBridge->GetRegisteredQuestCount()) +
                    " | NPCs: " + std::to_string(m_npcSystem->GetNPCCount()));
    return true;
}

void SparkGameRPGModule::OnUnload()
{
    if (!m_initialized)
        return;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[RPG] Unloading Spark RPG module...");
    SPARK_LOG_INFO(Spark::LogCategory::Game, "RPG module shutting down");

    // Shutdown in reverse initialization order
    if (m_engineSystems)
    {
        m_engineSystems->Shutdown();
        m_engineSystems.reset();
    }
    if (m_npcSystem)
    {
        m_npcSystem->Shutdown();
        m_npcSystem.reset();
    }
    if (m_inventorySystem)
    {
        m_inventorySystem->Shutdown();
        m_inventorySystem.reset();
    }
    if (m_gameplayBridge)
    {
        m_gameplayBridge->Shutdown();
        m_gameplayBridge.reset();
    }
    if (m_combatSystem)
    {
        m_combatSystem->Shutdown();
        m_combatSystem.reset();
    }
    if (m_characterSystem)
    {
        m_characterSystem->Shutdown();
        m_characterSystem.reset();
    }
    if (m_worldSetup)
    {
        m_worldSetup->Shutdown();
        m_worldSetup.reset();
    }

    m_context = nullptr;
    m_initialized = false;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "RPG module unloaded");
    console.LogInfo("[RPG] Spark RPG module unloaded");
}

void SparkGameRPGModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_worldSetup->Update(deltaTime);
    m_combatSystem->Update(deltaTime);
    m_npcSystem->Update(deltaTime);
    m_gameplayBridge->Update(deltaTime);
    m_engineSystems->Update(deltaTime);
}

void SparkGameRPGModule::OnFixedUpdate(float fixedDeltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_combatSystem->FixedUpdate(fixedDeltaTime);
}

void SparkGameRPGModule::OnRender()
{
    if (!m_initialized)
        return;
}

void SparkGameRPGModule::OnResize(int width, int height)
{
    (void)width;
    (void)height;
}

void SparkGameRPGModule::OnPause()
{
    m_paused = true;
}

void SparkGameRPGModule::OnResume()
{
    m_paused = false;
}

void SparkGameRPGModule::OnImGui()
{
    if (!m_initialized)
        return;

    m_worldSetup->RenderDebugUI();
    m_characterSystem->RenderDebugUI();
    m_combatSystem->RenderDebugUI();
    m_gameplayBridge->RenderDebugUI();
    m_inventorySystem->RenderDebugUI();
    m_npcSystem->RenderDebugUI();
    m_engineSystems->RenderDebugUI();
}

void SparkGameRPGModule::RegisterConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    console.RegisterCommand("rpg_status",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                if (!m_worldSetup)
                                    return "RPG module not initialized";

                                std::string status = "=== Spark RPG Status ===\n";
                                status += "Areas: " + std::to_string(m_worldSetup->GetAreaCount()) + "\n";
                                status += "Classes: " + std::to_string(m_characterSystem->GetClassCount()) + "\n";
                                status += "Items: " + std::to_string(m_inventorySystem->GetItemCount()) + "\n";
                                status +=
                                    "Quests: " + std::to_string(m_gameplayBridge->GetRegisteredQuestCount()) + "\n";
                                status += "NPCs: " + std::to_string(m_npcSystem->GetNPCCount()) + "\n";
                                status +=
                                    "Active combats: " + std::to_string(m_combatSystem->GetActiveCombatCount()) + "\n";
                                return status;
                            });

    console.RegisterCommand("rpg_areas", [this](const std::vector<std::string>&) -> std::string
                            { return m_worldSetup->GetAreaListString(); });

    console.RegisterCommand("rpg_classes", [this](const std::vector<std::string>&) -> std::string
                            { return m_characterSystem->GetClassListString(); });

    console.RegisterCommand("rpg_quests", [this](const std::vector<std::string>&) -> std::string
                            { return Spark::Gameplay::QuestSystem::GetInstance().Console_GetStatus(); });

    console.RegisterCommand("rpg_npcs", [this](const std::vector<std::string>&) -> std::string
                            { return m_npcSystem->GetNPCListString(); });

    console.RegisterCommand("rpg_items", [this](const std::vector<std::string>&) -> std::string
                            { return m_inventorySystem->GetItemListString(); });

    // Engine system integration commands
    console.RegisterCommand("rpg_save",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (!m_engineSystems)
                                    return "Engine systems not initialized";
                                std::string slot = args.empty() ? "slot1" : args[0];
                                return m_engineSystems->SaveGame(slot);
                            });

    console.RegisterCommand("rpg_load",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (!m_engineSystems)
                                    return "Engine systems not initialized";
                                std::string slot = args.empty() ? "slot1" : args[0];
                                return m_engineSystems->LoadGame(slot);
                            });

    console.RegisterCommand("rpg_weather",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (!m_engineSystems)
                                    return "Engine systems not initialized";
                                if (args.empty())
                                    return "Usage: rpg_weather <clear|cloudy|rain|snow|fog|storm>";
                                return m_engineSystems->SetWeather(args[0]);
                            });

    console.RegisterCommand("rpg_time",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (!m_engineSystems)
                                    return "Engine systems not initialized";
                                if (args.empty())
                                    return "Usage: rpg_time <hour> (0-24)";
                                float hour = 0.0f;
                                try
                                {
                                    hour = std::stof(args[0]);
                                }
                                catch (...)
                                {
                                    return "Invalid hour value: " + args[0];
                                }
                                return m_engineSystems->SetTime(hour);
                            });
}
