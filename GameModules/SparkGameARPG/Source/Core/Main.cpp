/**
 * @file Main.cpp
 * @brief SparkGameARPG DLL - IModule implementation and exports
 *
 * Implements the SparkGameARPGModule class and exports the CreateModule/
 * DestroyModule factory functions for the engine's ModuleManager.
 */

#include "SparkGameARPG.h"
#include "ARPGEngineSystems.h"
#include "Hero/ARPGHeroSystem.h"
#include "Combat/ARPGCombatSystem.h"
#include "Loot/ARPGLootSystem.h"
#include "Dungeon/ARPGDungeonSystem.h"
#include "Skill/ARPGSkillSystem.h"
#include "Monster/ARPGMonsterSystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "Utils/InvalidStateDetector.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"
#include "Engine/ECS/Components/AIComponents.h"
#include "Engine/ECS/Components/PhysicsComponents.h"

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
    SPARK_LOG_INFO(Spark::LogCategory::Game, "ARPG module loading — initializing 7 subsystems");

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

    // Wire engine systems into ARPG gameplay (after all subsystems are ready)
    m_engineSystems = std::make_unique<ARPG::ARPGEngineSystems>();
    if (!m_engineSystems->Initialize(context, m_heroSystem.get(), m_combatSystem.get(), m_lootSystem.get(),
                                     m_dungeonSystem.get()))
    {
        console.LogError("[ARPG] Failed to initialize engine systems integration");
        return false;
    }

    RegisterConsoleCommands();

    // Register ARPG-specific state validation rules
    auto& stateDetector = Spark::InvalidStateDetector::GetInstance();

    stateDetector.AddRule({"ARPG.DeadMobTargeting", "ARPG", Spark::StateViolationSeverity::Error, true,
                           [](World& w, std::vector<Spark::StateViolation>& out)
                           {
                               for (auto entity : w.GetEntitiesWith<HealthComponent, AIComponent>())
                               {
                                   auto* h = w.GetComponent<HealthComponent>(entity);
                                   auto* ai = w.GetComponent<AIComponent>(entity);
                                   if (h && ai && h->isDead && ai->targetEntity != entt::null)
                                   {
                                       out.push_back({"ARPG.DeadMobTargeting", static_cast<uint32_t>(entity),
                                                      "Dead monster still has a target assigned",
                                                      Spark::StateViolationSeverity::Error});
                                   }
                               }
                           }});

    stateDetector.AddRule(
        {"ARPG.StaticBodyDynamic", "ARPG", Spark::StateViolationSeverity::Warning, true,
         [](World& w, std::vector<Spark::StateViolation>& out)
         {
             for (auto entity : w.GetEntitiesWith<RigidBodyComponent, HealthComponent>())
             {
                 auto* rb = w.GetComponent<RigidBodyComponent>(entity);
                 auto* h = w.GetComponent<HealthComponent>(entity);
                 if (rb && h && h->isDead && rb->type == RigidBodyComponent::Type::Dynamic && rb->mass > 0.0f)
                 {
                     float speedSq = rb->linearVelocity.x * rb->linearVelocity.x +
                                     rb->linearVelocity.y * rb->linearVelocity.y +
                                     rb->linearVelocity.z * rb->linearVelocity.z;
                     if (speedSq > 25.0f)
                     {
                         out.push_back({"ARPG.StaticBodyDynamic", static_cast<uint32_t>(entity),
                                        "Dead entity moving at high speed (speedSq=" + std::to_string(speedSq) + ")",
                                        Spark::StateViolationSeverity::Warning});
                     }
                 }
             }
         }});

    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "ARPG module loaded successfully — 7 subsystems active");
    console.LogInfo("[ARPG] Spark ARPG module loaded successfully (7 subsystems)");
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
    SPARK_LOG_INFO(Spark::LogCategory::Game, "ARPG module shutting down");

    // Shutdown engine integrations first (depends on all ARPG subsystems)
    if (m_engineSystems)
    {
        m_engineSystems->Shutdown();
        m_engineSystems.reset();
    }

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
    SPARK_LOG_INFO(Spark::LogCategory::Game, "ARPG module unloaded");
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
    m_engineSystems->Update(deltaTime);
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
    m_engineSystems->RenderDebugUI();
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

    console.RegisterCommand("arpg_save",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                auto* saveSystem = m_context->GetSaveSystem();
                                if (!saveSystem)
                                    return "Save system not available";

                                auto* world = m_context->GetWorld();
                                if (!world)
                                    return "World not available";

                                std::string slot = args.empty() ? "arpg_quicksave" : args[0];
                                Spark::SaveMetadata meta;
                                meta.saveName = "ARPG Save";
                                meta.sceneName =
                                    "Dungeon Floor " + std::to_string(m_dungeonSystem->GetCurrentFloorNumber());

                                if (saveSystem->Save(slot, *world, meta))
                                    return "ARPG state saved to slot: " + slot;
                                return "Failed to save to slot: " + slot;
                            });

    console.RegisterCommand("arpg_load",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                auto* saveSystem = m_context->GetSaveSystem();
                                if (!saveSystem)
                                    return "Save system not available";

                                auto* world = m_context->GetWorld();
                                if (!world)
                                    return "World not available";

                                std::string slot = args.empty() ? "arpg_quicksave" : args[0];
                                if (!saveSystem->SaveExists(slot))
                                    return "No save found in slot: " + slot;

                                if (saveSystem->Load(slot, *world))
                                    return "ARPG state loaded from slot: " + slot;
                                return "Failed to load from slot: " + slot;
                            });

    console.RegisterCommand("arpg_abilities",
                            [this]([[maybe_unused]] const std::vector<std::string>& args) -> std::string
                            {
                                // AbilitySystem.h cannot be included from game modules (types not in scope).
                                // Report the statically-known ARPG ability configuration instead.
                                std::string info = "=== ARPG Abilities ===\n";
                                info += "Configured abilities: 4 (Fireball, Whirlwind, Raise Skeleton, Holy Light)\n";
                                info += "Configured auras: 4 (Holy Shield, Bone Armor, Poison DoT, Fire Mastery)\n";
                                info += "Configured procs: 1 (Fire Mastery proc)\n";
                                return info;
                            });
}
