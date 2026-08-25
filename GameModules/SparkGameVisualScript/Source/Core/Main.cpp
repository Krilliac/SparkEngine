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
 * generated AngelScript assets, not in this C++ code.
 */

#include "SparkGameVisualScript.h"
#include "VisualScriptDemoRuntime.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Engine/Scripting/AngelScriptEngine.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "Utils/InvalidStateDetector.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <Spark/ModuleDllMain.h>

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
    info.version = "1.1.0";
    info.sdkVersion = SPARK_SDK_VERSION;
    info.loadOrder = 1010;
    return info;
}

bool SparkGameVisualScriptModule::OnLoad(Spark::IEngineContext* context)
{
    if (!context)
        return false;

    if (m_initialized)
        return true;

    m_context = context;
    auto& console = Spark::SimpleConsole::GetInstance();

    console.LogInfo("[VisualScript] Loading visual-script-only game module...");
    console.LogInfo("[VisualScript] ALL game logic is defined in visual scripts — zero C++ game code");

    const auto runtimeSupport = Spark::VisualScriptDemo::EvaluateRuntimeSupport(
        Spark::VisualScriptDemo::AngelScriptCompiledIn, m_context->GetWorld() != nullptr,
        m_context->GetScriptEngine() != nullptr);
    if (runtimeSupport != Spark::VisualScriptDemo::RuntimeSupport::Ready)
    {
        console.LogError("[VisualScript] " +
                         std::string(Spark::VisualScriptDemo::RuntimeSupportMessage(runtimeSupport)));
        m_context = nullptr;
        return false;
    }

    // Step 1: resolve and validate the complete script manifest exactly once.
    if (!LoadAndCompileScripts())
    {
        m_context = nullptr;
        return false;
    }

    // Step 2: spawn entities, bind each generated script to its real entity ID,
    // and call Start(). A partial demo is treated as a load failure.
    AngelScriptEngine::BindWorld(m_context->GetWorld());
    if (!SpawnGameEntities())
    {
        DestroyGameEntities();
        if (AngelScriptEngine::GetBoundWorld() == m_context->GetWorld())
            AngelScriptEngine::BindWorld(nullptr);
        m_scriptSources.clear();
        m_scriptRoot.clear();
        m_context = nullptr;
        return false;
    }

    RegisterConsoleCommands();

    // Register VisualScript state validation rules
    Spark::InvalidStateDetector::GetInstance().AddRule(
        {"VS.ScriptEntityHealth", "VisualScript", Spark::StateViolationSeverity::Warning, true,
         [](World& w, std::vector<Spark::StateViolation>& out)
         {
             for (auto entity : w.GetEntitiesWith<HealthComponent, NameComponent>())
             {
                 auto* h = w.GetComponent<HealthComponent>(entity);
                 auto* name = w.GetComponent<NameComponent>(entity);
                 if (h && name && name->name.starts_with("VS_") &&
                     (!std::isfinite(h->health) || h->health < 0.0f || h->health > h->maxHealth))
                 {
                     out.push_back({"VS.ScriptEntityHealth", static_cast<uint32_t>(entity),
                                    "Script entity health is non-finite or outside [0, maxHealth]",
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

    UnregisterConsoleCommands();
    Spark::InvalidStateDetector::GetInstance().RemoveRulesByCategory("VisualScript");
    DestroyGameEntities();
    if (m_context && AngelScriptEngine::GetBoundWorld() == m_context->GetWorld())
        AngelScriptEngine::BindWorld(nullptr);

    m_scriptSources.clear();
    m_scriptRoot.clear();
    m_context = nullptr;
    m_initialized = false;
    m_paused = false;
}

void SparkGameVisualScriptModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    auto* world = m_context ? m_context->GetWorld() : nullptr;
    auto* scriptEngine = m_context ? m_context->GetScriptEngine() : nullptr;
    const float scriptDeltaTime = Spark::VisualScriptDemo::SanitizeDeltaTime(deltaTime);
    if (!world || !scriptEngine || scriptDeltaTime <= 0.0f)
        return;

    for (EntityID entity : m_scriptEntities)
    {
        if (!world->GetRegistry().valid(entity))
        {
            scriptEngine->DetachScript(entity);
            continue;
        }

        auto* script = world->GetComponent<Script>(entity);
        if (script && script->enabled)
            scriptEngine->CallUpdate(entity, scriptDeltaTime);
    }
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

bool SparkGameVisualScriptModule::LoadAndCompileScripts()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    auto* asEngine = m_context ? m_context->GetScriptEngine() : nullptr;

    if (!asEngine)
    {
        console.LogError("[VisualScript] AngelScript engine not available");
        return false;
    }

    const std::array<std::filesystem::path, 2> searchPaths = {
        std::filesystem::path{"Assets/Scripts/Generated"},
        std::filesystem::path{"GameModules/SparkGameVisualScript/Assets/Scripts/Generated"},
    };

    const auto root =
        Spark::VisualScriptDemo::SelectCompleteScriptRoot(searchPaths,
                                                          [](const std::filesystem::path& path)
                                                          {
                                                              std::error_code error;
                                                              return std::filesystem::is_regular_file(path, error);
                                                          });
    if (!root)
    {
        console.LogError("[VisualScript] Could not find a complete five-script asset set");
        return false;
    }

    m_scriptRoot = *root;
    m_scriptSources.clear();

    for (const auto& asset : Spark::VisualScriptDemo::ScriptManifest)
    {
        const auto path = m_scriptRoot / std::filesystem::path(asset.fileName);
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            console.LogError("[VisualScript] Failed to read: " + path.string());
            m_scriptSources.clear();
            return false;
        }

        std::ostringstream source;
        source << stream.rdbuf();
        if (source.str().empty() || !asEngine->CompileScriptFile(path.string()))
        {
            console.LogError("[VisualScript] Failed to compile: " + path.string() + " — " + asEngine->GetLastError());
            m_scriptSources.clear();
            return false;
        }

        m_scriptSources.emplace(std::string(asset.className), source.str());
        console.LogSuccess("[VisualScript] Validated: " + std::string(asset.className));
    }

    console.LogInfo("[VisualScript] Validated 5 visual scripts from " + m_scriptRoot.string());
    return true;
}

// ============================================================================
// Entity Spawning — create game entities and attach visual scripts
// ============================================================================

bool SparkGameVisualScriptModule::SpawnGameEntities()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    auto* world = m_context->GetWorld();
    auto* asEngine = m_context->GetScriptEngine();

    if (!world || !asEngine)
    {
        console.LogError("[VisualScript] World or AngelScript not available — can't spawn entities");
        return false;
    }

    m_scriptEntities.clear();
    AngelScriptEngine::BindWorld(world);

    // --- Player entity ---
    // Visual script "PlayerController" handles: WASD movement, sprint, jump, health
    {
        auto player = world->CreateEntity("VS_Player");
        world->AddComponent<Transform>(player, Transform{{0.0f, 1.0f, 0.0f}, {0, 0, 0}, {1, 1, 1}});
        world->AddComponent<HealthComponent>(player, HealthComponent{100.0f, 100.0f});
        world->AddComponent<MeshRenderer>(player).meshPath = "Assets/Models/character.obj";
        if (!AttachScript(player, "PlayerController"))
            return false;
        console.LogInfo("[VisualScript] Spawned Player with PlayerController script");
    }

    // --- Collectible items ---
    // Visual script "Collectible" handles: spin, proximity pickup, and score increment
    for (int i = 0; i < 5; i++)
    {
        float x = -10.0f + i * 5.0f;
        float z = 8.0f + (i % 2) * 4.0f;
        std::string name = "VS_Coin_" + std::to_string(i);

        auto coin = world->CreateEntity(name);
        world->AddComponent<Transform>(coin, Transform{{x, 0.5f, z}, {0, 0, 0}, {0.5f, 0.5f, 0.5f}});
        auto& coinMesh = world->AddComponent<MeshRenderer>(coin);
        coinMesh.meshPath = "Assets/Models/Sphere.obj";
        coinMesh.emissive = 1.0f;
        if (!AttachScript(coin, "Collectible"))
            return false;
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
        world->AddComponent<HealthComponent>(enemy, HealthComponent{50.0f, 50.0f});
        world->AddComponent<MeshRenderer>(enemy).meshPath = "Assets/Models/Pyramid.obj";
        if (!AttachScript(enemy, "EnemyPatrol"))
            return false;
    }
    console.LogInfo("[VisualScript] Spawned 3 enemies with EnemyPatrol script");

    // --- Game Manager entity ---
    // Visual script "GameManager" handles: score tracking and win/lose conditions
    {
        auto manager = world->CreateEntity("VS_GameManager");
        world->AddComponent<HealthComponent>(manager, HealthComponent{0.0f, 500.0f});
        if (!AttachScript(manager, "GameManager"))
            return false;
        console.LogInfo("[VisualScript] Spawned GameManager with scoring/win-condition script");
    }

    // --- Healing pickup ---
    // Visual script "HealthPickup" handles: proximity healing and respawn cooldown
    {
        auto heal = world->CreateEntity("VS_HealthPack");
        world->AddComponent<Transform>(heal, Transform{{-5.0f, 0.3f, -5.0f}, {0, 0, 0}, {0.7f, 0.7f, 0.7f}});
        auto& healthMesh = world->AddComponent<MeshRenderer>(heal);
        healthMesh.meshPath = "Assets/Models/Cube.obj";
        healthMesh.emissive = 0.5f;
        if (!AttachScript(heal, "HealthPickup"))
            return false;
        console.LogInfo("[VisualScript] Spawned HealthPack with HealthPickup script");
    }

    console.LogInfo("[VisualScript] All game entities spawned — 11 entities, 5 script types, 0 lines of C++ game code");
    return m_scriptEntities.size() == Spark::VisualScriptDemo::ExpectedEntityCount;
}

bool SparkGameVisualScriptModule::AttachScript(EntityID entity, const std::string& className)
{
    auto* world = m_context ? m_context->GetWorld() : nullptr;
    auto* scriptEngine = m_context ? m_context->GetScriptEngine() : nullptr;
    if (!world || !scriptEngine || !world->GetRegistry().valid(entity))
        return false;

    // Track the entity before any fallible operation so a failed partial load is
    // rolled back by DestroyGameEntities().
    m_scriptEntities.push_back(entity);

    const auto source = m_scriptSources.find(className);
    if (source == m_scriptSources.end())
    {
        Spark::SimpleConsole::GetInstance().LogError("[VisualScript] Missing validated source for " + className);
        return false;
    }

    const uint32_t entityValue = static_cast<uint32_t>(entity);
    const auto boundSource = Spark::VisualScriptDemo::BindSelfEntity(source->second, entityValue);
    if (!boundSource)
    {
        Spark::SimpleConsole::GetInstance().LogError("[VisualScript] " + className +
                                                     " must declare selfEntity exactly once");
        return false;
    }

    const std::string moduleName = className + "_Entity_" + std::to_string(entityValue);
    if (!scriptEngine->CompileScriptFromString(*boundSource, moduleName))
    {
        Spark::SimpleConsole::GetInstance().LogError("[VisualScript] Failed to bind " + className + " to entity " +
                                                     std::to_string(entityValue) + " — " +
                                                     scriptEngine->GetLastError());
        return false;
    }

    Script script;
    script.scriptPath = (m_scriptRoot / (className + ".as")).string();
    script.className = className;
    script.moduleName = moduleName;
    world->AddComponent<Script>(entity, script);

    if (!scriptEngine->AttachScript(entity, className, moduleName))
    {
        Spark::SimpleConsole::GetInstance().LogError("[VisualScript] Failed to attach " + className + " — " +
                                                     scriptEngine->GetLastError());
        return false;
    }

    scriptEngine->CallStart(entity);
    world->GetComponent<Script>(entity)->started = true;
    return true;
}

void SparkGameVisualScriptModule::DestroyGameEntities()
{
    auto* world = m_context ? m_context->GetWorld() : nullptr;
    auto* scriptEngine = m_context ? m_context->GetScriptEngine() : nullptr;

    for (auto it = m_scriptEntities.rbegin(); it != m_scriptEntities.rend(); ++it)
    {
        if (scriptEngine)
            scriptEngine->DetachScript(*it);
        if (world && world->GetRegistry().valid(*it))
            world->DestroyEntity(*it);
    }
    m_scriptEntities.clear();
}

void SparkGameVisualScriptModule::RegisterConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    console.RegisterCommand(
        "vs_status", [this](const std::vector<std::string>&) { return GetStatusString(); },
        "Show the visual-script demo health, score, and entity status", "VisualScript");
    console.RegisterCommand(
        "vs_restart",
        [this](const std::vector<std::string>&)
        {
            if (!m_initialized || !m_context)
                return std::string{"Visual-script demo is not initialized"};

            DestroyGameEntities();
            if (!SpawnGameEntities())
            {
                DestroyGameEntities();
                return std::string{"Visual-script demo restart failed; inspect the script compilation log"};
            }
            return std::string{"Visual-script demo restarted\n"} + GetStatusString();
        },
        "Recreate the complete visual-script demo", "VisualScript");
    console.RegisterCommand(
        "vs_help",
        [](const std::vector<std::string>&)
        {
            return std::string{"Controls: WASD move, Left Shift sprint, Space jump. Collect five gold pickups, "
                               "avoid patrols, and use the green health pickup. Commands: vs_status, vs_restart."};
        },
        "Show visual-script demo controls", "VisualScript");
}

void SparkGameVisualScriptModule::UnregisterConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    console.UnregisterCommand("vs_status");
    console.UnregisterCommand("vs_restart");
    console.UnregisterCommand("vs_help");
}

std::string SparkGameVisualScriptModule::GetStatusString() const
{
    if (!m_context || !m_context->GetWorld())
        return "Visual-script demo is not initialized";

    const auto* world = m_context->GetWorld();
    uint32_t liveScripts = 0;
    uint32_t remainingCoins = 0;
    float playerHealth = 0.0f;
    float score = 0.0f;

    for (EntityID entity : m_scriptEntities)
    {
        if (!world->GetRegistry().valid(entity))
            continue;

        if (world->HasComponent<Script>(entity) && world->GetComponent<Script>(entity)->enabled)
            ++liveScripts;

        const auto* name = world->GetComponent<NameComponent>(entity);
        if (!name)
            continue;

        if (name->name == "VS_Player")
        {
            if (const auto* health = world->GetComponent<HealthComponent>(entity))
                playerHealth = health->health;
        }
        else if (name->name == "VS_GameManager")
        {
            if (const auto* gameState = world->GetComponent<HealthComponent>(entity))
                score = gameState->health;
        }
        else if (name->name.starts_with("VS_Coin_"))
        {
            const auto* transform = world->GetComponent<Transform>(entity);
            if (transform && transform->position.y > -50.0f)
                ++remainingCoins;
        }
    }

    std::ostringstream status;
    status << "=== Visual Script Demo ===\n"
           << "Scripts: " << liveScripts << "/" << Spark::VisualScriptDemo::ExpectedEntityCount << " active\n"
           << "Player health: " << playerHealth << "/100\n"
           << "Score: " << static_cast<uint32_t>((std::max)(score, 0.0f)) << "/500\n"
           << "Coins remaining: " << remainingCoins << "/5\n"
           << "Controls: WASD, Shift, Space | Reset: vs_restart";
    return status.str();
}
