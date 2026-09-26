/**
 * @file Main.cpp
 * @brief SparkGameVisualScript — IModule shell that loads visual scripts
 *
 * The IModule shell: it checks runtime prerequisites, then hands script
 * validation, entity spawning and rollback to VisualScriptDemoWorld, and
 * exposes the vs_* console commands.
 *
 * ALL game logic — movement, combat, scoring, AI, win/lose — lives in
 * generated AngelScript assets, not in this C++ code.
 */

#include "SparkGameVisualScript.h"
#include "VisualScriptDemoRuntime.h"
#include "VisualScriptDemoWorld.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Engine/Scripting/AngelScriptEngine.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "Utils/InvalidStateDetector.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
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
    // Step 2: spawn entities, bind each generated script to its real entity ID,
    // and call Start(). A partial demo is rolled back and treated as a load failure.
    const std::array<std::filesystem::path, 2> searchPaths = {
        std::filesystem::path{"Assets/Scripts/Generated"},
        std::filesystem::path{"GameModules/SparkGameVisualScript/Assets/Scripts/Generated"},
    };
    auto demo =
        std::make_unique<Spark::VisualScriptDemo::DemoWorld>(*m_context->GetWorld(), *m_context->GetScriptEngine());
    if (!demo->LoadScripts(searchPaths) || !demo->Spawn())
    {
        m_context = nullptr;
        return false;
    }
    m_demo = std::move(demo);

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
    m_demo.reset();
    if (m_context && AngelScriptEngine::GetBoundWorld() == m_context->GetWorld())
        AngelScriptEngine::BindWorld(nullptr);

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
    if (!world || !scriptEngine || !m_demo || scriptDeltaTime <= 0.0f)
        return;

    for (EntityID entity : m_demo->GetEntities())
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
            if (!m_initialized || !m_context || !m_demo)
                return std::string{"Visual-script demo is not initialized"};

            // Spawn() destroys the previous entities first and rolls back a partial restart.
            if (!m_demo->Spawn())
                return "Visual-script demo restart failed: " + m_demo->GetLastError();
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
    if (!m_context || !m_context->GetWorld() || !m_demo)
        return "Visual-script demo is not initialized";

    const auto* world = m_context->GetWorld();
    uint32_t liveScripts = 0;
    uint32_t remainingCoins = 0;
    float playerHealth = 0.0f;
    float score = 0.0f;

    for (EntityID entity : m_demo->GetEntities())
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
