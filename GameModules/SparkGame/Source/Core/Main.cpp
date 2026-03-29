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

    SPARK_LOG_INFO(Spark::LogCategory::Game, "[Default] Loading Spark Engine Showcase module...");

    // Initialize the gameplay showcase
    m_showcase = std::make_unique<GameplayShowcase>();
    if (!m_showcase->Initialize(context))
    {
        SPARK_LOG_WARN(Spark::LogCategory::Game,
                       "[Default] GameplayShowcase initialization failed — running without showcase");
        m_showcase.reset();
    }

    RegisterConsoleCommands();

    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[Default] Spark Engine Showcase module loaded successfully");
    return true;
}

void SparkGameDefaultModule::OnUnload()
{
    if (!m_initialized)
        return;

    SPARK_LOG_INFO(Spark::LogCategory::Game, "[Default] Unloading Spark Engine Showcase module...");

    if (m_showcase)
    {
        m_showcase->Shutdown();
        m_showcase.reset();
    }

    m_context = nullptr;
    m_initialized = false;

    SPARK_LOG_INFO(Spark::LogCategory::Game, "[Default] Spark Engine Showcase module unloaded");
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
