/**
 * @file Main.cpp
 * @brief SparkGameVisualScript — IModule shell that loads visual scripts
 *
 * This is the ONLY C++ file in the game module. It does three things:
 *   1. Compiles all .as script files from the Assets/Scripts directory
 *   2. Spawns game entities (player, enemies, collectibles, world)
 *   3. Attaches the visual scripts to entities via the Script component
 *
 * ALL game logic — movement, combat, scoring, AI, win/lose — lives in
 * the visual script graphs (.vscript files), not in this C++ code.
 */

#include "SparkGameVisualScript.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Engine/Scripting/AngelScriptEngine.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "Utils/InvalidStateDetector.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"

#include <filesystem>

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

SPARK_IMPLEMENT_MODULE(SparkGameVisualScriptModule)

SparkGameVisualScriptModule::~SparkGameVisualScriptModule()
{
    if (m_initialized)
        OnUnload();
}

Spark::ModuleInfo SparkGameVisualScriptModule::GetModuleInfo() const
{
    Spark::ModuleInfo info{};
    info.name = "Spark Visual Script Game — Zero C++ Logic";
    info.version = "1.0.0";
    info.sdkVersion = SPARK_SDK_VERSION;
    info.loadOrder = 1010;
    return info;
}

bool SparkGameVisualScriptModule::OnLoad(Spark::IEngineContext* context)
{
    if (!context)
        return false;

    m_context = context;
    auto& console = Spark::SimpleConsole::GetInstance();

    console.LogInfo("[VisualScript] Loading visual-script-only game module...");
    console.LogInfo("[VisualScript] ALL game logic is defined in visual scripts — zero C++ game code");

    // Step 1: Compile all visual scripts
    LoadAndCompileScripts();

    // Step 2: Spawn entities and attach scripts
    SpawnGameEntities();

    // Register VisualScript state validation rules
    Spark::InvalidStateDetector::GetInstance().AddRule(
        {"VS.ScriptEntityHealth", "VisualScript", Spark::StateViolationSeverity::Warning, true,
         [](World& w, std::vector<Spark::StateViolation>& out)
         {
             for (auto entity : w.GetEntitiesWith<HealthComponent>())
             {
                 auto* h = w.GetComponent<HealthComponent>(entity);
                 if (h && h->isDead && !h->deathProcessed)
                 {
                     out.push_back({"VS.ScriptEntityHealth", static_cast<uint32_t>(entity),
                                    "Script entity dead but deathProcessed=false (script may have missed death event)",
                                    Spark::StateViolationSeverity::Warning});
                 }
             }
         }});

    m_initialized = true;
    console.LogInfo("[VisualScript] Module loaded — game is running entirely on visual scripts");
    return true;
}

void SparkGameVisualScriptModule::OnUnload()
{
    if (!m_initialized)
        return;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[VisualScript] Unloading visual script game module");

    m_context = nullptr;
    m_initialized = false;
}

void SparkGameVisualScriptModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    // All update logic is handled by AngelScriptEngine calling
    // Update(dt) on each entity's attached script — nothing to do here.
    (void)deltaTime;
}

void SparkGameVisualScriptModule::OnFixedUpdate(float fixedDeltaTime)
{
    (void)fixedDeltaTime;
}

void SparkGameVisualScriptModule::OnRender() {}

void SparkGameVisualScriptModule::OnResize(int width, int height)
{
    (void)width;
    (void)height;
}

void SparkGameVisualScriptModule::OnPause()
{
    m_paused = true;
}

void SparkGameVisualScriptModule::OnResume()
{
    m_paused = false;
}

void SparkGameVisualScriptModule::OnImGui() {}

// ============================================================================
// Script Loading — compile all .as files from the scripts directory
// ============================================================================

void SparkGameVisualScriptModule::LoadAndCompileScripts()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    auto* asEngine = AngelScriptEngine::GetInstance();

    if (!asEngine)
    {
        console.LogWarning("[VisualScript] AngelScript engine not available — scripts won't execute");
        return;
    }

    // Look for .as files in multiple locations
    std::vector<std::string> searchPaths = {
        "Assets/Scripts/Generated/",
        "GameModules/SparkGameVisualScript/Assets/Scripts/Generated/",
    };

    int compiled = 0;
    for (const auto& dir : searchPaths)
    {
        if (!std::filesystem::exists(dir))
            continue;

        for (const auto& entry : std::filesystem::directory_iterator(dir))
        {
            if (entry.path().extension() == ".as")
            {
                std::string path = entry.path().string();
                std::string moduleName = entry.path().stem().string();

                if (asEngine->CompileScriptFile(path))
                {
                    console.LogSuccess("[VisualScript] Compiled: " + moduleName);
                    compiled++;
                }
                else
                {
                    console.LogError("[VisualScript] Failed to compile: " + path + " — " + asEngine->GetLastError());
                }
            }
        }
    }

    console.LogInfo("[VisualScript] Compiled " + std::to_string(compiled) + " visual script(s)");
}

// ============================================================================
// Entity Spawning — create game entities and attach visual scripts
// ============================================================================

void SparkGameVisualScriptModule::SpawnGameEntities()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    auto* world = m_context->GetWorld();
    auto* asEngine = AngelScriptEngine::GetInstance();

    if (!world || !asEngine)
    {
        console.LogWarning("[VisualScript] World or AngelScript not available — can't spawn entities");
        return;
    }

    // Bind world for script API (getPosition, createEntity, etc.)
    AngelScriptEngine::BindWorld(world);

    // --- Player entity ---
    // Visual script "PlayerController" handles: WASD movement, mouse look, jump, health
    {
        auto player = world->CreateEntity("VS_Player");
        world->AddComponent<Transform>(player, Transform{{0.0f, 1.0f, 0.0f}, {0, 0, 0}, {1, 1, 1}});
        world->AddComponent<NameComponent>(player, NameComponent{"VS_Player"});

        Script scriptComp;
        scriptComp.scriptPath = "Assets/Scripts/Generated/PlayerController.as";
        scriptComp.className = "PlayerController";
        scriptComp.moduleName = "PlayerController";
        world->AddComponent<Script>(player, scriptComp);

        asEngine->AttachScript(player, "PlayerController", "PlayerController");
        console.LogInfo("[VisualScript] Spawned Player with PlayerController script");
    }

    // --- Collectible items ---
    // Visual script "Collectible" handles: spin animation, pickup on collision, score increment
    for (int i = 0; i < 5; i++)
    {
        float x = -10.0f + i * 5.0f;
        float z = 8.0f + (i % 2) * 4.0f;
        std::string name = "VS_Coin_" + std::to_string(i);

        auto coin = world->CreateEntity(name);
        world->AddComponent<Transform>(coin, Transform{{x, 0.5f, z}, {0, 0, 0}, {0.5f, 0.5f, 0.5f}});
        world->AddComponent<NameComponent>(coin, NameComponent{name});

        Script scriptComp;
        scriptComp.scriptPath = "Assets/Scripts/Generated/Collectible.as";
        scriptComp.className = "Collectible";
        scriptComp.moduleName = "Collectible";
        world->AddComponent<Script>(coin, scriptComp);

        asEngine->AttachScript(coin, "Collectible", "Collectible");
    }
    console.LogInfo("[VisualScript] Spawned 5 collectible items with Collectible script");

    // --- Enemy patrol entities ---
    // Visual script "EnemyPatrol" handles: waypoint patrol, player detection, chase, attack
    for (int i = 0; i < 3; i++)
    {
        float x = 15.0f + i * 10.0f;
        std::string name = "VS_Enemy_" + std::to_string(i);

        auto enemy = world->CreateEntity(name);
        world->AddComponent<Transform>(enemy, Transform{{x, 0.0f, 5.0f}, {0, 0, 0}, {1, 1, 1}});
        world->AddComponent<NameComponent>(enemy, NameComponent{name});
        world->AddComponent<HealthComponent>(enemy, HealthComponent{50.0f, 50.0f});

        Script scriptComp;
        scriptComp.scriptPath = "Assets/Scripts/Generated/EnemyPatrol.as";
        scriptComp.className = "EnemyPatrol";
        scriptComp.moduleName = "EnemyPatrol";
        world->AddComponent<Script>(enemy, scriptComp);

        asEngine->AttachScript(enemy, "EnemyPatrol", "EnemyPatrol");
    }
    console.LogInfo("[VisualScript] Spawned 3 enemies with EnemyPatrol script");

    // --- Game Manager entity ---
    // Visual script "GameManager" handles: score tracking, win condition, HUD updates
    {
        auto manager = world->CreateEntity("VS_GameManager");
        world->AddComponent<NameComponent>(manager, NameComponent{"VS_GameManager"});

        Script scriptComp;
        scriptComp.scriptPath = "Assets/Scripts/Generated/GameManager.as";
        scriptComp.className = "GameManager";
        scriptComp.moduleName = "GameManager";
        world->AddComponent<Script>(manager, scriptComp);

        asEngine->AttachScript(manager, "GameManager", "GameManager");
        console.LogInfo("[VisualScript] Spawned GameManager with scoring/win-condition script");
    }

    // --- Healing pickup ---
    // Visual script "HealthPickup" handles: heal player on collision, respawn after cooldown
    {
        auto heal = world->CreateEntity("VS_HealthPack");
        world->AddComponent<Transform>(heal, Transform{{-5.0f, 0.3f, -5.0f}, {0, 0, 0}, {0.7f, 0.7f, 0.7f}});
        world->AddComponent<NameComponent>(heal, NameComponent{"VS_HealthPack"});

        Script scriptComp;
        scriptComp.scriptPath = "Assets/Scripts/Generated/HealthPickup.as";
        scriptComp.className = "HealthPickup";
        scriptComp.moduleName = "HealthPickup";
        world->AddComponent<Script>(heal, scriptComp);

        asEngine->AttachScript(heal, "HealthPickup", "HealthPickup");
        console.LogInfo("[VisualScript] Spawned HealthPack with HealthPickup script");
    }

    console.LogInfo("[VisualScript] All game entities spawned — 11 entities, 5 script types, 0 lines of C++ game code");
}
