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
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"

#include <Spark/IConsole.h>
#include <Spark/IStateValidation.h>
#include <Spark/ModuleDllMain.h>
#include <Spark/ModuleLog.h>

#include <string_view>
#include <utility>

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

    // cmake/RunSparkLinuxSDLConsoleStartup.cmake orders this OnLoad line against SimpleConsole startup.
    Spark::ModuleLog::Info(m_context, "[Default] Loading SparkGame showcase module");

    // Initialize the gameplay showcase
    m_showcase = std::make_unique<GameplayShowcase>();
    if (!m_showcase->Initialize(context))
    {
        Spark::ModuleLog::Warn(m_context,
                               "[Default] GameplayShowcase initialization failed — running without showcase");
        m_showcase.reset();
    }

    RegisterConsoleCommands();

    // Register base game state validation rules
    Spark::IStateValidation* stateRules = m_context->GetStateValidation();
    const bool stateRulesRegistered =
        stateRules != nullptr &&
        stateRules->AddRule("Base.HealthInvariant", "Base", Spark::StateViolationSeverity::Error,
                            [](World& w, std::vector<Spark::StateViolation>& out)
                            {
                                for (auto entity : w.GetEntitiesWith<HealthComponent>())
                                {
                                    auto* h = w.GetComponent<HealthComponent>(entity);
                                    if (h && h->maxHealth > 0.0f && h->health > h->maxHealth * 1.5f)
                                    {
                                        out.push_back(
                                            {"Base.HealthInvariant", static_cast<uint32_t>(entity),
                                             "health=" + std::to_string(h->health) +
                                                 " significantly exceeds maxHealth=" + std::to_string(h->maxHealth),
                                             Spark::StateViolationSeverity::Error});
                                    }
                                }
                            });
    if (!stateRulesRegistered)
    {
        Spark::ModuleLog::Warn(m_context, "[Default] Host refused the Base state-validation rules");
    }

    m_initialized = true;
    Spark::ModuleLog::Info(m_context, "[Default] Spark Engine Showcase module loaded successfully");
    return true;
}

void SparkGameDefaultModule::OnUnload()
{
    if (!m_initialized)
        return;

    Spark::ModuleLog::Info(m_context, "[Default] Unloading Spark Engine Showcase module...");

    // Command handlers are std::functions in this module image and call into the showcase torn down below.
    UnregisterConsoleCommands();

    if (m_showcase)
    {
        m_showcase->Shutdown();
        m_showcase.reset();
    }

    // This callback's std::function manager lives in this dynamic module.
    // Remove it while the image is still mapped so host-static registry
    // destruction cannot call into unloaded code.
    if (Spark::IStateValidation* stateRules = m_context ? m_context->GetStateValidation() : nullptr)
    {
        stateRules->RemoveRulesByCategory("Base");
    }

    // Logged before the context is released: ModuleLog is silent without one.
    Spark::ModuleLog::Info(m_context, "[Default] Spark Engine Showcase module unloaded");
    m_context = nullptr;
    m_initialized = false;
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
    Spark::IConsole* host = m_context->GetConsole();
    if (!host)
    {
        Spark::ModuleLog::Warn(m_context, "[Default] Host has no console; showcase commands are unavailable");
        return;
    }
    // Remember each accepted name so OnUnload removes exactly what this module registered.
    auto registerCommand = [this, host](std::string_view name, Spark::IConsole::CommandHandler handler,
                                        std::string_view help, std::string_view usage = {})
    {
        if (host->RegisterCommand(name, std::move(handler), help, "Showcase", usage))
        {
            m_consoleCommands.emplace_back(name);
        }
        else
        {
            Spark::ModuleLog::Warn(m_context, "[Default] Console command '{}' was not registered", name);
        }
    };

    registerCommand(
        "showcase_status",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_showcase)
                return "Showcase not initialized";
            return m_showcase->GetStatus();
        },
        "Show gameplay showcase status");

    registerCommand(
        "showcase_outcome",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_showcase)
            {
                return "Showcase not initialized";
            }
            return GameplayShowcase::FormatOutcome(m_showcase->GetOutcome());
        },
        "Print the showcase outcome as one SPARK_SHOWCASE_OUTCOME line");

    registerCommand(
        "showcase_weather",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_showcase)
                return "Showcase not initialized";
            return m_showcase->CycleWeather();
        },
        "Cycle to the next weather type");

    registerCommand(
        "showcase_save",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_showcase)
                return "Showcase not initialized";
            return m_showcase->DoQuickSave();
        },
        "QuickSave the current world state");

    registerCommand(
        "showcase_load",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_showcase)
                return "Showcase not initialized";
            return m_showcase->DoQuickLoad();
        },
        "QuickLoad the last saved world state");

    registerCommand(
        "showcase_spawn",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (!m_showcase)
                return "Showcase not initialized";
            std::string name = args.empty() ? "" : args[0];
            return m_showcase->SpawnEntity(name);
        },
        "Spawn a showcase entity (optional: name)", "showcase_spawn [name]");

    registerCommand(
        "showcase_language",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (!m_showcase)
            {
                return "Showcase not initialized";
            }
            if (args.empty())
            {
                return "Usage: showcase_language <en|fr>";
            }
            return m_showcase->SetLanguage(args[0]);
        },
        "Switch the showcase status language", "showcase_language <en|fr>");
}

void SparkGameDefaultModule::UnregisterConsoleCommands()
{
    if (Spark::IConsole* console = m_context ? m_context->GetConsole() : nullptr)
    {
        for (const std::string& name : m_consoleCommands)
        {
            console->UnregisterCommand(name);
        }
    }
    m_consoleCommands.clear();
}
