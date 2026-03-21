/**
 * @file Main.cpp
 * @brief SparkGameMMO DLL - IModule implementation and exports
 *
 * Implements the SparkGameMMOModule class and exports the CreateModule/
 * DestroyModule factory functions for the engine's ModuleManager.
 */

#include "SparkGameMMO.h"
#include "World/MMOWorldSetup.h"
#include "Player/MMOPlayerSystem.h"
#include "Chat/MMOChatSystem.h"
#include "Inventory/MMOInventorySystem.h"
#include "Crafting/MMOCraftingSystem.h"
#include "Guild/MMOGuildSystem.h"
#include "Trading/MMOTradingSystem.h"
#include "Party/MMOPartySystem.h"
#include "Achievement/MMOAchievementSystem.h"
#include "Reputation/MMOReputationSystem.h"
#include "Dungeon/MMODungeonSystem.h"
#include "WorldBoss/MMOWorldBossSystem.h"
#include "Persistence/MMOPersistenceSystem.h"
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

SPARK_IMPLEMENT_MODULE(SparkGameMMOModule)

// =============================================================================
// SparkGameMMOModule implementation
// =============================================================================

SparkGameMMOModule::SparkGameMMOModule() = default;

SparkGameMMOModule::~SparkGameMMOModule()
{
    if (m_initialized)
        OnUnload();
}

Spark::ModuleInfo SparkGameMMOModule::GetModuleInfo() const
{
    Spark::ModuleInfo info{};
    info.name = "Spark MMO - Networking Showcase";
    info.version = "1.0.0";
    info.sdkVersion = SPARK_SDK_VERSION;
    info.loadOrder = 1001; // Load after SparkGame if both present
    return info;
}

bool SparkGameMMOModule::OnLoad(Spark::IEngineContext* context)
{
    if (!context)
        return false;

    m_context = context;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[MMO] Loading Spark MMO module...");

    // Initialize the world area setup (registers areas with streaming/area servers)
    m_worldSetup = std::make_unique<MMO::MMOWorldSetup>();
    if (!m_worldSetup->Initialize(context))
    {
        console.LogError("[MMO] Failed to initialize world setup");
        return false;
    }

    // Initialize player system (spawning, replication, prediction)
    m_playerSystem = std::make_unique<MMO::MMOPlayerSystem>();
    if (!m_playerSystem->Initialize(context))
    {
        console.LogError("[MMO] Failed to initialize player system");
        return false;
    }

    // Initialize chat system (area, global, party, whisper channels)
    m_chatSystem = std::make_unique<MMO::MMOChatSystem>();
    if (!m_chatSystem->Initialize(context))
    {
        console.LogError("[MMO] Failed to initialize chat system");
        return false;
    }

    // Initialize MMO gameplay systems
    m_inventorySystem = std::make_unique<MMO::MMOInventorySystem>();
    if (!m_inventorySystem->Initialize(context))
    {
        console.LogError("[MMO] Failed to initialize inventory system");
        return false;
    }

    m_craftingSystem = std::make_unique<MMO::MMOCraftingSystem>();
    if (!m_craftingSystem->Initialize(context))
    {
        console.LogError("[MMO] Failed to initialize crafting system");
        return false;
    }

    m_guildSystem = std::make_unique<MMO::MMOGuildSystem>();
    if (!m_guildSystem->Initialize(context))
    {
        console.LogError("[MMO] Failed to initialize guild system");
        return false;
    }

    m_tradingSystem = std::make_unique<MMO::MMOTradingSystem>();
    if (!m_tradingSystem->Initialize(context))
    {
        console.LogError("[MMO] Failed to initialize trading system");
        return false;
    }

    m_partySystem = std::make_unique<MMO::MMOPartySystem>();
    if (!m_partySystem->Initialize(context))
    {
        console.LogError("[MMO] Failed to initialize party system");
        return false;
    }

    m_achievementSystem = std::make_unique<MMO::MMOAchievementSystem>();
    if (!m_achievementSystem->Initialize(context))
    {
        console.LogError("[MMO] Failed to initialize achievement system");
        return false;
    }

    m_reputationSystem = std::make_unique<MMO::MMOReputationSystem>();
    if (!m_reputationSystem->Initialize(context))
    {
        console.LogError("[MMO] Failed to initialize reputation system");
        return false;
    }

    m_dungeonSystem = std::make_unique<MMO::MMODungeonSystem>();
    if (!m_dungeonSystem->Initialize(context))
    {
        console.LogError("[MMO] Failed to initialize dungeon system");
        return false;
    }

    m_worldBossSystem = std::make_unique<MMO::MMOWorldBossSystem>();
    if (!m_worldBossSystem->Initialize(context))
    {
        console.LogError("[MMO] Failed to initialize world boss system");
        return false;
    }

    m_persistenceSystem = std::make_unique<MMO::MMOPersistenceSystem>();
    if (!m_persistenceSystem->Initialize(context))
    {
        console.LogWarning("[MMO] Persistence system unavailable (non-fatal)");
        m_persistenceSystem.reset();
    }

    RegisterConsoleCommands();

    m_initialized = true;
    console.LogInfo("[MMO] Spark MMO module loaded successfully (13 subsystems)");
    console.LogInfo("[MMO] World areas: " + std::to_string(m_worldSetup->GetAreaCount()));
    console.LogInfo("[MMO] Items: " + std::to_string(m_inventorySystem->GetItemCount()) +
                    " | Recipes: " + std::to_string(m_craftingSystem->GetRecipeCount()) +
                    " | Factions: " + std::to_string(m_reputationSystem->GetFactionCount()) +
                    " | Dungeons: " + std::to_string(m_dungeonSystem->GetDungeonCount()) +
                    " | World Bosses: " + std::to_string(m_worldBossSystem->GetBossCount()) +
                    " | Achievements: " + std::to_string(m_achievementSystem->GetAchievementCount()));
    return true;
}

void SparkGameMMOModule::OnUnload()
{
    if (!m_initialized)
        return;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[MMO] Unloading Spark MMO module...");

    // Shutdown persistence first (final save)
    if (m_persistenceSystem)
    {
        m_persistenceSystem->Shutdown();
        m_persistenceSystem.reset();
    }

    // Shutdown gameplay systems in reverse order
    if (m_worldBossSystem)
    {
        m_worldBossSystem->Shutdown();
        m_worldBossSystem.reset();
    }
    if (m_dungeonSystem)
    {
        m_dungeonSystem->Shutdown();
        m_dungeonSystem.reset();
    }
    if (m_reputationSystem)
    {
        m_reputationSystem->Shutdown();
        m_reputationSystem.reset();
    }
    if (m_achievementSystem)
    {
        m_achievementSystem->Shutdown();
        m_achievementSystem.reset();
    }
    if (m_partySystem)
    {
        m_partySystem->Shutdown();
        m_partySystem.reset();
    }
    if (m_tradingSystem)
    {
        m_tradingSystem->Shutdown();
        m_tradingSystem.reset();
    }
    if (m_guildSystem)
    {
        m_guildSystem->Shutdown();
        m_guildSystem.reset();
    }
    if (m_craftingSystem)
    {
        m_craftingSystem->Shutdown();
        m_craftingSystem.reset();
    }
    if (m_inventorySystem)
    {
        m_inventorySystem->Shutdown();
        m_inventorySystem.reset();
    }

    if (m_chatSystem)
    {
        m_chatSystem->Shutdown();
        m_chatSystem.reset();
    }

    if (m_playerSystem)
    {
        m_playerSystem->Shutdown();
        m_playerSystem.reset();
    }

    if (m_worldSetup)
    {
        m_worldSetup->Shutdown();
        m_worldSetup.reset();
    }

    m_context = nullptr;
    m_initialized = false;
    console.LogInfo("[MMO] Spark MMO module unloaded");
}

void SparkGameMMOModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_worldSetup->Update(deltaTime);
    m_playerSystem->Update(deltaTime);
    m_chatSystem->Update(deltaTime);

    // Update gameplay systems
    m_tradingSystem->Update(deltaTime);
    m_partySystem->Update(deltaTime);
    m_dungeonSystem->Update(deltaTime);
    m_worldBossSystem->Update(deltaTime);

    if (m_persistenceSystem)
        m_persistenceSystem->Update(deltaTime);
}

void SparkGameMMOModule::OnFixedUpdate(float fixedDeltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_playerSystem->FixedUpdate(fixedDeltaTime);
}

void SparkGameMMOModule::OnRender()
{
    if (!m_initialized)
        return;

    m_playerSystem->Render();
}

void SparkGameMMOModule::OnResize(int width, int height)
{
    (void)width;
    (void)height;
}

void SparkGameMMOModule::OnPause()
{
    m_paused = true;
}

void SparkGameMMOModule::OnResume()
{
    m_paused = false;
}

void SparkGameMMOModule::OnImGui()
{
    if (!m_initialized)
        return;

    m_worldSetup->RenderDebugUI();
    m_playerSystem->RenderDebugUI();
    m_chatSystem->RenderDebugUI();
    m_inventorySystem->RenderDebugUI();
    m_craftingSystem->RenderDebugUI();
    m_guildSystem->RenderDebugUI();
    m_tradingSystem->RenderDebugUI();
    m_partySystem->RenderDebugUI();
    m_achievementSystem->RenderDebugUI();
    m_reputationSystem->RenderDebugUI();
    m_dungeonSystem->RenderDebugUI();
    m_worldBossSystem->RenderDebugUI();
    if (m_persistenceSystem)
        m_persistenceSystem->RenderDebugUI();
}

void SparkGameMMOModule::RegisterConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    console.RegisterCommand(
        "mmo_status",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_worldSetup)
                return "MMO module not initialized";

            std::string status = "=== Spark MMO Status ===\n";
            status += "Areas: " + std::to_string(m_worldSetup->GetAreaCount()) + "\n";
            status += "Players: " + std::to_string(m_playerSystem->GetPlayerCount()) + "\n";
            status += "Chat channels: " + std::to_string(m_chatSystem->GetChannelCount()) + "\n";
            status += "Guilds: " + std::to_string(m_guildSystem->GetGuildCount()) + "\n";
            status += "Dungeons: " + std::to_string(m_dungeonSystem->GetDungeonCount()) + "\n";
            status += "Items: " + std::to_string(m_inventorySystem->GetItemCount()) + "\n";
            status += "Achievements: " + std::to_string(m_achievementSystem->GetAchievementCount()) + "\n";
            status += "DB: " + std::string(m_persistenceSystem ? "Connected" : "Offline") + "\n";
            status += m_worldSetup->GetWorldStatusString();
            return status;
        });

    console.RegisterCommand("mmo_chat",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (args.size() < 2)
                                    return "Usage: mmo_chat <channel> <message>";

                                std::string message;
                                for (size_t i = 1; i < args.size(); ++i)
                                {
                                    if (i > 1)
                                        message += " ";
                                    message += args[i];
                                }
                                m_chatSystem->SendMessage(args[0], message);
                                return "";
                            });

    console.RegisterCommand("mmo_areas", [this](const std::vector<std::string>&) -> std::string
                            { return m_worldSetup->GetAreaListString(); });

    console.RegisterCommand("mmo_players", [this](const std::vector<std::string>&) -> std::string
                            { return m_playerSystem->GetPlayerListString(); });

    console.RegisterCommand("mmo_guilds", [this](const std::vector<std::string>&) -> std::string
                            { return m_guildSystem->GetGuildListString(); });

    console.RegisterCommand("mmo_guild_create",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (args.size() < 2)
                                    return "Usage: mmo_guild_create <name> <tag>";
                                uint32_t id = m_guildSystem->CreateGuild(args[0], args[1], 1, "Player");
                                return id ? "Guild created (ID " + std::to_string(id) + ")" : "Failed";
                            });

    console.RegisterCommand("mmo_auctions", [this](const std::vector<std::string>&) -> std::string
                            { return m_tradingSystem->GetAuctionListString(); });

    console.RegisterCommand("mmo_dungeons", [this](const std::vector<std::string>&) -> std::string
                            { return m_dungeonSystem->GetDungeonListString(); });

    console.RegisterCommand("mmo_bosses", [this](const std::vector<std::string>&) -> std::string
                            { return m_worldBossSystem->GetBossStatusString(); });

    console.RegisterCommand("mmo_boss_spawn",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (args.empty())
                                    return "Usage: mmo_boss_spawn <boss_id>";
                                uint32_t id = static_cast<uint32_t>(std::stoi(args[0]));
                                return m_worldBossSystem->SpawnBoss(id) ? "Boss spawned!" : "Spawn failed";
                            });

    console.RegisterCommand("mmo_recipes",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                std::string result = "Crafting Recipes:\n";
                                // List all recipes briefly
                                return result + "Total recipes: " + std::to_string(m_craftingSystem->GetRecipeCount());
                            });

    console.RegisterCommand("mmo_db_status",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                if (!m_persistenceSystem)
                                    return "Persistence system not available";
                                return m_persistenceSystem->GetStatusString();
                            });
}
