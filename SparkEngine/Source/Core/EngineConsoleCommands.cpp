/**
 * @file EngineConsoleCommands.cpp
 * @brief Engine-wide console command registration
 *
 * Consolidates all non-graphics console commands: modules, save system,
 * physics, audio, and engine architecture queries. Each subsystem has its
 * own static registration function to keep individual functions manageable.
 */

#include "EngineConsoleCommands.h"
#include "EngineContext.h"
#include "AssetIntegration.h"
#include "ModuleManager.h"
#include "ModuleHotReload.h"
#include "Audio/AudioEngine.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Utils/SparkConsole.h"
#include "EngineSetup.h"

#include <sstream>
#include <string>

namespace Spark
{

    // ============================================================================
    // Module commands
    // ============================================================================

    static void RegisterModuleCommands(SimpleConsole& console, ModuleManager* moduleManager)
    {
        console.RegisterCommand(
            "module_info",
            [moduleManager](const std::vector<std::string>&) -> std::string
            {
                if (!moduleManager || !moduleManager->HasModules())
                    return "No modules loaded";
                std::stringstream ss;
                ss << "=== Loaded Modules (" << moduleManager->GetModuleCount() << ") ===\n";
                auto* primary = moduleManager->GetPrimaryModule();
                if (primary)
                {
                    auto info = primary->GetModuleInfo();
                    ss << "Primary: " << info.name << " v" << info.version << " (loadOrder=" << info.loadOrder << ")\n";
                }
                return ss.str();
            },
            "Show loaded module info");

        console.RegisterCommand(
            "module_reload",
            [moduleManager](const std::vector<std::string>& args) -> std::string
            {
                if (!moduleManager || !moduleManager->HasModules())
                    return "No modules loaded";
                if (!EngineContext::Get())
                    return "Engine context not available";

                std::string name;
                if (!args.empty())
                {
                    name = args[0];
                }
                else
                {
                    auto* primary = moduleManager->GetPrimaryModule();
                    if (!primary)
                        return "No primary module to reload";
                    name = primary->GetModuleInfo().name;
                }

                if (moduleManager->ReloadModule(name, EngineContext::Get()))
                    return "Module '" + name + "' reloaded successfully";
                return "Failed to reload module '" + name + "'";
            },
            "Hot-reload a module (usage: module_reload [name])");
    }

    // ============================================================================
    // SaveSystem commands
    // ============================================================================

    static void RegisterSaveCommands(SimpleConsole& console)
    {
        console.RegisterCommand(
            "save_list",
            [](const std::vector<std::string>&) -> std::string
            {
                try
                {
                    return SaveSystem::GetInstance().Console_ListSaves();
                }
                catch (...)
                {
                    return "SaveSystem not available";
                }
            },
            "List all save slots", "Save");

        console.RegisterCommand(
            "save_info",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: save_info <slot_name>";
                try
                {
                    return SaveSystem::GetInstance().Console_GetSaveInfo(args[0]);
                }
                catch (...)
                {
                    return "SaveSystem not available";
                }
            },
            "Show details for a save slot", "Save");
    }

    // ============================================================================
    // PhysicsSystem commands
    // ============================================================================

    static void RegisterPhysicsCommands(SimpleConsole& console)
    {
        console.RegisterCommand(
            "physics_metrics",
            [](const std::vector<std::string>&) -> std::string
            {
                if (!EngineContext::Get())
                    return "Engine context not available";
                auto* physics = EngineContext::Get()->GetPhysics();
                if (!physics)
                    return "Physics system not available";
                auto m = physics->Console_GetMetrics();
                std::stringstream ss;
                ss << "=== Physics Metrics ===\n"
                   << "Active Bodies: " << m.activeRigidBodies << "/" << m.totalRigidBodies << "\n"
                   << "Constraints: " << m.activeConstraints << "\n"
                   << "Collision Pairs: " << m.collisionPairs << "\n"
                   << "Sim Time: " << m.simulationTime << "ms\n"
                   << "Substeps: " << m.substeps << "\n";
                return ss.str();
            },
            "Display physics performance metrics", "Physics");

        console.RegisterCommand(
            "physics_list",
            [](const std::vector<std::string>&) -> std::string
            {
                if (!EngineContext::Get())
                    return "Engine context not available";
                auto* physics = EngineContext::Get()->GetPhysics();
                if (!physics)
                    return "Physics system not available";
                return physics->Console_ListBodies();
            },
            "List all physics bodies", "Physics");

        console.RegisterCommand(
            "physics_body_info",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: physics_body_info <name>";
                if (!EngineContext::Get())
                    return "Engine context not available";
                auto* physics = EngineContext::Get()->GetPhysics();
                if (!physics)
                    return "Physics system not available";
                return physics->Console_GetBodyInfo(args[0]);
            },
            "Get detailed info about a physics body", "Physics");

        console.RegisterCommand(
            "physics_gravity",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 3)
                    return "Usage: physics_gravity <x> <y> <z>";
                if (!EngineContext::Get())
                    return "Engine context not available";
                auto* physics = EngineContext::Get()->GetPhysics();
                if (!physics)
                    return "Physics system not available";
                try
                {
                    float x = std::stof(args[0]), y = std::stof(args[1]), z = std::stof(args[2]);
                    physics->Console_SetGravity(x, y, z);
                    return "Gravity set to (" + args[0] + ", " + args[1] + ", " + args[2] + ")";
                }
                catch (const std::exception&)
                {
                    return "Invalid number format. Usage: physics_gravity <x> <y> <z>";
                }
            },
            "Set world gravity vector", "Physics");

        console.RegisterCommand(
            "physics_create",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 5)
                    return "Usage: physics_create <name> <type> <x> <y> <z>";
                if (!EngineContext::Get())
                    return "Engine context not available";
                auto* physics = EngineContext::Get()->GetPhysics();
                if (!physics)
                    return "Physics system not available";
                try
                {
                    float x = std::stof(args[2]), y = std::stof(args[3]), z = std::stof(args[4]);
                    bool ok = physics->Console_CreateBody(args[0], args[1], x, y, z);
                    return ok ? "Body '" + args[0] + "' created" : "Failed to create body (invalid type?)";
                }
                catch (const std::exception&)
                {
                    return std::string("Invalid numeric argument");
                }
            },
            "Create a physics body (type: static/kinematic/dynamic)", "Physics");

        console.RegisterCommand(
            "physics_remove",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: physics_remove <name>";
                if (!EngineContext::Get())
                    return "Engine context not available";
                auto* physics = EngineContext::Get()->GetPhysics();
                if (!physics)
                    return "Physics system not available";
                bool ok = physics->Console_RemoveBody(args[0]);
                return ok ? "Body '" + args[0] + "' removed" : "Body not found";
            },
            "Remove a physics body", "Physics");

        console.RegisterCommand(
            "physics_set",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 3)
                    return "Usage: physics_set <name> <property> <value>";
                if (!EngineContext::Get())
                    return "Engine context not available";
                auto* physics = EngineContext::Get()->GetPhysics();
                if (!physics)
                    return "Physics system not available";
                try
                {
                    physics->Console_SetBodyProperty(args[0], args[1], std::stof(args[2]));
                    return args[1] + " set to " + args[2] + " on '" + args[0] + "'";
                }
                catch (const std::exception&)
                {
                    return std::string("Invalid numeric argument");
                }
            },
            "Set body property (mass/friction/restitution/linearDamping/angularDamping)", "Physics");

        console.RegisterCommand(
            "physics_force",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 4)
                    return "Usage: physics_force <name> <x> <y> <z>";
                if (!EngineContext::Get())
                    return "Engine context not available";
                auto* physics = EngineContext::Get()->GetPhysics();
                if (!physics)
                    return "Physics system not available";
                try
                {
                    physics->Console_ApplyForce(args[0], std::stof(args[1]), std::stof(args[2]), std::stof(args[3]));
                    return "Force applied to '" + args[0] + "'";
                }
                catch (const std::exception&)
                {
                    return std::string("Invalid numeric argument");
                }
            },
            "Apply force to a physics body", "Physics");

        console.RegisterCommand(
            "physics_impulse",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 4)
                    return "Usage: physics_impulse <name> <x> <y> <z>";
                if (!EngineContext::Get())
                    return "Engine context not available";
                auto* physics = EngineContext::Get()->GetPhysics();
                if (!physics)
                    return "Physics system not available";
                try
                {
                    physics->Console_ApplyImpulse(args[0], std::stof(args[1]), std::stof(args[2]), std::stof(args[3]));
                    return "Impulse applied to '" + args[0] + "'";
                }
                catch (const std::exception&)
                {
                    return std::string("Invalid numeric argument");
                }
            },
            "Apply impulse to a physics body", "Physics");

        console.RegisterCommand(
            "physics_debug",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: physics_debug <on|off>";
                if (!EngineContext::Get())
                    return "Engine context not available";
                auto* physics = EngineContext::Get()->GetPhysics();
                if (!physics)
                    return "Physics system not available";
                bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                physics->Console_EnableDebugDraw(enable);
                return enable ? "Physics debug draw enabled" : "Physics debug draw disabled";
            },
            "Toggle physics debug overlay", "Physics");

        console.RegisterCommand(
            "physics_pause",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: physics_pause <on|off>";
                if (!EngineContext::Get())
                    return "Engine context not available";
                auto* physics = EngineContext::Get()->GetPhysics();
                if (!physics)
                    return "Physics system not available";
                bool pause = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                physics->Console_PausePhysics(pause);
                return pause ? "Physics simulation paused" : "Physics simulation resumed";
            },
            "Pause/resume physics simulation", "Physics");

        console.RegisterCommand(
            "physics_timestep",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: physics_timestep <seconds>";
                if (!EngineContext::Get())
                    return "Engine context not available";
                auto* physics = EngineContext::Get()->GetPhysics();
                if (!physics)
                    return "Physics system not available";
                try
                {
                    float ts = std::stof(args[0]);
                    physics->Console_SetTimeStep(ts);
                    return "Physics timestep set to " + args[0] + "s";
                }
                catch (const std::exception&)
                {
                    return std::string("Invalid numeric argument");
                }
            },
            "Set physics fixed timestep", "Physics");

        console.RegisterCommand(
            "physics_raycast",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 7)
                    return "Usage: physics_raycast <ox> <oy> <oz> <dx> <dy> <dz> <maxDist>";
                if (!EngineContext::Get())
                    return "Engine context not available";
                auto* physics = EngineContext::Get()->GetPhysics();
                if (!physics)
                    return "Physics system not available";
                try
                {
                    return physics->Console_Raycast(std::stof(args[0]), std::stof(args[1]), std::stof(args[2]),
                                                    std::stof(args[3]), std::stof(args[4]), std::stof(args[5]),
                                                    std::stof(args[6]));
                }
                catch (const std::exception&)
                {
                    return std::string("Invalid numeric argument");
                }
            },
            "Perform a physics raycast", "Physics");

        console.RegisterCommand(
            "physics_reset",
            [](const std::vector<std::string>&) -> std::string
            {
                if (!EngineContext::Get())
                    return "Engine context not available";
                auto* physics = EngineContext::Get()->GetPhysics();
                if (!physics)
                    return "Physics system not available";
                physics->Console_Reset();
                return "Physics world reset";
            },
            "Reset physics world to initial state", "Physics");
    }

    // ============================================================================
    // AudioEngine commands
    // ============================================================================

    static void RegisterAudioCommands(SimpleConsole& console, AudioEngine* audioEngine)
    {
        console.RegisterCommand(
            "audio_master_volume",
            [audioEngine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: audio_master_volume <0.0-1.0>";
                if (!audioEngine)
                    return "Audio engine not available";
                try
                {
                    audioEngine->Console_SetMasterVolume(std::stof(args[0]));
                    return "Master volume set to " + args[0];
                }
                catch (const std::exception&)
                {
                    return "Invalid number format. Usage: audio_master_volume <0.0-1.0>";
                }
            },
            "Set master audio volume", "Audio");

        console.RegisterCommand(
            "audio_sfx_volume",
            [audioEngine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: audio_sfx_volume <0.0-1.0>";
                if (!audioEngine)
                    return "Audio engine not available";
                try
                {
                    audioEngine->Console_SetSFXVolume(std::stof(args[0]));
                    return "SFX volume set to " + args[0];
                }
                catch (const std::exception&)
                {
                    return std::string("Invalid number format");
                }
            },
            "Set SFX volume", "Audio");

        console.RegisterCommand(
            "audio_music_volume",
            [audioEngine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: audio_music_volume <0.0-1.0>";
                if (!audioEngine)
                    return "Audio engine not available";
                try
                {
                    audioEngine->Console_SetMusicVolume(std::stof(args[0]));
                    return "Music volume set to " + args[0];
                }
                catch (const std::exception&)
                {
                    return std::string("Invalid number format");
                }
            },
            "Set music volume", "Audio");

        console.RegisterCommand(
            "audio_3d",
            [audioEngine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: audio_3d <on|off>";
                if (!audioEngine)
                    return "Audio engine not available";
                bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                audioEngine->Console_Set3DAudio(enable);
                return enable ? "3D audio enabled" : "3D audio disabled";
            },
            "Toggle 3D spatial audio", "Audio");

        console.RegisterCommand(
            "audio_play_test",
            [audioEngine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: audio_play_test <sound_name> [3d]";
                if (!audioEngine)
                    return "Audio engine not available";
                bool is3D = (args.size() > 1 && (args[1] == "3d" || args[1] == "true"));
                uint32_t id = audioEngine->Console_PlayTestSound(args[0], is3D);
                return id > 0 ? "Playing sound (ID: " + std::to_string(id) + ")" : "Failed to play sound";
            },
            "Play a test sound", "Audio");

        console.RegisterCommand(
            "audio_stop",
            [audioEngine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: audio_stop <source_id>";
                if (!audioEngine)
                    return "Audio engine not available";
                try
                {
                    audioEngine->Console_StopSound(static_cast<uint32_t>(std::stoul(args[0])));
                    return std::string("Sound stopped");
                }
                catch (const std::exception&)
                {
                    return std::string("Invalid source ID");
                }
            },
            "Stop a playing sound by ID", "Audio");

        console.RegisterCommand(
            "audio_stop_all",
            [audioEngine](const std::vector<std::string>&) -> std::string
            {
                if (!audioEngine)
                    return "Audio engine not available";
                audioEngine->Console_StopAllSounds();
                return "All sounds stopped";
            },
            "Stop all playing sounds", "Audio");

        console.RegisterCommand(
            "audio_list",
            [audioEngine](const std::vector<std::string>&) -> std::string
            {
                if (!audioEngine)
                    return "Audio engine not available";
                return audioEngine->Console_ListSounds();
            },
            "List all loaded sounds", "Audio");

        console.RegisterCommand(
            "audio_metrics",
            [audioEngine](const std::vector<std::string>&) -> std::string
            {
                if (!audioEngine)
                    return "Audio engine not available";
                auto m = audioEngine->Console_GetMetrics();
                std::stringstream ss;
                ss << "=== Audio Metrics ===\n"
                   << "Active Sources: " << m.activeSources << "/" << m.totalSources << "\n"
                   << "Loaded Sounds: " << m.loadedSounds << "\n"
                   << "Memory: " << (m.memoryUsage / 1024) << " KB\n";
                return ss.str();
            },
            "Display audio performance metrics", "Audio");

        console.RegisterCommand(
            "audio_reset",
            [audioEngine](const std::vector<std::string>&) -> std::string
            {
                if (!audioEngine)
                    return "Audio engine not available";
                audioEngine->Console_ResetToDefaults();
                return "Audio settings reset to defaults";
            },
            "Reset audio settings to defaults", "Audio");

        console.RegisterCommand(
            "audio_listener_pos",
            [audioEngine](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 3)
                    return "Usage: audio_listener_pos <x> <y> <z>";
                if (!audioEngine)
                    return "Audio engine not available";
                try
                {
                    audioEngine->Console_SetListenerPosition(std::stof(args[0]), std::stof(args[1]),
                                                             std::stof(args[2]));
                    return std::string("Listener position set");
                }
                catch (const std::exception&)
                {
                    return std::string("Invalid numeric argument");
                }
            },
            "Set 3D audio listener position", "Audio");

        console.RegisterCommand(
            "audio_doppler",
            [audioEngine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: audio_doppler <scale>";
                if (!audioEngine)
                    return "Audio engine not available";
                try
                {
                    audioEngine->Console_SetDopplerScale(std::stof(args[0]));
                    return "Doppler scale set to " + args[0];
                }
                catch (const std::exception&)
                {
                    return std::string("Invalid numeric argument");
                }
            },
            "Set Doppler effect scale", "Audio");

        console.RegisterCommand(
            "audio_source_info",
            [audioEngine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: audio_source_info <source_id>";
                if (!audioEngine)
                    return "Audio engine not available";
                try
                {
                    return audioEngine->Console_GetSourceInfo(static_cast<uint32_t>(std::stoul(args[0])));
                }
                catch (const std::exception&)
                {
                    return std::string("Invalid source ID");
                }
            },
            "Get info about an audio source", "Audio");
    }

    // ============================================================================
    // Architecture / engine subsystem commands
    // ============================================================================

    static void RegisterArchitectureCommands(SimpleConsole& console)
    {
        console.RegisterCommand(
            "engine_subsystems",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* ctx = EngineContext::Get();
                if (!ctx)
                    return "Engine context not available";
                std::stringstream ss;
                ss << "=== Engine Subsystems ===\n"
                   << "  Registered: " << ctx->GetSubsystemCount() << "\n"
                   << "  Graphics:   " << (ctx->GetGraphics() ? "YES" : "NO") << "\n"
                   << "  Input:      " << (ctx->GetInput() ? "YES" : "NO") << "\n"
                   << "  Timer:      " << (ctx->GetTimer() ? "YES" : "NO") << "\n"
                   << "  EventBus:   " << (ctx->GetEventBus() ? "YES" : "NO") << "\n"
                   << "  Audio:      " << (ctx->GetAudio() ? "YES" : "NO") << "\n"
                   << "  Physics:    " << (ctx->GetPhysics() ? "YES" : "NO") << "\n"
                   << "  Animation:  " << (ctx->GetAnimation() ? "YES" : "NO") << "\n"
                   << "  AI:         " << (ctx->GetAI() ? "YES" : "NO") << "\n"
                   << "  Network:    " << (ctx->GetNetwork() ? "YES" : "NO") << "\n"
                   << "  SaveSystem: " << (ctx->GetSaveSystem() ? "YES" : "NO") << "\n"
                   << "  Scene:      " << (ctx->GetSceneManager() ? "YES" : "NO") << "\n"
                   << "  Scripting:  " << (ctx->GetScriptEngine() ? "YES" : "NO") << "\n"
                   << "  Coroutines: " << (ctx->GetCoroutineScheduler() ? "YES" : "NO") << "\n"
                   << "  Headless:   " << (ctx->IsHeadless() ? "YES" : "NO") << "\n";
                auto* assetReg = ctx->GetSystem<AssetRegistry>();
                ss << "  AssetReg:   " << (assetReg ? "YES" : "NO");
                if (assetReg)
                    ss << " (" << assetReg->GetAssetCount() << " assets)";
                ss << "\n";
                auto& js = JobSystem::Get();
                ss << "  JobSystem:  " << (js.IsInitialized() ? "YES" : "NO");
                if (js.IsInitialized())
                    ss << " (" << js.GetWorkerCount() << " workers)";
                ss << "\n";
                return ss.str();
            },
            "Show status of all registered engine subsystems", "Engine");

        console.RegisterCommand(
            "asset_list",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* ctx = EngineContext::Get();
                if (!ctx)
                    return "Engine context not available";
                auto* reg = ctx->GetSystem<AssetRegistry>();
                if (!reg)
                    return "Asset registry not available";
                return reg->Console_ListAssets();
            },
            "List all registered assets in the asset registry", "Engine");
    }

    // ============================================================================
    // Module hot-reload commands
    // ============================================================================

    static void RegisterHotReloadCommands(SimpleConsole& console, ModuleHotReloadManager* hotReload)
    {
        console.RegisterCommand(
            "module.hotreload.enable",
            [hotReload](const std::vector<std::string>&) -> std::string
            {
                if (!hotReload)
                    return "Module hot-reload not available";
                hotReload->SetEnabled(true);
                return "Module hot-reload enabled";
            },
            "Enable automatic module hot-reload", "Modules");

        console.RegisterCommand(
            "module.hotreload.disable",
            [hotReload](const std::vector<std::string>&) -> std::string
            {
                if (!hotReload)
                    return "Module hot-reload not available";
                hotReload->SetEnabled(false);
                return "Module hot-reload disabled";
            },
            "Disable automatic module hot-reload", "Modules");

        console.RegisterCommand(
            "module.reload",
            [hotReload](const std::vector<std::string>& args) -> std::string
            {
                if (!hotReload)
                    return "Module hot-reload not available";
                if (args.empty())
                    return "Usage: module.reload <module_name>";
                bool ok = hotReload->ForceReload(args[0]);
                return ok ? "Module '" + args[0] + "' reloaded" : "Failed to reload '" + args[0] + "'";
            },
            "Force reload a specific module (usage: module.reload <name>)", "Modules");

        console.RegisterCommand(
            "module.hotreload.status",
            [hotReload](const std::vector<std::string>&) -> std::string
            {
                if (!hotReload)
                    return "Module hot-reload not available";
                return hotReload->GetStatus();
            },
            "Show module hot-reload status and watched modules", "Modules");
    }

    // ============================================================================
    // Public API — delegates to per-subsystem registrations
    // ============================================================================

    void RegisterEngineConsoleCommands(ModuleManager* moduleManager, AudioEngine* audioEngine,
                                       ModuleHotReloadManager* hotReload)
    {
        auto& console = SimpleConsole::GetInstance();

        RegisterModuleCommands(console, moduleManager);
        RegisterSaveCommands(console);
        RegisterPhysicsCommands(console);
        RegisterAudioCommands(console, audioEngine);
        RegisterArchitectureCommands(console);
        RegisterHotReloadCommands(console, hotReload);
    }

} // namespace Spark
