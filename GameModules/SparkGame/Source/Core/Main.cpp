/**
 * @file Main.cpp
 * @brief SparkGame DLL - IModule implementation and exports
 *
 * Implements the SparkGameDefaultModule class and exports the CreateModule/
 * DestroyModule factory functions for the engine's ModuleManager.
 *
 * This is the minimal default game module — a blank slate with no
 * subsystems. Extend from here to build your game.
 */

#include "SparkGame.h"
#include "Utils/SparkConsole.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>

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

SparkGameDefaultModule::~SparkGameDefaultModule()
{
    if (m_initialized)
        OnUnload();
}

Spark::ModuleInfo SparkGameDefaultModule::GetModuleInfo() const
{
    Spark::ModuleInfo info{};
    info.name = "Spark Default - Engine Template";
    info.version = "1.0.0";
    info.sdkVersion = SPARK_SDK_VERSION;
    info.loadOrder = 999;
    return info;
}

bool SparkGameDefaultModule::OnLoad(Spark::IEngineContext* context)
{
    if (!context)
        return false;

    m_context = context;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[Default] Loading Spark Default module...");

    RegisterConsoleCommands();

    m_initialized = true;
    console.LogInfo("[Default] Spark Default module loaded successfully");
    return true;
}

void SparkGameDefaultModule::OnUnload()
{
    if (!m_initialized)
        return;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[Default] Unloading Spark Default module...");

    m_context = nullptr;
    m_initialized = false;

    console.LogInfo("[Default] Spark Default module unloaded");
}

void SparkGameDefaultModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    (void)deltaTime;
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
}

void SparkGameDefaultModule::RegisterConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    console.RegisterCommand("default_status",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                if (!m_initialized)
                                    return "Default module not initialized";

                                auto info = GetModuleInfo();
                                std::string status = "=== Spark Default Module ===\n";
                                status += "Name: " + std::string(info.name) + "\n";
                                status += "Version: " + std::string(info.version) + "\n";
                                status += "Status: Running\n";
                                status += "Paused: " + std::string(m_paused ? "Yes" : "No") + "\n";
                                return status;
                            });
}
