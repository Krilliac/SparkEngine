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
#include "Account/MMOAccountSystem.h"
#include "Character/MMOCharacterSystem.h"
#include "UI/MMOLoginUI.h"
#include "Gameplay/MMOGameplaySession.h"
#include "MMOEngineSystems.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "Utils/InvalidStateDetector.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"
#include "Engine/ECS/Components/NetworkComponents.h"

#include <Spark/ModuleDllMain.h>

#include <charconv>
#include <cmath>

namespace
{
    bool ParseUint(const std::string& text, uint32_t& value)
    {
        if (text.empty())
            return false;
        const char* begin = text.data();
        const char* end = begin + text.size();
        const auto [ptr, error] = std::from_chars(begin, end, value);
        return error == std::errc{} && ptr == end;
    }

    bool ParsePositiveInt(const std::string& text, int& value)
    {
        if (text.empty())
            return false;
        const char* begin = text.data();
        const char* end = begin + text.size();
        const auto [ptr, error] = std::from_chars(begin, end, value);
        return error == std::errc{} && ptr == end && value > 0;
    }

    bool ParsePositiveFloat(const std::string& text, float& value)
    {
        try
        {
            size_t parsed = 0;
            value = std::stof(text, &parsed);
            return parsed == text.size() && std::isfinite(value) && value > 0.0f;
        }
        catch (...)
        {
            return false;
        }
    }
} // namespace

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

    // A reused module instance must not retain handlers or partially initialized
    // systems from an earlier failed/hot-reload attempt.
    OnUnload();

    m_context = context;

    SPARK_LOG_INFO(Spark::LogCategory::Game, "Loading SparkGameMMO module");

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[MMO] Loading Spark MMO module...");

    const auto failLoad = [this, &console](const std::string& message)
    {
        console.LogError(message);
        ShutdownSystems();
        m_context = nullptr;
        return false;
    };

    // Initialize the world area setup (registers areas with streaming/area servers)
    m_worldSetup = std::make_unique<MMO::MMOWorldSetup>();
    if (!m_worldSetup->Initialize(context))
    {
        return failLoad("[MMO] Failed to initialize world setup");
    }

    // Initialize player system (spawning, replication, prediction)
    m_playerSystem = std::make_unique<MMO::MMOPlayerSystem>();
    if (!m_playerSystem->Initialize(context))
    {
        return failLoad("[MMO] Failed to initialize player system");
    }
    m_playerSystem->SetAreaResolver(
        [this](float x, float y, float z, uint32_t currentAreaId)
        { return m_worldSetup ? m_worldSetup->FindAreaId(x, y, z, currentAreaId) : currentAreaId; });

    // Initialize chat system (area, global, party, whisper channels)
    m_chatSystem = std::make_unique<MMO::MMOChatSystem>();
    if (!m_chatSystem->Initialize(context))
    {
        return failLoad("[MMO] Failed to initialize chat system");
    }

    // Initialize MMO gameplay systems
    m_inventorySystem = std::make_unique<MMO::MMOInventorySystem>();
    if (!m_inventorySystem->Initialize(context))
    {
        return failLoad("[MMO] Failed to initialize inventory system");
    }

    m_craftingSystem = std::make_unique<MMO::MMOCraftingSystem>();
    if (!m_craftingSystem->Initialize(context, m_inventorySystem.get()))
    {
        return failLoad("[MMO] Failed to initialize crafting system");
    }

    m_guildSystem = std::make_unique<MMO::MMOGuildSystem>();
    if (!m_guildSystem->Initialize(context))
    {
        return failLoad("[MMO] Failed to initialize guild system");
    }

    m_tradingSystem = std::make_unique<MMO::MMOTradingSystem>();
    if (!m_tradingSystem->Initialize(context))
    {
        return failLoad("[MMO] Failed to initialize trading system");
    }

    m_partySystem = std::make_unique<MMO::MMOPartySystem>();
    if (!m_partySystem->Initialize(context))
    {
        return failLoad("[MMO] Failed to initialize party system");
    }

    m_achievementSystem = std::make_unique<MMO::MMOAchievementSystem>();
    if (!m_achievementSystem->Initialize(context))
    {
        return failLoad("[MMO] Failed to initialize achievement system");
    }

    m_reputationSystem = std::make_unique<MMO::MMOReputationSystem>();
    if (!m_reputationSystem->Initialize(context))
    {
        return failLoad("[MMO] Failed to initialize reputation system");
    }

    m_dungeonSystem = std::make_unique<MMO::MMODungeonSystem>();
    if (!m_dungeonSystem->Initialize(context))
    {
        return failLoad("[MMO] Failed to initialize dungeon system");
    }

    m_worldBossSystem = std::make_unique<MMO::MMOWorldBossSystem>();
    if (!m_worldBossSystem->Initialize(context))
    {
        return failLoad("[MMO] Failed to initialize world boss system");
    }

    m_gameplaySession = std::make_unique<MMO::MMOGameplaySession>();
    if (!m_gameplaySession->Initialize(m_playerSystem.get(), m_worldSetup.get(), m_inventorySystem.get(),
                                       m_craftingSystem.get(), m_achievementSystem.get(), m_reputationSystem.get(),
                                       m_dungeonSystem.get(), m_worldBossSystem.get()))
    {
        return failLoad("[MMO] Failed to initialize playable gameplay session");
    }

    m_persistenceSystem = std::make_unique<MMO::MMOPersistenceSystem>();
    if (!m_persistenceSystem->Initialize(context))
    {
        console.LogWarning("[MMO] Persistence system unavailable (non-fatal)");
        m_persistenceSystem.reset();
    }

    m_accountSystem = std::make_unique<MMO::MMOAccountSystem>();
    if (!m_accountSystem->Initialize(context))
    {
        return failLoad("[MMO] Failed to initialize account system");
    }

    m_characterSystem = std::make_unique<MMO::MMOCharacterSystem>();
    if (!m_characterSystem->Initialize(context))
    {
        return failLoad("[MMO] Failed to initialize character system");
    }

    m_loginUI = std::make_unique<MMO::MMOLoginUI>();
    if (!m_loginUI->Initialize(context, m_accountSystem.get(), m_characterSystem.get()))
    {
        return failLoad("[MMO] Failed to initialize login UI");
    }
    m_loginUI->SetEnterWorldCallback(
        [this](uint32_t accountId, uint32_t characterId)
        {
            if (!m_characterSystem || !m_gameplaySession)
                return;

            MMO::CharacterSaveData saved;
            if (m_persistenceSystem && m_persistenceSystem->LoadCharacter(characterId, saved) && !saved.name.empty())
            {
                // Legacy records did not persist accountId; the authenticated
                // login flow is authoritative for ownership during migration.
                saved.accountId = accountId;
                m_gameplaySession->LoadSaveData(saved);
                return;
            }

            const auto* character = m_characterSystem->GetCharacter(characterId);
            if (!character)
                return;
            const auto* race = m_characterSystem->GetRace(character->race);
            const auto stats = m_characterSystem->ComputeStats(character->race, character->classId, character->level);
            const float x = race ? race->spawnX : 0.0f;
            const float y = race ? race->spawnY : 1.0f;
            const float z = race ? race->spawnZ : 0.0f;
            m_gameplaySession->ActivateCharacter(accountId, characterId, character->name, character->level,
                                                 stats.health, character->areaId, x, y, z);
        });

    // Wire engine subsystems (weather, abilities, dialogue, cinematic, AI, animation, events, localization)
    m_engineSystems = std::make_unique<MMO::MMOEngineSystems>();
    if (!m_engineSystems->Initialize(context))
    {
        console.LogWarning("[MMO] Engine subsystem wiring partially failed (non-fatal)");
    }

    RegisterConsoleCommands();

#ifdef ENABLE_NETWORKING
    // In headless/dedicated mode, start the network server automatically
    if (context->IsHeadless())
    {
        constexpr uint16_t MMO_SERVER_PORT = 27015;
        if (m_worldSetup->StartNetworkServer(MMO_SERVER_PORT))
        {
            console.LogInfo("[MMO] Dedicated server listening on port " + std::to_string(MMO_SERVER_PORT));
        }
        else
        {
            console.LogError("[MMO] Failed to start network server on port " + std::to_string(MMO_SERVER_PORT));
        }
    }
#endif

    RegisterStateValidationRules();

    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "SparkGameMMO module loaded: playable session ready");
    console.LogInfo("[MMO] Spark MMO module loaded successfully; playable session ready");
    console.LogInfo("[MMO] Controls: WASD move, Shift sprint; run mmo_help for gameplay commands");
    console.LogInfo("[MMO] World areas: " + std::to_string(m_worldSetup->GetAreaCount()));
    console.LogInfo("[MMO] Items: " + std::to_string(m_inventorySystem->GetItemCount()) +
                    " | Recipes: " + std::to_string(m_craftingSystem->GetRecipeCount()) +
                    " | Factions: " + std::to_string(m_reputationSystem->GetFactionCount()) +
                    " | Dungeons: " + std::to_string(m_dungeonSystem->GetDungeonCount()) +
                    " | World Bosses: " + std::to_string(m_worldBossSystem->GetBossCount()) +
                    " | Achievements: " + std::to_string(m_achievementSystem->GetAchievementCount()));
    return true;
}

void SparkGameMMOModule::RegisterStateValidationRules()
{
    auto& stateDetector = Spark::InvalidStateDetector::GetInstance();
    stateDetector.RemoveRulesByCategory("MMO");
    stateDetector.AddRule({"MMO.DeadWithNetwork", "MMO", Spark::StateViolationSeverity::Error, true,
                           [](World& w, std::vector<Spark::StateViolation>& out)
                           {
                               for (auto entity : w.GetEntitiesWith<HealthComponent, NetworkIdentity>())
                               {
                                   auto* h = w.GetComponent<HealthComponent>(entity);
                                   auto* ni = w.GetComponent<NetworkIdentity>(entity);
                                   if (h && ni && h->isDead && !h->deathProcessed && ni->isLocalAuthority)
                                   {
                                       out.push_back({"MMO.DeadWithNetwork", static_cast<uint32_t>(entity),
                                                      "Local-authority entity dead but deathProcessed=false",
                                                      Spark::StateViolationSeverity::Error});
                                   }
                               }
                           }});

    stateDetector.AddRule({"MMO.HealthOverMax", "MMO", Spark::StateViolationSeverity::Warning, true,
                           [](World& w, std::vector<Spark::StateViolation>& out)
                           {
                               for (auto entity : w.GetEntitiesWith<HealthComponent>())
                               {
                                   auto* h = w.GetComponent<HealthComponent>(entity);
                                   if (h && !h->isDead && h->health > h->maxHealth * 1.01f)
                                   {
                                       out.push_back({"MMO.HealthOverMax", static_cast<uint32_t>(entity),
                                                      "health=" + std::to_string(h->health) +
                                                          " exceeds maxHealth=" + std::to_string(h->maxHealth),
                                                      Spark::StateViolationSeverity::Warning});
                                   }
                               }
                           }});
    m_stateRulesRegistered = true;
}

void SparkGameMMOModule::OnUnload()
{
    if (!m_context && !m_initialized && !m_consoleCommandsRegistered && !m_stateRulesRegistered)
        return;

    SPARK_LOG_INFO(Spark::LogCategory::Game, "Unloading SparkGameMMO module");
    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[MMO] Unloading Spark MMO module...");

    if (m_gameplaySession && m_persistenceSystem)
    {
        const auto save = m_gameplaySession->BuildSaveData();
        if (save.accountId != 0 && save.characterId != 0)
            m_persistenceSystem->SaveCharacterSync(save);
    }

    UnregisterConsoleCommands();
    Spark::InvalidStateDetector::GetInstance().RemoveRulesByCategory("MMO");
    m_stateRulesRegistered = false;
    ShutdownSystems();

    m_context = nullptr;
    m_initialized = false;
    m_paused = false;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "SparkGameMMO module unloaded");
    console.LogInfo("[MMO] Spark MMO module unloaded");
}

void SparkGameMMOModule::ShutdownSystems()
{
    if (m_engineSystems)
    {
        m_engineSystems->Shutdown();
        m_engineSystems.reset();
    }

    if (m_loginUI)
    {
        m_loginUI->Shutdown();
        m_loginUI.reset();
    }

    if (m_gameplaySession)
    {
        m_gameplaySession->Shutdown();
        m_gameplaySession.reset();
    }

    if (m_characterSystem)
    {
        m_characterSystem->Shutdown();
        m_characterSystem.reset();
    }
    if (m_accountSystem)
    {
        m_accountSystem->Shutdown();
        m_accountSystem.reset();
    }

    if (m_persistenceSystem)
    {
        m_persistenceSystem->Shutdown();
        m_persistenceSystem.reset();
    }

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
#ifdef ENABLE_NETWORKING
        m_worldSetup->StopNetworkServer();
#endif
        m_worldSetup->Shutdown();
        m_worldSetup.reset();
    }
}

void SparkGameMMOModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    if (m_engineSystems)
        m_engineSystems->Update(deltaTime);

#ifdef ENABLE_NETWORKING
    // Drive the network server tick (processes socket I/O, bridges to WorldServer)
    m_worldSetup->ServerTick(deltaTime);
#endif

    m_worldSetup->Update(deltaTime);
    m_playerSystem->Update(deltaTime);
    m_chatSystem->Update(deltaTime);

    m_loginUI->Update(deltaTime);
    m_gameplaySession->Update(deltaTime);

    // Update gameplay systems
    m_tradingSystem->Update(deltaTime);
    m_partySystem->Update(deltaTime);
    m_dungeonSystem->Update(deltaTime);
    m_worldBossSystem->Update(deltaTime);

    m_accountSystem->Update(deltaTime);

    if (m_persistenceSystem)
    {
        m_persistenceSystem->Update(deltaTime);
        if (m_persistenceSystem->IsAutoSaveDue())
        {
            const auto save = m_gameplaySession->BuildSaveData();
            if (save.accountId != 0 && save.characterId != 0)
                m_persistenceSystem->SaveCharacterAsync(save);
            m_persistenceSystem->ResetAutoSaveTimer();
        }
    }
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
    m_gameplaySession->RenderDebugUI();
    if (m_persistenceSystem)
        m_persistenceSystem->RenderDebugUI();
    m_accountSystem->RenderDebugUI();
    m_characterSystem->RenderDebugUI();
    m_loginUI->RenderUI();
}

void SparkGameMMOModule::RegisterConsoleCommands()
{
    if (m_consoleCommandsRegistered)
        return;

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
            status += m_worldSetup->GetWorldStatusString() + "\n";
            if (m_gameplaySession)
                status += m_gameplaySession->GetStatusString();
            return status;
        });

    console.RegisterCommand("mmo_play",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                if (m_loginUI)
                                    m_loginUI->SetState(MMO::LoginUIState::InGame);
                                return m_gameplaySession ? m_gameplaySession->GetStatusString()
                                                         : "MMO gameplay session is unavailable";
                            });

    console.RegisterCommand("mmo_restart",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                if (m_loginUI)
                                    m_loginUI->SetState(MMO::LoginUIState::InGame);
                                return m_gameplaySession ? m_gameplaySession->ResetDemo()
                                                         : "MMO gameplay session is unavailable";
                            });

    console.RegisterCommand("mmo_travel",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                uint32_t areaId = 0;
                                if (args.empty() || !ParseUint(args[0], areaId))
                                    return "Usage: mmo_travel <area-id>";
                                return m_gameplaySession->Travel(areaId);
                            });

    console.RegisterCommand("mmo_gather",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                uint32_t itemId = 0;
                                int count = 1;
                                if (args.empty() || !ParseUint(args[0], itemId) ||
                                    (args.size() > 1 && !ParsePositiveInt(args[1], count)))
                                {
                                    return "Usage: mmo_gather <material-item-id> [count]";
                                }
                                return m_gameplaySession->Gather(itemId, count);
                            });

    console.RegisterCommand("mmo_craft",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                uint32_t recipeId = 0;
                                if (args.empty() || !ParseUint(args[0], recipeId))
                                    return "Usage: mmo_craft <recipe-id>";
                                return m_gameplaySession->Craft(recipeId);
                            });

    console.RegisterCommand("mmo_use",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                uint32_t itemId = 0;
                                if (args.empty() || !ParseUint(args[0], itemId))
                                    return "Usage: mmo_use <item-id>";
                                return m_gameplaySession->UseItem(itemId);
                            });

    console.RegisterCommand("mmo_boss_attack",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                uint32_t bossId = 0;
                                if (args.empty() || !ParseUint(args[0], bossId))
                                    return "Usage: mmo_boss_attack <boss-id>";
                                return m_gameplaySession->AttackWorldBoss(bossId);
                            });

    console.RegisterCommand("mmo_dungeon_enter",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                uint32_t dungeonId = 0;
                                if (args.empty() || !ParseUint(args[0], dungeonId))
                                    return "Usage: mmo_dungeon_enter <dungeon-id>";
                                return m_gameplaySession->EnterDungeon(dungeonId);
                            });

    console.RegisterCommand("mmo_dungeon_boss", [this](const std::vector<std::string>&) -> std::string
                            { return m_gameplaySession->DefeatNextDungeonBoss(); });

    console.RegisterCommand("mmo_damage",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                float amount = 0.0f;
                                if (args.empty() || !ParsePositiveFloat(args[0], amount))
                                    return "Usage: mmo_damage <positive-amount>";
                                return m_gameplaySession->TakeDamage(amount);
                            });

    console.RegisterCommand("mmo_respawn", [this](const std::vector<std::string>&) -> std::string
                            { return m_gameplaySession->Respawn(); });

    console.RegisterCommand("mmo_inventory",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                return m_gameplaySession
                                           ? m_inventorySystem->GetInventoryString(m_gameplaySession->GetInventory())
                                           : "MMO gameplay session is unavailable";
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
                                uint32_t id = 0;
                                if (args.empty() || !ParseUint(args[0], id))
                                    return "Usage: mmo_boss_spawn <boss_id>";
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

    console.RegisterSensitiveCommand("mmo_register",
                                     [this](const std::vector<std::string>& args) -> std::string
                                     {
                                         if (args.size() < 2)
                                             return "Usage: mmo_register <username> <password>";
                                         auto result = m_accountSystem->Register(args[0], args[1]);
                                         return result.success
                                                    ? "Account created (ID " + std::to_string(result.accountId) + ")"
                                                    : "Error: " + result.errorMessage;
                                     });

    console.RegisterSensitiveCommand("mmo_login",
                                     [this](const std::vector<std::string>& args) -> std::string
                                     {
                                         if (args.size() < 2)
                                             return "Usage: mmo_login <username> <password>";
                                         auto result = m_accountSystem->Login(args[0], args[1]);
                                         return result.success ? "Logged in" : "Error: " + result.errorMessage;
                                     });

    console.RegisterCommand("mmo_online", [this](const std::vector<std::string>&) -> std::string
                            { return m_accountSystem->GetOnlineListString(); });

    console.RegisterCommand(
        "mmo_abilities", [this](const std::vector<std::string>&) -> std::string
        { return m_engineSystems ? m_engineSystems->GetAbilitySummary() : "Engine systems not loaded"; });

    console.RegisterCommand(
        "mmo_weather", [this](const std::vector<std::string>&) -> std::string
        { return m_engineSystems ? m_engineSystems->GetWeatherStatus() : "Engine systems not loaded"; });

    console.RegisterCommand(
        "mmo_cinematic", [this](const std::vector<std::string>&) -> std::string
        { return m_engineSystems ? m_engineSystems->GetCinematicList() : "Engine systems not loaded"; });

    console.RegisterCommand(
        "mmo_locale", [this](const std::vector<std::string>&) -> std::string
        { return m_engineSystems ? m_engineSystems->GetLocaleStatus() : "Engine systems not loaded"; });

    console.RegisterCommand("mmo_characters",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                uint32_t acctId = 0;
                                if (args.empty() || !ParseUint(args[0], acctId))
                                    return "Usage: mmo_characters <account_id>";
                                return m_characterSystem->GetCharacterListString(acctId);
                            });

    console.RegisterCommand("mmo_help",
                            [](const std::vector<std::string>&) -> std::string
                            {
                                return "MMO playable commands:\n"
                                       "  mmo_play | mmo_restart | mmo_status\n"
                                       "  mmo_travel <area-id> | mmo_areas | mmo_players\n"
                                       "  mmo_gather <material-id> [count] | mmo_craft <recipe-id>\n"
                                       "  mmo_use <item-id> | mmo_inventory | mmo_recipes\n"
                                       "  mmo_boss_attack <boss-id> | mmo_bosses\n"
                                       "  mmo_dungeon_enter <dungeon-id> | mmo_dungeon_boss | mmo_dungeons\n"
                                       "  mmo_damage <amount> | mmo_respawn\n"
                                       "  mmo_chat <channel> <message> | mmo_guilds | mmo_auctions\n"
                                       "  mmo_register <user> <password> | mmo_login <user> <password>\n"
                                       "  mmo_online | mmo_characters <account-id> | mmo_db_status";
                            });

    m_consoleCommandsRegistered = true;
}

void SparkGameMMOModule::UnregisterConsoleCommands()
{
    if (!m_consoleCommandsRegistered)
        return;

    auto& console = Spark::SimpleConsole::GetInstance();
    constexpr const char* commandNames[]{
        "mmo_status",    "mmo_play",        "mmo_restart",       "mmo_travel",       "mmo_gather",  "mmo_craft",
        "mmo_use",       "mmo_boss_attack", "mmo_dungeon_enter", "mmo_dungeon_boss", "mmo_damage",  "mmo_respawn",
        "mmo_inventory", "mmo_chat",        "mmo_areas",         "mmo_players",      "mmo_guilds",  "mmo_guild_create",
        "mmo_auctions",  "mmo_dungeons",    "mmo_bosses",        "mmo_boss_spawn",   "mmo_recipes", "mmo_db_status",
        "mmo_register",  "mmo_login",       "mmo_online",        "mmo_abilities",    "mmo_weather", "mmo_cinematic",
        "mmo_locale",    "mmo_characters",  "mmo_help",
    };
    for (const char* commandName : commandNames)
        console.UnregisterCommand(commandName);
    m_consoleCommandsRegistered = false;
}
