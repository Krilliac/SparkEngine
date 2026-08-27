/**
 * @file Main.cpp
 * @brief SparkGame DLL - IModule implementation and exports
 *
 * Implements the SparkGameDefaultModule class and exports the CreateModule/
 * DestroyModule factory functions for the engine's ModuleManager.
 *
 * This module showcases core engine subsystem integration via GameplayShowcase.
 */

#include "SparkGame.h"
#include "GameplayShowcase.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "Utils/InvalidStateDetector.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"

#include <Spark/ModuleDllMain.h>

// =============================================================================
// Module exports
// =============================================================================

SPARK_IMPLEMENT_MODULE(SparkGameDefaultModule)

// =============================================================================
// SparkGameDefaultModule implementation
// =============================================================================

SparkGameDefaultModule::SparkGameDefaultModule() = default;

SparkGameDefaultModule::~SparkGameDefaultModule()
{
    if (m_initialized)
        OnUnload();
}

Spark::ModuleInfo SparkGameDefaultModule::GetModuleInfo() const
{
    Spark::ModuleInfo info{};
    info.name = "Spark Default - Engine Showcase";
    info.version = "2.0.0";
    info.sdkVersion = SPARK_SDK_VERSION;
    info.loadOrder = 999;
    return info;
}

bool SparkGameDefaultModule::OnLoad(Spark::IEngineContext* context)
{
    if (!context)
        return false;

    m_context = context;

    SPARK_LOG_INFO(Spark::LogCategory::Game, "Loading SparkGame showcase module");

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[Default] Loading Spark Engine Showcase module...");

    // Initialize the gameplay showcase
    m_showcase = std::make_unique<GameplayShowcase>();
    if (!m_showcase->Initialize(context))
    {
        console.LogWarning("[Default] GameplayShowcase initialization failed — running without showcase");
        m_showcase.reset();
    }

    RegisterConsoleCommands();

    // Register base game state validation rules
    Spark::InvalidStateDetector::GetInstance().AddRule(
        {"Base.HealthInvariant", "Base", Spark::StateViolationSeverity::Error, true,
         [](World& w, std::vector<Spark::StateViolation>& out)
         {
             for (auto entity : w.GetEntitiesWith<HealthComponent>())
             {
                 auto* h = w.GetComponent<HealthComponent>(entity);
                 if (h && h->maxHealth > 0.0f && h->health > h->maxHealth * 1.5f)
                 {
                     out.push_back({"Base.HealthInvariant", static_cast<uint32_t>(entity),
                                    "health=" + std::to_string(h->health) +
                                        " significantly exceeds maxHealth=" + std::to_string(h->maxHealth),
                                    Spark::StateViolationSeverity::Error});
                 }
             }
         }});

    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "SparkGame showcase module loaded successfully");
    console.LogInfo("[Default] Spark Engine Showcase module loaded successfully");
    return true;
}

void SparkGameDefaultModule::OnUnload()
{
    if (!m_initialized)
        return;

    SPARK_LOG_INFO(Spark::LogCategory::Game, "Unloading SparkGame showcase module");
    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[Default] Unloading Spark Engine Showcase module...");

    if (m_showcase)
    {
        m_showcase->Shutdown();
        m_showcase.reset();
    }

    console.UnregisterCommand("showcase_status");
    console.UnregisterCommand("showcase_weather");
    console.UnregisterCommand("showcase_save");
    console.UnregisterCommand("showcase_load");
    console.UnregisterCommand("showcase_spawn");

    // This callback's std::function manager lives in this dynamic module.
    // Remove it while the image is still mapped so host-static registry
    // destruction cannot call into unloaded code.
    Spark::InvalidStateDetector::GetInstance().RemoveRulesByCategory("Base");

    m_context = nullptr;
    m_initialized = false;

    console.LogInfo("[Default] Spark Engine Showcase module unloaded");
}

void SparkGameDefaultModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    if (m_showcase)
    {
        m_showcase->Update(deltaTime);
    }
}

void SparkGameDefaultModule::OnFixedUpdate(float fixedDeltaTime)
{
    if (!m_initialized || m_paused)
        return;

    (void)fixedDeltaTime;
}

void SparkGameDefaultModule::OnRender()
{
    if (!m_initialized)
        return;
}

void SparkGameDefaultModule::OnResize(int width, int height)
{
    (void)width;
    (void)height;
}

void SparkGameDefaultModule::OnPause()
{
    m_paused = true;
}

void SparkGameDefaultModule::OnResume()
{
    m_paused = false;
}

void SparkGameDefaultModule::OnImGui()
{
    if (!m_initialized)
        return;

    if (m_showcase)
    {
        m_showcase->RenderDebugUI();
    }
}

void SparkGameDefaultModule::RegisterConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    console.RegisterCommand(
        "showcase_status",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_showcase)
                return "Showcase not initialized";
            return m_showcase->GetStatus();
        },
        "Show gameplay showcase status", "Showcase");

    console.RegisterCommand(
        "showcase_weather",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_showcase)
                return "Showcase not initialized";
            return m_showcase->CycleWeather();
        },
        "Cycle to the next weather type", "Showcase");

    console.RegisterCommand(
        "showcase_save",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_showcase)
                return "Showcase not initialized";
            return m_showcase->DoQuickSave();
        },
        "QuickSave the current world state", "Showcase");

    console.RegisterCommand(
        "showcase_load",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_showcase)
                return "Showcase not initialized";
            return m_showcase->DoQuickLoad();
        },
        "QuickLoad the last saved world state", "Showcase");

    console.RegisterCommand(
        "showcase_spawn",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (!m_showcase)
                return "Showcase not initialized";
            std::string name = args.empty() ? "" : args[0];
            return m_showcase->SpawnEntity(name);
        },
        "Spawn a showcase entity (optional: name)", "Showcase", "showcase_spawn [name]");
}
