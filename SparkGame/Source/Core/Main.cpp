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
#include "Utils/SparkConsole.h"

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
    info.name       = "Spark Game";
    info.version    = "1.0.0";
    info.sdkVersion = SPARK_SDK_VERSION;
    info.loadOrder  = 1000;
    return info;
}

bool SparkGameModule::OnLoad(Spark::IEngineContext* context)
{
    // Delegate to the shared Initialize logic using the context's subsystems
    return Initialize(context->GetGraphics(), context->GetInput());
}

void SparkGameModule::OnUnload()
{
    Shutdown();
}

void SparkGameModule::OnUpdate(float deltaTime)
{
    if (m_game && !m_game->IsPaused())
        m_game->Update(deltaTime);
}

void SparkGameModule::OnRender()
{
    if (m_game)
        m_game->Render();
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
    if (m_initialized) return true; // Prevent double-init

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("Initializing SparkGame module...");

    m_game = std::make_unique<Game>();
    HRESULT hr = m_game->Initialize(graphics, input);
    if (FAILED(hr))
    {
        console.LogError("Game::Initialize() failed");
        m_game.reset();
        return false;
    }

    // Initialize the in-game console overlay
    m_console = std::make_unique<Console>();
    m_console->Initialize(1280, 720);

    // Register game-specific console commands
    RegisterGameConsoleCommands();

    m_initialized = true;
    console.LogSuccess("SparkGame module initialized");
    return true;
}

void SparkGameModule::Shutdown()
{
    if (!m_initialized) return;

    if (m_game)
    {
        m_game->Shutdown();
        m_game.reset();
    }
    m_console.reset();
    m_initialized = false;

    Spark::SimpleConsole::GetInstance().LogInfo("SparkGame module shut down");
}

void SparkGameModule::Update(float deltaTime)
{
    if (m_game)
        m_game->Update(deltaTime);
}

void SparkGameModule::Render()
{
    if (m_game)
        m_game->Render();
}

void SparkGameModule::OnResize(int width, int height)
{
    (void)width;
    (void)height;
}

void SparkGameModule::Pause()
{
    if (m_game) m_game->Pause();
}

void SparkGameModule::Resume()
{
    if (m_game) m_game->Resume();
}

bool SparkGameModule::IsPaused() const
{
    return m_game ? m_game->IsPaused() : false;
}

// ===================================================================================
// Game-specific console commands (registered when game module loads)
// ===================================================================================
void SparkGameModule::RegisterGameConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    Game* game = m_game.get();

    console.RegisterCommand("game_timescale", [game](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: game_timescale <scale>";
        if (!game) return "Game not available";
        try {
            float scale = std::stof(args[0]);
            game->SetTimeScale(scale);
            return "Time scale set to " + std::to_string(scale);
        } catch (const std::exception& e) {
            return std::string("Error: ") + e.what();
        }
    }, "Set game time scale");

    console.RegisterCommand("player_tp", [game](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 3) return "Usage: player_tp <x> <y> <z>";
        if (!game) return "Game not available";
        try {
            float x = std::stof(args[0]);
            float y = std::stof(args[1]);
            float z = std::stof(args[2]);
            game->TeleportPlayer(x, y, z);
            return "Teleported to (" + args[0] + ", " + args[1] + ", " + args[2] + ")";
        } catch (const std::exception& e) {
            return std::string("Error: ") + e.what();
        }
    }, "Teleport player to coordinates");

    console.RegisterCommand("spawn", [game](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 4) return "Usage: spawn <type> <x> <y> <z>";
        if (!game) return "Game not available";
        try {
            float x = std::stof(args[1]);
            float y = std::stof(args[2]);
            float z = std::stof(args[3]);
            bool ok = game->SpawnObject(args[0], x, y, z);
            return ok ? "Spawned " + args[0] : "Failed to spawn '" + args[0] + "'";
        } catch (const std::exception& e) {
            return std::string("Error: ") + e.what();
        }
    }, "Spawn an object at coordinates");

    console.RegisterCommand("god", [game](const std::vector<std::string>& args) -> std::string {
        if (!game) return "Game not available";
        bool enable = args.empty() || (args[0] == "on" || args[0] == "true" || args[0] == "1");
        game->ApplyDebugSettings(enable, false, false);
        return enable ? "God mode enabled" : "God mode disabled";
    }, "Toggle god mode");

    console.RegisterCommand("noclip", [game](const std::vector<std::string>& args) -> std::string {
        if (!game) return "Game not available";
        bool enable = args.empty() || (args[0] == "on" || args[0] == "true" || args[0] == "1");
        game->ApplyDebugSettings(false, enable, false);
        return enable ? "Noclip enabled" : "Noclip disabled";
    }, "Toggle noclip mode");

    console.RegisterCommand("game_stats", [game](const std::vector<std::string>&) -> std::string {
        if (!game) return "Game not available";
        int drawCalls, triangles, activeObjects;
        game->GetPerformanceStats(drawCalls, triangles, activeObjects);
        std::stringstream ss;
        ss << "=== Game Stats ===\n";
        ss << "Draw Calls: " << drawCalls << "\n";
        ss << "Triangles: " << triangles << "\n";
        ss << "Active Objects: " << activeObjects << "\n";
        ss << "Time Scale: " << game->GetTimeScale() << "\n";
        return ss.str();
    }, "Display game performance statistics");
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
