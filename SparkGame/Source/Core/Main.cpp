/**
 * @file Main.cpp
 * @brief SparkGame DLL - IModule + IGameModule implementation and exports
 *
 * This file implements the SparkGameModule class and exports both the new
 * (CreateModule/DestroyModule) and legacy (CreateGameModule/DestroyGameModule)
 * factory functions. The engine's ModuleManager will prefer the new exports.
 *
 * When the engine starts, it finds and loads SparkGame.dll, calls
 * CreateModule() to get a SparkGameModule instance, then drives
 * the game loop through the IModule interface.
 */

#include "SparkGame.h"
#include "Game/Game.h"
#include "Game/Console.h"
#include "Game/GameMode.h"
#include "Game/InventorySystem.h"
#include "Game/QuestSystem.h"
#include "Core/EngineContext.h"
#include "Engine/Events/EventSystem.h"
#include "Utils/SparkConsole.h"

// Global game pointer used by SparkConsole (in SparkEngineLib) to call into
// game systems.  Owned by SparkGameModule; set during Initialize, cleared
// during Shutdown.
SPARK_GAME_API std::unique_ptr<Game> g_game;

// Global in-game console overlay used by Game::Render().
Console g_console;

#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>

// DLL entry point
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

// ===================================================================================
// SparkGameModule implementation
// ===================================================================================

SparkGameModule::SparkGameModule() = default;

SparkGameModule::~SparkGameModule()
{
    if (m_initialized)
        Shutdown();
}

// --- Spark::IModule interface (new) ---

Spark::ModuleInfo SparkGameModule::GetModuleInfo() const
{
    Spark::ModuleInfo info{};
    info.name = "Spark Game";
    info.version = "1.0.0";
    info.sdkVersion = SPARK_SDK_VERSION;
    info.loadOrder = 1000;
    return info;
}

bool SparkGameModule::OnLoad(Spark::IEngineContext* context)
{
    m_context = context;

    // Delegate to the shared Initialize logic using the context's subsystems
    if (!Initialize(context->GetGraphics(), context->GetInput()))
        return false;

    if (g_game)
    {
        // Wire up EventBus so game systems can communicate via events
        if (context->GetEventBus())
            g_game->SetEventBus(context->GetEventBus());

        // Wire up physics system for projectile area queries (explosions)
        if (context->GetPhysics())
            g_game->SetPhysicsSystem(context->GetPhysics());

        // Wire up SceneManager from the engine context if the game doesn't own one
        // (Game creates its own SceneManager, but the engine context should know about it)
        if (auto* sceneMgr = g_game->GetSceneManager())
        {
            if (!context->GetSceneManager())
            {
                // Register the game's SceneManager with the engine context so
                // the editor and other modules can access it
                auto* ctx = dynamic_cast<EngineContext*>(context);
                if (ctx)
                    ctx->SetSceneManager(sceneMgr);
            }
        }
    }

    return true;
}

void SparkGameModule::OnUnload()
{
    Shutdown();
}

void SparkGameModule::OnUpdate(float deltaTime)
{
    if (g_game && !g_game->IsPaused())
        g_game->Update(deltaTime);
}

void SparkGameModule::OnRender()
{
    if (g_game)
        g_game->Render();
}

// --- IGameModule interface (legacy) ---

const char* SparkGameModule::GetGameName() const
{
    return "Spark Game";
}

const char* SparkGameModule::GetGameVersion() const
{
    return "1.0.0";
}

bool SparkGameModule::Initialize(GraphicsEngine* graphics, InputManager* input)
{
    if (m_initialized)
        return true; // Prevent double-init

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("Initializing SparkGame module...");

    g_game = std::make_unique<Game>();
    HRESULT hr = g_game->Initialize(graphics, input);
    if (FAILED(hr))
    {
        console.LogError("Game::Initialize() failed");
        g_game.reset();
        return false;
    }

    // Initialize the in-game console overlay (global used by Game::Render)
    g_console.Initialize(1280, 720);

    // Register game-specific console commands
    RegisterGameConsoleCommands();

    m_initialized = true;
    console.LogSuccess("SparkGame module initialized");
    return true;
}

void SparkGameModule::Shutdown()
{
    if (!m_initialized)
        return;

    if (g_game)
    {
        g_game->Shutdown();
        g_game.reset();
    }
    m_initialized = false;

    Spark::SimpleConsole::GetInstance().LogInfo("SparkGame module shut down");
}

void SparkGameModule::Update(float deltaTime)
{
    if (g_game)
        g_game->Update(deltaTime);
}

void SparkGameModule::Render()
{
    if (g_game)
        g_game->Render();
}

void SparkGameModule::OnResize(int width, int height)
{
    if (width > 0 && height > 0)
        g_console.Initialize(width, height);
}

void SparkGameModule::Pause()
{
    if (g_game)
        g_game->Pause();
}

void SparkGameModule::Resume()
{
    if (g_game)
        g_game->Resume();
}

bool SparkGameModule::IsPaused() const
{
    return g_game ? g_game->IsPaused() : false;
}

// ===================================================================================
// Game-specific console commands (registered when game module loads)
// ===================================================================================
void SparkGameModule::RegisterGameConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    Game* game = g_game.get();

    console.RegisterCommand(
        "game_timescale",
        [game](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: game_timescale <scale>";
            if (!game)
                return "Game not available";
            try
            {
                float scale = std::stof(args[0]);
                game->SetTimeScale(scale);
                return "Time scale set to " + std::to_string(scale);
            }
            catch (const std::exception& e)
            {
                return std::string("Error: ") + e.what();
            }
        },
        "Set game time scale");

    console.RegisterCommand(
        "player_tp",
        [game](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 3)
                return "Usage: player_tp <x> <y> <z>";
            if (!game)
                return "Game not available";
            try
            {
                float x = std::stof(args[0]);
                float y = std::stof(args[1]);
                float z = std::stof(args[2]);
                game->TeleportPlayer(x, y, z);
                return "Teleported to (" + args[0] + ", " + args[1] + ", " + args[2] + ")";
            }
            catch (const std::exception& e)
            {
                return std::string("Error: ") + e.what();
            }
        },
        "Teleport player to coordinates");

    console.RegisterCommand(
        "spawn",
        [game](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 4)
                return "Usage: spawn <type> <x> <y> <z>";
            if (!game)
                return "Game not available";
            try
            {
                float x = std::stof(args[1]);
                float y = std::stof(args[2]);
                float z = std::stof(args[3]);
                bool ok = game->SpawnObject(args[0], x, y, z);
                return ok ? "Spawned " + args[0] : "Failed to spawn '" + args[0] + "'";
            }
            catch (const std::exception& e)
            {
                return std::string("Error: ") + e.what();
            }
        },
        "Spawn an object at coordinates");

    console.RegisterCommand(
        "god",
        [game](const std::vector<std::string>& args) -> std::string
        {
            if (!game)
                return "Game not available";
            bool enable = args.empty() || (args[0] == "on" || args[0] == "true" || args[0] == "1");
            game->ApplyDebugSettings(enable, false, false);
            return enable ? "God mode enabled" : "God mode disabled";
        },
        "Toggle god mode");

    console.RegisterCommand(
        "noclip",
        [game](const std::vector<std::string>& args) -> std::string
        {
            if (!game)
                return "Game not available";
            bool enable = args.empty() || (args[0] == "on" || args[0] == "true" || args[0] == "1");
            game->ApplyDebugSettings(false, enable, false);
            return enable ? "Noclip enabled" : "Noclip disabled";
        },
        "Toggle noclip mode");

    console.RegisterCommand(
        "game_stats",
        [game](const std::vector<std::string>&) -> std::string
        {
            if (!game)
                return "Game not available";
            int drawCalls, triangles, activeObjects;
            game->GetPerformanceStats(drawCalls, triangles, activeObjects);
            std::stringstream ss;
            ss << "=== Game Stats ===\n";
            ss << "Draw Calls: " << drawCalls << "\n";
            ss << "Triangles: " << triangles << "\n";
            ss << "Active Objects: " << activeObjects << "\n";
            ss << "Time Scale: " << game->GetTimeScale() << "\n";
            return ss.str();
        },
        "Display game performance statistics");

    // -------------------------------------------------------------------------
    // GameMode commands
    // -------------------------------------------------------------------------

    console.RegisterCommand(
        "gamemode_info",
        [game](const std::vector<std::string>&) -> std::string
        {
            if (!game)
                return "Game not available";
            auto* gm = game->GetGameMode();
            if (!gm)
                return "GameMode not initialized";
            auto& rules = gm->GetRules();
            std::stringstream ss;
            ss << "=== Game Mode ===\n";
            ss << "Mode: " << rules.modeName << "\n";
            ss << "Score Limit: " << rules.scoreLimit << "\n";
            ss << "Time Limit: " << rules.timeLimit << "s\n";
            ss << "Round: " << gm->GetCurrentRound() << "/" << rules.roundLimit << "\n";
            ss << "Active: " << (gm->IsMatchActive() ? "Yes" : "No") << "\n";
            auto scoreboard = gm->GetScoreboard();
            for (const auto& ps : scoreboard)
            {
                ss << "  " << ps.playerName << ": K=" << ps.kills << " D=" << ps.deaths << " Score=" << ps.totalScore
                   << "\n";
            }
            return ss.str();
        },
        "Show current game mode info and scoreboard");

    console.RegisterCommand(
        "gamemode",
        [game](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: gamemode <freeplay|deathmatch|tdm|ctf|domination|elimination>";
            if (!game)
                return "Game not available";
            auto* gm = game->GetGameMode();
            if (!gm)
                return "GameMode not initialized";

            Spark::GameModeType type = Spark::GameModeType::FreePlay;
            if (args[0] == "deathmatch" || args[0] == "dm")
                type = Spark::GameModeType::Deathmatch;
            else if (args[0] == "tdm")
                type = Spark::GameModeType::TeamDeathmatch;
            else if (args[0] == "ctf")
                type = Spark::GameModeType::CaptureTheFlag;
            else if (args[0] == "domination")
                type = Spark::GameModeType::Domination;
            else if (args[0] == "elimination")
                type = Spark::GameModeType::Elimination;
            else if (args[0] == "gungame")
                type = Spark::GameModeType::GunGame;
            else if (args[0] == "koth")
                type = Spark::GameModeType::KingOfTheHill;
            else if (args[0] == "survival")
                type = Spark::GameModeType::Survival;
            else if (args[0] == "freeplay")
                type = Spark::GameModeType::FreePlay;

            gm->EndMatch();
            auto rules = Spark::GameMode::GetPreset(type);
            gm->Initialize(rules);
            gm->AddPlayer("Player1");
            gm->StartMatch();
            return "Switched to " + rules.modeName;
        },
        "Switch game mode");

    // -------------------------------------------------------------------------
    // Inventory commands
    // -------------------------------------------------------------------------

    console.RegisterCommand(
        "inventory",
        [game](const std::vector<std::string>&) -> std::string
        {
            if (!game)
                return "Game not available";
            auto& inv = game->GetPlayerInventory();
            auto& reg = game->GetItemRegistry();
            if (inv.slots.empty())
                return "Inventory is empty";
            std::stringstream ss;
            ss << "=== Inventory (" << inv.slots.size() << "/" << inv.maxSlots << " slots) ===\n";
            float totalWeight = Spark::InventoryOps::GetTotalWeight(inv, reg);
            ss << "Weight: " << totalWeight << "/" << inv.maxWeight << " kg\n";
            ss << "Currency: " << inv.currency << "\n";
            for (size_t i = 0; i < inv.slots.size(); ++i)
            {
                auto& slot = inv.slots[i];
                if (auto* def = reg.GetItem(slot.itemDefId))
                {
                    ss << "  [" << i << "] " << def->name << " x" << slot.count << "\n";
                }
            }
            return ss.str();
        },
        "Show player inventory");

    console.RegisterCommand(
        "give",
        [game](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: give <item_id> [count]";
            if (!game)
                return "Game not available";
            try
            {
                uint32_t id = static_cast<uint32_t>(std::stoul(args[0]));
                int count = args.size() > 1 ? std::stoi(args[1]) : 1;
                int added =
                    Spark::InventoryOps::AddItem(game->GetPlayerInventory(), game->GetItemRegistry(), id, count);
                auto* def = game->GetItemRegistry().GetItem(id);
                std::string name = def ? def->name : "Item #" + args[0];
                return "Added " + std::to_string(added) + "x " + name;
            }
            catch (const std::exception& e)
            {
                return std::string("Error: ") + e.what();
            }
        },
        "Give item to player (give <id> [count])");

    // -------------------------------------------------------------------------
    // Quest commands
    // -------------------------------------------------------------------------

    console.RegisterCommand(
        "quest_list",
        [game](const std::vector<std::string>&) -> std::string
        {
            if (!game)
                return "Game not available";
            auto& journal = game->GetPlayerQuests();
            auto& reg = game->GetQuestRegistry();
            std::stringstream ss;
            ss << "=== Active Quests ===\n";
            for (const auto& aq : journal.activeQuests)
            {
                if (aq.status != Spark::QuestStatus::Active)
                    continue;
                auto* def = reg.GetQuest(aq.questId);
                if (!def)
                    continue;
                ss << "[" << aq.questId << "] " << def->name << "\n";
                ss << "  " << def->description << "\n";
                for (size_t i = 0; i < def->objectives.size(); ++i)
                {
                    auto& obj = def->objectives[i];
                    auto& prog = aq.objectiveProgress[i];
                    ss << "  " << (prog.completed ? "[X] " : "[ ] ") << obj.description << " (" << prog.currentCount
                       << "/" << obj.requiredCount << ")\n";
                }
            }
            ss << "\nCompleted: " << journal.completedQuestIds.size() << " quests\n";
            return ss.str();
        },
        "Show active quests and progress");

    console.RegisterCommand(
        "quest_start",
        [game](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: quest_start <quest_id>";
            if (!game)
                return "Game not available";
            try
            {
                uint32_t id = static_cast<uint32_t>(std::stoul(args[0]));
                bool ok = Spark::QuestOps::StartQuest(game->GetPlayerQuests(), game->GetQuestRegistry(), id);
                if (ok)
                {
                    auto* def = game->GetQuestRegistry().GetQuest(id);
                    return "Started quest: " + (def ? def->name : "Quest #" + args[0]);
                }
                return "Failed to start quest (not found, already active, or prerequisites not met)";
            }
            catch (const std::exception& e)
            {
                return std::string("Error: ") + e.what();
            }
        },
        "Start a quest by ID");

    console.RegisterCommand(
        "quest_all",
        [game](const std::vector<std::string>&) -> std::string
        {
            if (!game)
                return "Game not available";
            auto& reg = game->GetQuestRegistry();
            std::stringstream ss;
            ss << "=== All Quests ===\n";
            for (auto& [id, def] : reg.GetAllQuests())
            {
                ss << "[" << id << "] " << def.name << " - " << def.description << "\n";
            }
            return ss.str();
        },
        "List all available quests");

    // -------------------------------------------------------------------------
    // HUD commands
    // -------------------------------------------------------------------------

    console.RegisterCommand(
        "hud",
        [game](const std::vector<std::string>& args) -> std::string
        {
            if (!game)
                return "Game not available";
            auto* hud = game->GetHUDSystem();
            if (!hud)
                return "HUD not initialized";
            if (args.empty())
            {
                auto& cfg = hud->GetConfig();
                std::stringstream ss;
                ss << "=== HUD Config ===\n";
                ss << "Health Bar: " << (cfg.showHealthBar ? "On" : "Off") << "\n";
                ss << "Ammo Counter: " << (cfg.showAmmoCounter ? "On" : "Off") << "\n";
                ss << "Minimap: " << (cfg.showMinimap ? "On" : "Off") << "\n";
                ss << "Kill Feed: " << (cfg.showKillFeed ? "On" : "Off") << "\n";
                return ss.str();
            }
            bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
            auto& cfg = hud->GetConfig();
            cfg.showHealthBar = enable;
            cfg.showAmmoCounter = enable;
            cfg.showMinimap = enable;
            cfg.showKillFeed = enable;
            return enable ? "HUD elements enabled" : "HUD elements disabled";
        },
        "Toggle HUD visibility (hud [on|off])");
}

// ===================================================================================
// DLL Exports - New API (preferred by ModuleManager)
// ===================================================================================

extern "C"
{

    SPARK_MODULE_API Spark::IModule* CreateModule()
    {
        return new SparkGameModule();
    }

    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod)
    {
        delete mod;
    }

} // extern "C"

// ===================================================================================
// DLL Exports - Legacy API (backward compatibility)
// ===================================================================================

extern "C"
{

    SPARK_GAME_API IGameModule* CreateGameModule()
    {
        return new SparkGameModule();
    }

    SPARK_GAME_API void DestroyGameModule(IGameModule* module)
    {
        delete module;
    }

} // extern "C"
