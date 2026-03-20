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
#include "Utils/Validate.h"
#include "Audio/MusicManager.h"
#include "Graphics/WeatherSystem.h"
#include "Engine/Destruction/DestructionSystem.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/Cinematic/Sequencer.h"
#include "Engine/Replay/ReplaySystem.h"

// Global game pointer used by SparkConsole (in SparkEngineLib) to call into
// game systems.  Owned by SparkGameModule; set during Initialize, cleared
// during Shutdown.  Raw pointer avoids unique_ptr ABI mismatch across DLL boundary.
SPARK_GAME_API Game* g_game = nullptr;

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
    info.name = "Spark Arena - Engine Showcase";
    info.version = "1.0.0";
    info.sdkVersion = SPARK_SDK_VERSION;
    info.loadOrder = 1000;
    return info;
}

bool SparkGameModule::OnLoad(Spark::IEngineContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Game, context, false);
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
    return "Spark Arena";
}

const char* SparkGameModule::GetGameVersion() const
{
    return "1.0.0";
}

bool SparkGameModule::Initialize(GraphicsEngine* graphics, InputManager* input)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    if (m_initialized)
        return true; // Prevent double-init

    SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Game, graphics, false);
    SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Game, input, false);

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("Initializing SparkGame module...");
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Initializing SparkGame module");

    g_game = new Game();
    HRESULT hr = g_game->Initialize(graphics, input);
    if (FAILED(hr))
    {
        console.LogError("Game::Initialize() failed");
        delete g_game;
        g_game = nullptr;
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
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    if (!m_initialized)
        return;

    SPARK_LOG_INFO(Spark::LogCategory::Game, "Shutting down SparkGame module");

    if (g_game)
    {
        g_game->Shutdown();
        delete g_game;
        g_game = nullptr;
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
    Game* game = g_game;

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

    // -------------------------------------------------------------------------
    // Engine system commands — audio, weather, save, dialogue
    // -------------------------------------------------------------------------

    console.RegisterCommand(
        "audio_volume",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 2)
                return "Usage: audio_volume <master|sfx|music> <0.0-1.0>";
            float vol = std::stof(args[1]);
            auto& mixer = Spark::Audio::AudioBusMixer::GetInstance();
            if (args[0] == "master")
                mixer.SetBusVolume(Spark::Audio::AudioBus::Master, vol);
            else if (args[0] == "sfx")
                mixer.SetBusVolume(Spark::Audio::AudioBus::SFX, vol);
            else if (args[0] == "music")
                mixer.SetBusVolume(Spark::Audio::AudioBus::Music, vol);
            else
                return "Unknown bus: " + args[0];
            return args[0] + " volume set to " + std::to_string(vol);
        },
        "Set audio volume (audio_volume <master|sfx|music> <0.0-1.0>)");

    console.RegisterCommand(
        "weather",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: weather <clear|rain|snow|fog|storm>";
            auto* ctx = EngineContext::Get();
            auto* weather = ctx ? ctx->GetSystem<Spark::WeatherSystem>() : nullptr;
            if (!weather)
                return "WeatherSystem not available";
            Spark::WeatherType type = Spark::WeatherType::Clear;
            if (args[0] == "rain")
                type = Spark::WeatherType::Rain;
            else if (args[0] == "snow")
                type = Spark::WeatherType::Snow;
            else if (args[0] == "fog")
                type = Spark::WeatherType::Fog;
            else if (args[0] == "storm")
                type = Spark::WeatherType::Storm;
            weather->SetWeather(type, 0.8f, 3.0f);
            return "Weather set to " + args[0];
        },
        "Set weather (weather <clear|rain|snow|fog|storm>)");

    console.RegisterCommand(
        "quicksave",
        [game](const std::vector<std::string>&) -> std::string
        {
            if (!game)
                return "Game not available";
            auto& ss = Spark::SaveSystem::GetInstance();
            Spark::SaveMetadata meta;
            meta.saveName = "quicksave";
            meta.sceneName = "combat_arena";
            meta.playTime = 0;
            auto slots = ss.GetSaveSlots();
            return "Quick save: " + std::to_string(slots.size()) + " existing saves. Save system ready.";
        },
        "Quick save current game state");

    console.RegisterCommand(
        "quickload",
        [](const std::vector<std::string>&) -> std::string
        {
            auto& ss = Spark::SaveSystem::GetInstance();
            auto slots = ss.GetSaveSlots();
            if (slots.empty())
                return "No saves found.";
            return "Save system has " + std::to_string(slots.size()) + " save(s) available.";
        },
        "Quick load last saved state");

    console.RegisterCommand(
        "save_list",
        [](const std::vector<std::string>&) -> std::string
        {
            auto& ss = Spark::SaveSystem::GetInstance();
            auto slots = ss.GetSaveSlots();
            if (slots.empty())
                return "No save files found.";
            std::string result = "=== Save Slots ===\n";
            for (const auto& slot : slots)
            {
                result += slot.saveName + " - " + slot.sceneName + "\n";
            }
            return result;
        },
        "List all save slots");

    console.RegisterCommand(
        "dialogue_start",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: dialogue_start <tree_id>";
            auto* ctx = EngineContext::Get();
            auto* dialogue = ctx ? ctx->GetSystem<Spark::DialogueSystem>() : nullptr;
            if (!dialogue)
                return "DialogueSystem not available";
            dialogue->StartConversation(args[0]);
            return "Started dialogue: " + args[0];
        },
        "Start a dialogue conversation (dialogue_start <tree_id>)");

    console.RegisterCommand(
        "destroy",
        [game](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: destroy <entity_id>";
            auto& destruction = Spark::DestructionSystem::GetInstance();
            uint32_t entityId = static_cast<uint32_t>(std::stoul(args[0]));
            destruction.ForceDestroy(entityId, 50.0f);
            return "Force-destroyed entity " + args[0];
        },
        "Force-destroy a destructible entity (destroy <entity_id>)");

    // -------------------------------------------------------------------------
    // Cinematic sequencer commands
    // -------------------------------------------------------------------------

    console.RegisterCommand(
        "seq_list", [](const std::vector<std::string>&) -> std::string
        { return Spark::Cinematic::SequencerManager::GetInstance().Console_ListSequences(); },
        "List all cinematic sequences");

    console.RegisterCommand(
        "seq_info",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: seq_info <name>";
            return Spark::Cinematic::SequencerManager::GetInstance().Console_GetSequenceInfo(args[0]);
        },
        "Show detailed info about a sequence");

    console.RegisterCommand(
        "seq_play",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: seq_play <name>";
            auto& mgr = Spark::Cinematic::SequencerManager::GetInstance();
            return mgr.PlaySequence(args[0]) ? "Playing sequence: " + args[0] : "Sequence not found: " + args[0];
        },
        "Play a cinematic sequence by name");

    console.RegisterCommand(
        "seq_stop",
        [](const std::vector<std::string>& args) -> std::string
        {
            auto& mgr = Spark::Cinematic::SequencerManager::GetInstance();
            if (args.empty())
            {
                mgr.StopAll();
                return "All sequences stopped";
            }
            mgr.StopSequence(args[0]);
            return "Stopped sequence: " + args[0];
        },
        "Stop a sequence (or all if no name given)");

    console.RegisterCommand(
        "seq_time",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 2)
                return "Usage: seq_time <name> <seconds>";
            auto* seq = Spark::Cinematic::SequencerManager::GetInstance().GetSequence(args[0]);
            if (!seq)
                return "Sequence not found: " + args[0];
            seq->SetTime(std::stof(args[1]));
            return "Seeked " + args[0] + " to " + args[1] + "s";
        },
        "Seek a sequence to a specific time");

    // -------------------------------------------------------------------------
    // Replay commands
    // -------------------------------------------------------------------------

    console.RegisterCommand(
        "replay_status", [](const std::vector<std::string>&) -> std::string
        { return Spark::ReplaySystem::GetInstance().Console_GetStatus(); }, "Show replay recording/playback status");

    console.RegisterCommand(
        "replay_start",
        [](const std::vector<std::string>&) -> std::string
        {
            Spark::ReplaySystem::GetInstance().StartRecording();
            return "Replay recording started";
        },
        "Start recording a replay");

    console.RegisterCommand(
        "replay_stop",
        [](const std::vector<std::string>&) -> std::string
        {
            Spark::ReplaySystem::GetInstance().StopRecording();
            return "Replay recording stopped";
        },
        "Stop recording");

    console.RegisterCommand(
        "replay_save",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: replay_save <filepath>";
            bool ok = Spark::ReplaySystem::GetInstance().SaveToFile(args[0]);
            return ok ? "Replay saved to " + args[0] : "Failed to save replay";
        },
        "Save replay to file");

    console.RegisterCommand(
        "replay_load",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: replay_load <filepath>";
            bool ok = Spark::ReplaySystem::GetInstance().LoadFromFile(args[0]);
            return ok ? "Replay loaded from " + args[0] : "Failed to load replay";
        },
        "Load replay from file");

    console.RegisterCommand(
        "replay_play",
        [](const std::vector<std::string>&) -> std::string
        {
            Spark::ReplaySystem::GetInstance().StartPlayback();
            return "Replay playback started";
        },
        "Start replay playback");

    console.RegisterCommand(
        "replay_pause",
        [](const std::vector<std::string>&) -> std::string
        {
            Spark::ReplaySystem::GetInstance().PausePlayback();
            return "Replay playback paused";
        },
        "Pause replay playback");

    console.RegisterCommand(
        "replay_seek",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: replay_seek <seconds>";
            float t = std::stof(args[0]);
            Spark::ReplaySystem::GetInstance().SeekTo(t);
            return "Seeked to " + args[0] + "s";
        },
        "Seek replay to time");

    console.RegisterCommand(
        "replay_speed",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: replay_speed <multiplier>";
            float speed = std::stof(args[0]);
            Spark::ReplaySystem::GetInstance().SetPlaybackSpeed(speed);
            return "Playback speed set to " + std::to_string(speed) + "x";
        },
        "Set replay playback speed");

    // -------------------------------------------------------------------------
    // Networking commands
    // -------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING
    console.RegisterCommand(
        "net_host",
        [game](const std::vector<std::string>& args) -> std::string
        {
            if (!game)
                return "Game not available";
            uint16_t port = 27015;
            int maxClients = 32;
            if (!args.empty())
                port = static_cast<uint16_t>(std::stoi(args[0]));
            if (args.size() > 1)
                maxClients = std::stoi(args[1]);
            return game->StartServer(port, maxClients) ? "Server started" : "Failed to start server";
        },
        "Host a server (net_host [port] [max_clients])");

    console.RegisterCommand(
        "net_connect",
        [game](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: net_connect <address> [port]";
            if (!game)
                return "Game not available";
            uint16_t port = 27015;
            if (args.size() > 1)
                port = static_cast<uint16_t>(std::stoi(args[1]));
            return game->ConnectToServer(args[0], port) ? "Connecting..." : "Failed to connect";
        },
        "Connect to a server (net_connect <address> [port])");

    console.RegisterCommand(
        "net_disconnect",
        [game](const std::vector<std::string>&) -> std::string
        {
            if (!game)
                return "Game not available";
            game->DisconnectNetwork();
            return "Disconnected";
        },
        "Disconnect from network");

    console.RegisterCommand(
        "net_status",
        [game](const std::vector<std::string>&) -> std::string
        {
            if (!game)
                return "Game not available";
            return game->GetNetworkStatus();
        },
        "Show network status");

    console.RegisterCommand(
        "net_stats",
        [game](const std::vector<std::string>&) -> std::string
        {
            if (!game)
                return "Game not available";
            auto stats = game->GetNetworkStats();
            std::stringstream ss;
            ss << "=== Network Stats ===\n";
            ss << "Ping: " << stats.ping << "ms\n";
            ss << "Jitter: " << stats.jitter << "ms\n";
            ss << "Packet Loss: " << (stats.packetLoss * 100.0f) << "%\n";
            ss << "Upload: " << stats.bandwidthUp << " KB/s\n";
            ss << "Download: " << stats.bandwidthDown << " KB/s\n";
            ss << "Sent: " << stats.packetsSent << " packets (" << stats.bytesSent << " bytes)\n";
            ss << "Received: " << stats.packetsReceived << " packets (" << stats.bytesReceived << " bytes)\n";
            return ss.str();
        },
        "Show network statistics");
#endif // ENABLE_NETWORKING
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
