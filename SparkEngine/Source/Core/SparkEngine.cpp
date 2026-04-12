/**
 * @file SparkEngine.cpp
 * @brief SparkEngine executable entry point - loads game modules dynamically
 *
 * SparkEngine is the runtime host. It creates the window, initializes engine
 * systems (graphics, input, timer), then loads game modules via ModuleManager.
 * Modules implement IModule (or the legacy IGameModule) and provide all
 * game-specific logic.
 *
 * Architecture: Engine (exe) -> loads -> Module DLLs (via ModuleManager)
 * Similar to Unreal Engine's module loading or Unity's player runtime.
 */

#include "SparkEngine.h"
#include "Platform.h"

// On Windows, framework.h must come before any header that uses Win32 types
// (HINSTANCE, HMODULE, HWND, etc.) because it pulls in <windows.h>.
#ifdef SPARK_PLATFORM_WINDOWS
#include "framework.h"
#endif

// ============================================================================
// Common includes (shared between all platforms)
// ============================================================================
#include "ModuleManager.h"
#include "EngineContext.h"
#include "EngineSettings.h"
#include "EngineConsoleCommands.h"
#include "Engine/Events/EventSystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/Coroutine/CoroutineScheduler.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/GraphicsConsoleCommands.h"
#include "Input/InputManager.h"
#include "Audio/AudioEngine.h"
#include "Audio/AudioBackendFactory.h"
#include "Utils/Timer.h"
#include "Utils/SparkConsole.h"
#include "Utils/ConsoleProcessManager.h"
#include "EngineSetup.h"
#include "AssetIntegration.h"
#include "GameplaySystemLifecycle.h"
// Subsystems still referenced by platform entry points
#include "Graphics/WeatherSystem.h"
#include "Engine/World/TimeOfDaySystem.h"
#include "Engine/UI/UISystem.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Engine/Modding/ModSystem.h"
#include "ModuleHotReload.h"
#include "Graphics/Neural/NeuralInference.h"
#include "Utils/DebugHookManager.h"
#include "Utils/Logger.h"
#include "Utils/JobSystem.h"
#include "Utils/FreezeDetector.h"
#include "Utils/DeadlockDetector.h"
#include "Utils/HitchDetector.h"
#include "Utils/AssetStallDetector.h"
#include "Utils/NetworkHealthMonitor.h"
#include "Utils/GPUResourceLeakDetector.h"
#include "Utils/InvalidStateDetector.h"
#include "FixedTimestepAccumulator.h"
#include "Engine/Networking/ClientPrediction.h"
#include "Engine/Networking/ConnectionScopeFilter.h"
#ifdef ENABLE_NETWORKING
#include "Engine/Networking/AreaServer.h"
#include "Engine/Networking/WorldServer.h"
#include "Engine/Networking/DedicatedServer.h"
#endif
#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
#include "Physics/PhysicsSystem.h"
#endif

#include <atomic>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <thread>

// CrashHandler is cross-platform — needed by SetupCrashHandler() which is called
// from both Windows and Linux main() paths.
#include "Utils/CrashHandler.h"

// Platform-specific includes
#ifdef SPARK_PLATFORM_WINDOWS
#include "Utils/Assert.h"
#include "Utils/SparkError.h"
#include "Utils/Validate.h"
#include "Utils/DeltaSmoother.h"
#include "Utils/D3DUtils.h"
#include "Utils/LocalFileCache.h"
#else
#include <csignal>
#include <cstring>
#ifdef SPARK_SDL2_AVAILABLE
#include <SDL.h>
#endif
#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// Common globals (shared between all platforms)
// ============================================================================
std::unique_ptr<GraphicsEngine> g_graphics;
std::unique_ptr<InputManager> g_input;
std::unique_ptr<Timer> g_timer;
std::unique_ptr<Spark::EventBus> g_eventBus;
std::unique_ptr<ModuleManager> g_moduleManager;
std::unique_ptr<AudioEngine> g_audioEngine;
std::unique_ptr<Spark::Audio::IAudioBackend> g_audioBackend;
std::unique_ptr<Spark::ModuleHotReloadManager> g_moduleHotReload;
#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
std::unique_ptr<PhysicsSystem> g_physicsOwned;
#endif

// ============================================================================
// Common helper functions (shared across all startup paths)
// ============================================================================

// LogMissingModuleWarnings, InitDebugSystems, InitGameplaySystems, UpdateGameplaySystems,
// UpdateDebugSystems, ShutdownGameplaySystems, ShutdownDebugSystems — all moved to
// GameplaySystemLifecycle.cpp to reduce this file's size.

// Remaining functions below: InitPhysics, InitConsole, ShutdownPhysics, ShutdownEngine
// (engine lifecycle that depends on globals defined in this file)


void InitPhysics()
{
#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
    ASSERT_NOT_NULL(EngineContext::Get());
    g_physicsOwned = std::make_unique<PhysicsSystem>();
    EngineContext::Get()->SetPhysics(g_physicsOwned.get());
    if (g_graphics)
        g_graphics->SetPhysicsSystem(g_physicsOwned.get());
    if (g_eventBus)
        g_physicsOwned->SetEventBus(g_eventBus.get());
#endif
}

/**
 * @brief Initialize the console subsystem and all dependent debug/gameplay systems.
 *
 * Must be called after EngineContext is set up (for EventBus, Physics, etc.)
 * but before module loading (so modules can register console commands).
 * ConsoleProcessManager launches the SparkConsole.exe subprocess and owns the
 * stdin/stdout pipe used for command I/O.
 */
void InitConsole()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    console.Initialize();
    console.LogSuccess("Spark Engine runtime initialized");

    if (!Spark::ConsoleProcessManager::GetInstance().Initialize())
    {
        console.LogWarning("ConsoleProcessManager failed to initialize — SparkConsole subprocess unavailable");
    }
    InitDebugSystems();
    InitGameplaySystems();

    // Publish EngineStartEvent — all systems initialized
    if (g_eventBus)
    {
        g_eventBus->Publish(Spark::EngineStartEvent{});
    }

    SPARK_DEBUG_HOOK(EnginePostInit, 0, 0.0f);
}

void ShutdownPhysics()
{
#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
    if (g_physicsOwned)
    {
        g_physicsOwned->Shutdown();
        g_physicsOwned.reset();
    }
#endif
}

/**
 * @brief Common engine shutdown sequence shared by all startup paths.
 *
 * Shuts down gameplay/debug systems, console, modules, audio, physics,
 * and engine context in the correct order.
 */
void ShutdownEngine()
{
    // Stop the freeze detector first — we're intentionally tearing down,
    // don't let the watchdog interpret shutdown delays as a freeze.
    Spark::FreezeDetector::GetInstance().Stop();

    SPARK_DEBUG_HOOK(EnginePreShutdown, GetGameplayFrameCount(), 0.0f);

    // Publish EngineShutdownEvent before tearing down systems
    if (g_eventBus)
    {
        g_eventBus->Publish(Spark::EngineShutdownEvent{});
    }

    ShutdownGameplaySystems();
    ShutdownDebugSystems();

    if (g_moduleManager)
    {
        g_moduleManager->ShutdownAll();

        // Clear console commands and EventBus channels BEFORE dlclose()
        // unmaps module code. Command handlers and ChannelOf<E> vtables
        // live in the .so — destroying them after unload segfaults.
        Spark::SimpleConsole::GetInstance().Shutdown();
        Spark::ConsoleProcessManager::GetInstance().Shutdown();
        if (g_eventBus)
            g_eventBus->ClearAll();

        const bool shouldSkipModuleUnload =
#ifndef _WIN32
            (EngineContext::Get() != nullptr && EngineContext::Get()->IsHeadless());
#else
            false;
#endif

        if (shouldSkipModuleUnload)
        {
            // Linux/headless teardown currently hits a late-shutdown crash path
            // when module-owned callbacks/channels are destroyed after dlclose().
            // Keep modules mapped until process exit in this mode.
            g_moduleManager.release();
        }
        else
        {
            g_moduleManager->UnloadAll();
            g_moduleManager.reset();
        }
    }
    else
    {
        Spark::SimpleConsole::GetInstance().Shutdown();
        Spark::ConsoleProcessManager::GetInstance().Shutdown();
    }

    g_audioEngine.reset();
    ShutdownPhysics();

    // Shut down the job system after all subsystems that submit jobs
    Spark::JobSystem::Get().Shutdown();

    EngineContext::ResetOwned();
    g_eventBus.reset();
    g_input.reset();
    g_graphics.reset();
    g_timer.reset();

    SPARK_DEBUG_HOOK(EnginePostShutdown, GetGameplayFrameCount(), 0.0f);
    Spark::DebugHookManager::GetInstance().Clear();
}

// Test automation: exit after N frames (0 = run indefinitely).
// Parsed from -test-frames N on the command line (both platforms).
int g_testFrameLimit = 0;

// Window size override from command line (-window-size WxH).
// 0 means use default from EngineSettings.
int g_windowWidthOverride = 0;
int g_windowHeightOverride = 0;


// ===================================================================================
//                       Cross-platform helpers
// ===================================================================================

/**
 * @brief Configure and install the crash handler from EngineSettings + env vars.
 *
 * Settings are read from [CrashReporting] in settings.ini, with env var overrides:
 *   SPARK_GITHUB_REPO, SPARK_GITHUB_TOKEN, SPARK_CRASH_PROXY_URL, SPARK_CRASH_UPLOAD_URL
 */
void SetupCrashHandler()
{
    const auto& cr = EngineSettings::GetInstance().CrashReporting();

    CrashConfig crashCfg{};
    crashCfg.dumpPrefix = L"SparkCrash";
    crashCfg.captureScreenshot = cr.captureScreenshot;
    crashCfg.captureSystemInfo = cr.captureSystemInfo;
    crashCfg.captureAllThreads = cr.captureAllThreads;
    crashCfg.zipBeforeUpload = true;
    crashCfg.triggerCrashOnAssert = false;
    crashCfg.connectTimeoutSeconds = cr.timeoutSeconds;
    crashCfg.enableCrashReporting = cr.enabled;
    crashCfg.requireConsent = cr.requireConsent;
    crashCfg.headlessMode = cr.headlessMode;
    crashCfg.promptUserDescription = cr.promptUserDescription;
    crashCfg.allowScreenshotRefusal = cr.allowScreenshotRefusal;
    crashCfg.githubLabels = cr.githubLabels;
    crashCfg.githubAttachDump = cr.attachDump;
    crashCfg.smtpUser = cr.smtpUser;
    crashCfg.smtpPass = cr.smtpPass;
    crashCfg.emailTo = cr.emailTo;
    crashCfg.emailFrom = cr.emailFrom;

    // Settings file / local override values
    crashCfg.uploadURL = cr.uploadURL;
    crashCfg.proxyURL = cr.proxyURL;
    crashCfg.githubRepo = cr.githubRepo;
    crashCfg.githubToken = cr.githubToken;

    // Env var overrides (take precedence over settings file / local override)
    const char* envRepo = std::getenv("SPARK_GITHUB_REPO");
    const char* envToken = std::getenv("SPARK_GITHUB_TOKEN");
    if (envRepo && envToken)
    {
        crashCfg.githubRepo = envRepo;
        crashCfg.githubToken = envToken;
    }
    const char* envProxy = std::getenv("SPARK_CRASH_PROXY_URL");
    if (envProxy)
        crashCfg.proxyURL = envProxy;
    const char* envUpload = std::getenv("SPARK_CRASH_UPLOAD_URL");
    if (envUpload)
        crashCfg.uploadURL = envUpload;
    const char* envSmtpUser = std::getenv("SPARK_SMTP_USER");
    if (envSmtpUser)
        crashCfg.smtpUser = envSmtpUser;
    const char* envSmtpPass = std::getenv("SPARK_SMTP_PASS");
    if (envSmtpPass)
        crashCfg.smtpPass = envSmtpPass;
    const char* envEmailTo = std::getenv("SPARK_CRASH_EMAIL_TO");
    if (envEmailTo)
        crashCfg.emailTo = envEmailTo;
    const char* envHeadless = std::getenv("SPARK_CRASH_HEADLESS");
    if (envHeadless && std::string(envHeadless) == "1")
        crashCfg.headlessMode = true;

    // Auto-detect CI environments — skip dialogs
    if (std::getenv("CI") || std::getenv("GITHUB_ACTIONS") || std::getenv("JENKINS_URL"))
        crashCfg.headlessMode = true;

    InstallCrashHandler(crashCfg);
}
