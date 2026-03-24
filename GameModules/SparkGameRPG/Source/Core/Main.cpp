/**
 * @file Main.cpp
 * @brief SparkGameRPG DLL - IModule implementation and exports
 *
 * Implements the SparkGameRPGModule class and exports the CreateModule/
 * DestroyModule factory functions for the engine's ModuleManager.
 */

#include "SparkGameRPG.h"
#include "World/RPGWorldSetup.h"
#include "Character/RPGCharacterSystem.h"
#include "Combat/RPGCombatSystem.h"
#include "Dialogue/RPGDialogueSystem.h"
#include "Quest/RPGQuestSystem.h"
#include "Inventory/RPGInventorySystem.h"
#include "NPC/RPGNPCSystem.h"
#include "Utils/SparkConsole.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>

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

    // Initialize dialogue system (branching trees, skill checks)
    m_dialogueSystem = std::make_unique<RPG::RPGDialogueSystem>();
    if (!m_dialogueSystem->Initialize(context))
    {
        console.LogError("[RPG] Failed to initialize dialogue system");
        return false;
    }

    // Initialize quest system (objectives, chains, rewards)
    m_questSystem = std::make_unique<RPG::RPGQuestSystem>();
    if (!m_questSystem->Initialize(context))
    {
        console.LogError("[RPG] Failed to initialize quest system");
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

    RegisterConsoleCommands();

    m_initialized = true;
    console.LogInfo("[RPG] Spark RPG module loaded successfully (7 subsystems)");
    console.LogInfo("[RPG] Areas: " + std::to_string(m_worldSetup->GetAreaCount()) +
                    " | Classes: " + std::to_string(m_characterSystem->GetClassCount()) +
                    " | Items: " + std::to_string(m_inventorySystem->GetItemCount()) +
                    " | Quests: " + std::to_string(m_questSystem->GetQuestCount()) +
                    " | NPCs: " + std::to_string(m_npcSystem->GetNPCCount()));
    return true;
}

void SparkGameRPGModule::OnUnload()
{
    if (!m_initialized)
        return;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[RPG] Unloading Spark RPG module...");

    // Shutdown in reverse initialization order
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
    if (m_questSystem)
    {
        m_questSystem->Shutdown();
        m_questSystem.reset();
    }
    if (m_dialogueSystem)
    {
        m_dialogueSystem->Shutdown();
        m_dialogueSystem.reset();
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
    console.LogInfo("[RPG] Spark RPG module unloaded");
}

void SparkGameRPGModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_worldSetup->Update(deltaTime);
    m_combatSystem->Update(deltaTime);
    m_npcSystem->Update(deltaTime);
    m_questSystem->Update(deltaTime);
    m_dialogueSystem->Update(deltaTime);
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
    m_dialogueSystem->RenderDebugUI();
    m_questSystem->RenderDebugUI();
    m_inventorySystem->RenderDebugUI();
    m_npcSystem->RenderDebugUI();
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
                                status += "Quests: " + std::to_string(m_questSystem->GetQuestCount()) + "\n";
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
                            { return m_questSystem->GetQuestListString(); });

    console.RegisterCommand("rpg_npcs", [this](const std::vector<std::string>&) -> std::string
                            { return m_npcSystem->GetNPCListString(); });

    console.RegisterCommand("rpg_items", [this](const std::vector<std::string>&) -> std::string
                            { return m_inventorySystem->GetItemListString(); });
}
