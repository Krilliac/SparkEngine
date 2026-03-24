/**
 * @file Main.cpp
 * @brief SparkGamePlatformer DLL - IModule implementation and exports
 *
 * Implements the SparkGamePlatformerModule class and exports the CreateModule/
 * DestroyModule factory functions for the engine's ModuleManager.
 */

#include "SparkGamePlatformer.h"
#include "PlatformerEngineSystems.h"
#include "Player/PlatformerPlayerController.h"
#include "Level/PlatformerLevelSystem.h"
#include "Collectible/PlatformerCollectibleSystem.h"
#include "Hazard/PlatformerHazardSystem.h"
#include "Checkpoint/PlatformerCheckpointSystem.h"
#include "Camera/PlatformerCameraSystem.h"
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

SPARK_IMPLEMENT_MODULE(SparkGamePlatformerModule)

// =============================================================================
// SparkGamePlatformerModule implementation
// =============================================================================

SparkGamePlatformerModule::SparkGamePlatformerModule() = default;

SparkGamePlatformerModule::~SparkGamePlatformerModule()
{
    if (m_initialized)
        OnUnload();
}

Spark::ModuleInfo SparkGamePlatformerModule::GetModuleInfo() const
{
    Spark::ModuleInfo info{};
    info.name = "Spark Platformer - Engine Showcase";
    info.version = "1.0.0";
    info.sdkVersion = SPARK_SDK_VERSION;
    info.loadOrder = 1004;
    return info;
}

bool SparkGamePlatformerModule::OnLoad(Spark::IEngineContext* context)
{
    if (!context)
        return false;

    m_context = context;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[Platformer] Loading Spark Platformer module...");

    // Initialize level system first (provides platform/spawn data to other systems)
    m_levelSystem = std::make_unique<Platformer::PlatformerLevelSystem>();
    if (!m_levelSystem->Initialize(context))
    {
        console.LogError("[Platformer] Failed to initialize level system");
        return false;
    }

    // Initialize checkpoint system (needs level data for placement)
    m_checkpointSystem = std::make_unique<Platformer::PlatformerCheckpointSystem>();
    if (!m_checkpointSystem->Initialize(context))
    {
        console.LogError("[Platformer] Failed to initialize checkpoint system");
        return false;
    }

    // Initialize player controller (movement, jumping, abilities)
    m_playerController = std::make_unique<Platformer::PlatformerPlayerController>();
    if (!m_playerController->Initialize(context, m_checkpointSystem.get()))
    {
        console.LogError("[Platformer] Failed to initialize player controller");
        return false;
    }

    // Initialize collectible system (coins, gems, stars, power-ups)
    m_collectibleSystem = std::make_unique<Platformer::PlatformerCollectibleSystem>();
    if (!m_collectibleSystem->Initialize(context))
    {
        console.LogError("[Platformer] Failed to initialize collectible system");
        return false;
    }

    // Initialize hazard system (spikes, lava, crushers, sawblades)
    m_hazardSystem = std::make_unique<Platformer::PlatformerHazardSystem>();
    if (!m_hazardSystem->Initialize(context))
    {
        console.LogError("[Platformer] Failed to initialize hazard system");
        return false;
    }

    // Initialize camera system (third-person follow)
    m_cameraSystem = std::make_unique<Platformer::PlatformerCameraSystem>();
    if (!m_cameraSystem->Initialize(context))
    {
        console.LogError("[Platformer] Failed to initialize camera system");
        return false;
    }

    // Wire engine subsystems (audio, events, save, destruction, replay, coroutines, localization)
    m_engineSystems = std::make_unique<Platformer::PlatformerEngineSystems>();
    if (!m_engineSystems->Initialize(context))
    {
        console.LogError("[Platformer] Failed to initialize engine systems");
        return false;
    }

    RegisterConsoleCommands();

    m_initialized = true;
    console.LogInfo("[Platformer] Spark Platformer module loaded successfully (7 subsystems)");
    console.LogInfo("[Platformer] Levels: " + std::to_string(m_levelSystem->GetLevelCount()) +
                    " | Collectibles: " + std::to_string(m_collectibleSystem->GetTotalCollectibleCount()) +
                    " | Hazards: " + std::to_string(m_hazardSystem->GetHazardCount()) +
                    " | Checkpoints: " + std::to_string(m_checkpointSystem->GetCheckpointCount()));
    return true;
}

void SparkGamePlatformerModule::OnUnload()
{
    if (!m_initialized)
        return;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[Platformer] Unloading Spark Platformer module...");

    // Shutdown in reverse initialization order
    if (m_engineSystems)
    {
        m_engineSystems->Shutdown();
        m_engineSystems.reset();
    }
    if (m_cameraSystem)
    {
        m_cameraSystem->Shutdown();
        m_cameraSystem.reset();
    }
    if (m_hazardSystem)
    {
        m_hazardSystem->Shutdown();
        m_hazardSystem.reset();
    }
    if (m_collectibleSystem)
    {
        m_collectibleSystem->Shutdown();
        m_collectibleSystem.reset();
    }
    if (m_playerController)
    {
        m_playerController->Shutdown();
        m_playerController.reset();
    }
    if (m_checkpointSystem)
    {
        m_checkpointSystem->Shutdown();
        m_checkpointSystem.reset();
    }
    if (m_levelSystem)
    {
        m_levelSystem->Shutdown();
        m_levelSystem.reset();
    }

    m_context = nullptr;
    m_initialized = false;
    console.LogInfo("[Platformer] Spark Platformer module unloaded");
}

void SparkGamePlatformerModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_levelSystem->Update(deltaTime);
    m_playerController->Update(deltaTime);
    m_collectibleSystem->Update(deltaTime);
    m_hazardSystem->Update(deltaTime);
    m_checkpointSystem->Update(deltaTime);
    m_cameraSystem->Update(deltaTime, m_playerController->GetPlayerPosition());
    m_engineSystems->Update(deltaTime);
}

void SparkGamePlatformerModule::OnFixedUpdate(float fixedDeltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_playerController->FixedUpdate(fixedDeltaTime);
}

void SparkGamePlatformerModule::OnRender()
{
    if (!m_initialized)
        return;

    m_levelSystem->Render();
    m_collectibleSystem->Render();
    m_hazardSystem->Render();
    m_playerController->Render();
}

void SparkGamePlatformerModule::OnResize(int width, int height)
{
    (void)width;
    (void)height;
}

void SparkGamePlatformerModule::OnPause()
{
    m_paused = true;
}

void SparkGamePlatformerModule::OnResume()
{
    m_paused = false;
}

void SparkGamePlatformerModule::OnImGui()
{
    if (!m_initialized)
        return;

    m_playerController->RenderDebugUI();
    m_levelSystem->RenderDebugUI();
    m_collectibleSystem->RenderDebugUI();
    m_hazardSystem->RenderDebugUI();
    m_checkpointSystem->RenderDebugUI();
    m_cameraSystem->RenderDebugUI();
}

void SparkGamePlatformerModule::RegisterConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    console.RegisterCommand("platformer_status",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                if (!m_levelSystem)
                                    return "Platformer module not initialized";

                                std::string status = "=== Spark Platformer Status ===\n";
                                status +=
                                    "Current Level: " + std::to_string(m_levelSystem->GetCurrentLevelIndex()) + "\n";
                                status += "Total Levels: " + std::to_string(m_levelSystem->GetLevelCount()) + "\n";
                                status += "Player State: " + m_playerController->GetStateString() + "\n";
                                status += "Lives: " + std::to_string(m_playerController->GetLives()) + "\n";
                                status += "Coins: " + std::to_string(m_collectibleSystem->GetCoinsCollected()) + "\n";
                                status += "Stars: " + std::to_string(m_collectibleSystem->GetStarsCollected()) + "\n";
                                status +=
                                    "Checkpoints: " + std::to_string(m_checkpointSystem->GetActivatedCount()) + "\n";
                                return status;
                            });

    console.RegisterCommand("platformer_levels", [this](const std::vector<std::string>&) -> std::string
                            { return m_levelSystem->GetLevelListString(); });

    console.RegisterCommand("platformer_player", [this](const std::vector<std::string>&) -> std::string
                            { return m_playerController->GetPlayerStatusString(); });

    console.RegisterCommand("platformer_level",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (args.empty())
                                    return "Usage: platformer_level <index>";
                                uint32_t idx = static_cast<uint32_t>(std::stoi(args[0]));
                                return m_levelSystem->LoadLevel(idx) ? "Level " + std::to_string(idx) + " loaded"
                                                                     : "Failed to load level";
                            });

    console.RegisterCommand("platformer_respawn",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                m_playerController->Respawn();
                                return "Player respawned at last checkpoint";
                            });

    console.RegisterCommand("platformer_collectibles", [this](const std::vector<std::string>&) -> std::string
                            { return m_collectibleSystem->GetCollectionString(); });

    // --- Engine system commands ---

    console.RegisterCommand("plat_save",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                std::string slot = args.empty() ? "plat_slot1" : args[0];
                                return m_engineSystems->SaveProgress(slot) ? "Saved to " + slot : "Save failed";
                            });

    console.RegisterCommand("plat_load",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                std::string slot = args.empty() ? "plat_slot1" : args[0];
                                return m_engineSystems->LoadProgress(slot) ? "Loaded from " + slot : "Load failed";
                            });

    console.RegisterCommand("plat_replay_start",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                m_engineSystems->StartReplayRecording();
                                return "Replay recording started";
                            });

    console.RegisterCommand("plat_replay_stop",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                m_engineSystems->StopReplayRecording();
                                return "Replay recording stopped and saved";
                            });

    console.RegisterCommand("plat_ghost",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                m_engineSystems->ToggleGhostPlayback();
                                return "Ghost playback toggled";
                            });
}
