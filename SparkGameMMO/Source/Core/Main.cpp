/**
 * @file Main.cpp
 * @brief SparkGameMMO DLL - IModule implementation and exports
 *
 * Implements the SparkGameMMOModule class and exports the CreateModule/
 * DestroyModule factory functions for the engine's ModuleManager.
 */

#include "SparkGameMMO.h"
#include "World/MMOWorldSetup.h"
#include "Player/MMOPlayerSystem.h"
#include "Chat/MMOChatSystem.h"
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

SPARK_IMPLEMENT_MODULE(SparkGameMMOModule)

// =============================================================================
// SparkGameMMOModule implementation
// =============================================================================

SparkGameMMOModule::SparkGameMMOModule() = default;

SparkGameMMOModule::~SparkGameMMOModule()
{
    if (m_initialized)
        OnUnload();
}

Spark::ModuleInfo SparkGameMMOModule::GetModuleInfo() const
{
    Spark::ModuleInfo info{};
    info.name = "Spark MMO - Networking Showcase";
    info.version = "1.0.0";
    info.sdkVersion = SPARK_SDK_VERSION;
    info.loadOrder = 1001; // Load after SparkGame if both present
    return info;
}

bool SparkGameMMOModule::OnLoad(Spark::IEngineContext* context)
{
    if (!context)
        return false;

    m_context = context;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[MMO] Loading Spark MMO module...");

    // Initialize the world area setup (registers areas with streaming/area servers)
    m_worldSetup = std::make_unique<MMO::MMOWorldSetup>();
    if (!m_worldSetup->Initialize(context))
    {
        console.LogError("[MMO] Failed to initialize world setup");
        return false;
    }

    // Initialize player system (spawning, replication, prediction)
    m_playerSystem = std::make_unique<MMO::MMOPlayerSystem>();
    if (!m_playerSystem->Initialize(context))
    {
        console.LogError("[MMO] Failed to initialize player system");
        return false;
    }

    // Initialize chat system (area, global, party, whisper channels)
    m_chatSystem = std::make_unique<MMO::MMOChatSystem>();
    if (!m_chatSystem->Initialize(context))
    {
        console.LogError("[MMO] Failed to initialize chat system");
        return false;
    }

    RegisterConsoleCommands();

    m_initialized = true;
    console.LogInfo("[MMO] Spark MMO module loaded successfully");
    console.LogInfo("[MMO] World areas: " + std::to_string(m_worldSetup->GetAreaCount()));
    return true;
}

void SparkGameMMOModule::OnUnload()
{
    if (!m_initialized)
        return;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[MMO] Unloading Spark MMO module...");

    if (m_chatSystem)
    {
        m_chatSystem->Shutdown();
        m_chatSystem.reset();
    }

    if (m_playerSystem)
    {
        m_playerSystem->Shutdown();
        m_playerSystem.reset();
    }

    if (m_worldSetup)
    {
        m_worldSetup->Shutdown();
        m_worldSetup.reset();
    }

    m_context = nullptr;
    m_initialized = false;
    console.LogInfo("[MMO] Spark MMO module unloaded");
}

void SparkGameMMOModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_worldSetup->Update(deltaTime);
    m_playerSystem->Update(deltaTime);
    m_chatSystem->Update(deltaTime);
}

void SparkGameMMOModule::OnFixedUpdate(float fixedDeltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_playerSystem->FixedUpdate(fixedDeltaTime);
}

void SparkGameMMOModule::OnRender()
{
    if (!m_initialized)
        return;

    m_playerSystem->Render();
}

void SparkGameMMOModule::OnResize(int width, int height)
{
    (void)width;
    (void)height;
}

void SparkGameMMOModule::OnPause()
{
    m_paused = true;
}

void SparkGameMMOModule::OnResume()
{
    m_paused = false;
}

void SparkGameMMOModule::OnImGui()
{
    if (!m_initialized)
        return;

    m_worldSetup->RenderDebugUI();
    m_playerSystem->RenderDebugUI();
    m_chatSystem->RenderDebugUI();
}

void SparkGameMMOModule::RegisterConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    console.RegisterCommand("mmo_status",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                if (!m_worldSetup)
                                    return "MMO module not initialized";

                                std::string status = "=== Spark MMO Status ===\n";
                                status += "Areas: " + std::to_string(m_worldSetup->GetAreaCount()) + "\n";
                                status += "Players: " + std::to_string(m_playerSystem->GetPlayerCount()) + "\n";
                                status += "Chat channels: " + std::to_string(m_chatSystem->GetChannelCount()) + "\n";
                                status += m_worldSetup->GetWorldStatusString();
                                return status;
                            });

    console.RegisterCommand("mmo_chat",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (args.size() < 2)
                                    return "Usage: mmo_chat <channel> <message>";

                                std::string message;
                                for (size_t i = 1; i < args.size(); ++i)
                                {
                                    if (i > 1)
                                        message += " ";
                                    message += args[i];
                                }
                                m_chatSystem->SendMessage(args[0], message);
                                return "";
                            });

    console.RegisterCommand("mmo_areas", [this](const std::vector<std::string>&) -> std::string
                            { return m_worldSetup->GetAreaListString(); });

    console.RegisterCommand("mmo_players", [this](const std::vector<std::string>&) -> std::string
                            { return m_playerSystem->GetPlayerListString(); });
}
