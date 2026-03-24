/**
 * @file Main.cpp
 * @brief SparkGameARPG DLL - IModule implementation and exports
 *
 * Implements the SparkGameARPGModule class and exports the CreateModule/
 * DestroyModule factory functions for the engine's ModuleManager.
 */

#include "SparkGameARPG.h"
#include "Hero/ARPGHeroSystem.h"
#include "Combat/ARPGCombatSystem.h"
#include "Loot/ARPGLootSystem.h"
#include "Dungeon/ARPGDungeonSystem.h"
#include "Skill/ARPGSkillSystem.h"
#include "Monster/ARPGMonsterSystem.h"
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

SPARK_IMPLEMENT_MODULE(SparkGameARPGModule)

// =============================================================================
// SparkGameARPGModule implementation
// =============================================================================

SparkGameARPGModule::SparkGameARPGModule() = default;

SparkGameARPGModule::~SparkGameARPGModule()
{
    if (m_initialized)
        OnUnload();
}

Spark::ModuleInfo SparkGameARPGModule::GetModuleInfo() const
{
    Spark::ModuleInfo info{};
    info.name = "Spark ARPG - Diablo-style Engine Showcase";
    info.version = "1.0.0";
    info.sdkVersion = SPARK_SDK_VERSION;
    info.loadOrder = 1003; // Load after SparkGame, SparkGameMMO, and SparkGameRPG
    return info;
}

bool SparkGameARPGModule::OnLoad(Spark::IEngineContext* context)
{
    if (!context)
        return false;

    m_context = context;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[ARPG] Loading Spark ARPG module...");

    // Initialize hero system (classes, stats, leveling)
    m_heroSystem = std::make_unique<ARPG::ARPGHeroSystem>();
    if (!m_heroSystem->Initialize(context))
    {
        console.LogError("[ARPG] Failed to initialize hero system");
        return false;
    }

    // Initialize combat system (damage, crits, AoE, DoTs)
    m_combatSystem = std::make_unique<ARPG::ARPGCombatSystem>();
    if (!m_combatSystem->Initialize(context))
    {
        console.LogError("[ARPG] Failed to initialize combat system");
        return false;
    }

    // Initialize loot system (affixes, rarity, item generation)
    m_lootSystem = std::make_unique<ARPG::ARPGLootSystem>();
    if (!m_lootSystem->Initialize(context))
    {
        console.LogError("[ARPG] Failed to initialize loot system");
        return false;
    }

    // Initialize dungeon system (floors, tiers, scaling)
    m_dungeonSystem = std::make_unique<ARPG::ARPGDungeonSystem>();
    if (!m_dungeonSystem->Initialize(context))
    {
        console.LogError("[ARPG] Failed to initialize dungeon system");
        return false;
    }

    // Initialize skill system (skill trees, cooldowns)
    m_skillSystem = std::make_unique<ARPG::ARPGSkillSystem>();
    if (!m_skillSystem->Initialize(context))
    {
        console.LogError("[ARPG] Failed to initialize skill system");
        return false;
    }

    // Initialize monster system (templates, spawning, affixes)
    m_monsterSystem = std::make_unique<ARPG::ARPGMonsterSystem>();
    if (!m_monsterSystem->Initialize(context))
    {
        console.LogError("[ARPG] Failed to initialize monster system");
        return false;
    }

    RegisterConsoleCommands();

    m_initialized = true;
    console.LogInfo("[ARPG] Spark ARPG module loaded successfully (6 subsystems)");
    console.LogInfo("[ARPG] Classes: " + std::to_string(m_heroSystem->GetClassCount()) +
                    " | Skills: " + std::to_string(m_skillSystem->GetTotalSkillCount()) +
                    " | Monsters: " + std::to_string(m_monsterSystem->GetTemplateCount()) +
                    " | Affixes: " + std::to_string(m_lootSystem->GetAffixPoolSize()) +
                    " | Tiers: " + std::to_string(m_dungeonSystem->GetTierCount()));
    return true;
}

void SparkGameARPGModule::OnUnload()
{
    if (!m_initialized)
        return;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[ARPG] Unloading Spark ARPG module...");

    // Shutdown in reverse initialization order
    if (m_monsterSystem)
    {
        m_monsterSystem->Shutdown();
        m_monsterSystem.reset();
    }
    if (m_skillSystem)
    {
        m_skillSystem->Shutdown();
        m_skillSystem.reset();
    }
    if (m_dungeonSystem)
    {
        m_dungeonSystem->Shutdown();
        m_dungeonSystem.reset();
    }
    if (m_lootSystem)
    {
        m_lootSystem->Shutdown();
        m_lootSystem.reset();
    }
    if (m_combatSystem)
    {
        m_combatSystem->Shutdown();
        m_combatSystem.reset();
    }
    if (m_heroSystem)
    {
        m_heroSystem->Shutdown();
        m_heroSystem.reset();
    }

    m_context = nullptr;
    m_initialized = false;
    console.LogInfo("[ARPG] Spark ARPG module unloaded");
}

void SparkGameARPGModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_heroSystem->Update(deltaTime);
    m_combatSystem->Update(deltaTime);
    m_skillSystem->Update(deltaTime);
    m_dungeonSystem->Update(deltaTime);
    m_monsterSystem->Update(deltaTime);
}

void SparkGameARPGModule::OnFixedUpdate(float fixedDeltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_combatSystem->FixedUpdate(fixedDeltaTime);
}

void SparkGameARPGModule::OnRender()
{
    if (!m_initialized)
        return;
}

void SparkGameARPGModule::OnResize(int width, int height)
{
    (void)width;
    (void)height;
}

void SparkGameARPGModule::OnPause()
{
    m_paused = true;
}

void SparkGameARPGModule::OnResume()
{
    m_paused = false;
}

void SparkGameARPGModule::OnImGui()
{
    if (!m_initialized)
        return;

    m_heroSystem->RenderDebugUI();
    m_combatSystem->RenderDebugUI();
    m_lootSystem->RenderDebugUI();
    m_dungeonSystem->RenderDebugUI();
    m_skillSystem->RenderDebugUI();
    m_monsterSystem->RenderDebugUI();
}

void SparkGameARPGModule::RegisterConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    console.RegisterCommand(
        "arpg_status",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_heroSystem)
                return "ARPG module not initialized";

            std::string status = "=== Spark ARPG Status ===\n";
            status += "Hero classes: " + std::to_string(m_heroSystem->GetClassCount()) + "\n";
            status += "Active heroes: " + std::to_string(m_heroSystem->GetHeroCount()) + "\n";
            status += "Skills: " + std::to_string(m_skillSystem->GetTotalSkillCount()) + "\n";
            status += "Monster templates: " + std::to_string(m_monsterSystem->GetTemplateCount()) + "\n";
            status += "Active monsters: " + std::to_string(m_monsterSystem->GetActiveMonsterCount()) + "\n";
            status += "Affix pool: " + std::to_string(m_lootSystem->GetAffixPoolSize()) + "\n";
            status += "Items generated: " + std::to_string(m_lootSystem->GetGeneratedItemCount()) + "\n";
            status += "Dungeon floor: " + std::to_string(m_dungeonSystem->GetCurrentFloorNumber()) + "\n";
            status += "Attacks processed: " + std::to_string(m_combatSystem->GetAttacksProcessed()) + "\n";
            return status;
        });

    console.RegisterCommand("arpg_heroes", [this](const std::vector<std::string>&) -> std::string
                            { return m_heroSystem->GetHeroListString(); });

    console.RegisterCommand("arpg_loot", [this](const std::vector<std::string>&) -> std::string
                            { return m_lootSystem->GetLootInfoString(); });

    console.RegisterCommand("arpg_dungeon", [this](const std::vector<std::string>&) -> std::string
                            { return m_dungeonSystem->GetDungeonStatusString(); });

    console.RegisterCommand("arpg_skills", [this](const std::vector<std::string>&) -> std::string
                            { return m_skillSystem->GetSkillListString(); });

    console.RegisterCommand("arpg_monsters", [this](const std::vector<std::string>&) -> std::string
                            { return m_monsterSystem->GetMonsterListString(); });
}
