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
#include "EngineRuntime.h"
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
#include "Utils/DaemonLifecycle.h"
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
// Subsystem ownership lives in the EngineRuntime struct (see EngineRuntime.h).
// Access via GetEngineRuntime().graphics, .input, .timer, .eventBus, etc.

// ============================================================================
// Common helper functions (shared across all startup paths)
// ============================================================================

// LogMissingModuleWarnings, InitDebugSystems, InitGameplaySystems, UpdateGameplaySystems,
// UpdateDebugSystems, ShutdownGameplaySystems, ShutdownDebugSystems — all moved to
// GameplaySystemLifecycle.cpp to reduce this file's size.

// Remaining functions below: InitPhysics, InitConsole, ShutdownPhysics, ShutdownEngine
// (engine lifecycle that depends on globals defined in this file)

// Forward declaration of globals defined later in this file so InitConsole
// can read them. Keeps the definition near the other globals at the bottom
// for documentation grouping.
extern bool g_noSubprocess;
extern bool g_minimalInit;

void InitPhysics()
{
#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
    ASSERT_NOT_NULL(EngineContext::Get());
    auto& rt = GetEngineRuntime();
    rt.physics = std::make_unique<PhysicsSystem>();
    EngineContext::Get()->SetPhysics(rt.physics.get());
    if (rt.graphics)
        rt.graphics->SetPhysicsSystem(rt.physics.get());
    if (rt.eventBus)
        rt.physics->SetEventBus(rt.eventBus.get());
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
    // Progress breadcrumbs via SPARK_LOG_INFO (routed through Logger's
    // stderr sink) rather than SimpleConsole::LogInfo (which only writes
    // to an in-memory buffer and is invisible to operators running under
    // Wine-in-terminal). These make it possible to tell from outside the
    // engine process exactly how far InitConsole progressed on a run that
    // crashed mid-init.
    SPARK_LOG_INFO(Spark::LogCategory::Core, "InitConsole: SimpleConsole::Initialize");
    auto& console = Spark::SimpleConsole::GetInstance();
    console.Initialize();
    console.LogSuccess("Spark Engine runtime initialized");

    // Subprocess spawn: optional. On a gVisor-class sandbox every Wine
    // process has to survive the gs.base race independently, so `-no-subprocess`
    // avoids the second Wine process entirely. In-process console (SimpleConsole)
    // still works; only the standalone SparkConsole.exe UI is skipped.
    if (!g_noSubprocess)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "InitConsole: ConsoleProcessManager::Initialize");
        if (!Spark::ConsoleProcessManager::GetInstance().Initialize())
        {
            console.LogWarning("ConsoleProcessManager failed to initialize — SparkConsole subprocess unavailable");
        }
    }
    else
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "InitConsole: ConsoleProcessManager skipped (-no-subprocess)");
    }
    if (!g_minimalInit)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "InitConsole: InitDebugSystems");
        InitDebugSystems();
        SPARK_LOG_INFO(Spark::LogCategory::Core, "InitConsole: InitGameplaySystems");
        InitGameplaySystems();

        // Opt-in daemon wiring. Reads spark.daemon.enabled CVar; if set and a
        // live daemon is reachable on the socket path, attaches the daemon's
        // ShaderService to the process-wide ShaderDiskCache so compiled
        // bytecode is shared across engine/editor/tool instances.
        SPARK_LOG_INFO(Spark::LogCategory::Core, "InitConsole: InitializeDaemonLifecycle");
        Spark::Daemon::InitializeDaemonLifecycle();
    }
    else
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core,
                       "InitConsole: InitDebugSystems + InitGameplaySystems skipped (-minimal-init)");
    }

    // Publish EngineStartEvent — all systems initialized
    if (auto& eventBus = GetEngineRuntime().eventBus)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "InitConsole: Publishing EngineStartEvent");
        eventBus->Publish(Spark::EngineStartEvent{});
    }

    SPARK_LOG_INFO(Spark::LogCategory::Core, "InitConsole: complete");
    SPARK_DEBUG_HOOK(EnginePostInit, 0, 0.0f);
}

void ShutdownPhysics()
{
#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
    auto& rt = GetEngineRuntime();
    if (rt.physics)
    {
        rt.physics->Shutdown();
        rt.physics.reset();
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

    auto& rt = GetEngineRuntime();

    // Publish EngineShutdownEvent before tearing down systems
    if (rt.eventBus)
    {
        rt.eventBus->Publish(Spark::EngineShutdownEvent{});
    }

    // Tear down the daemon wiring before ShaderDiskCache is destroyed so the
    // cache never holds a dangling client pointer during gameplay shutdown.
    Spark::Daemon::ShutdownDaemonLifecycle();

    ShutdownGameplaySystems();
    ShutdownDebugSystems();

    if (rt.moduleManager)
    {
        rt.moduleManager->ShutdownAll();

        // Clear console commands and EventBus channels BEFORE dlclose()
        // unmaps module code. Command handlers and ChannelOf<E> vtables
        // live in the .so — destroying them after unload segfaults.
        Spark::SimpleConsole::GetInstance().Shutdown();
        Spark::ConsoleProcessManager::GetInstance().Shutdown();
        if (rt.eventBus)
            rt.eventBus->ClearAll();

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
            rt.moduleManager.release();
        }
        else
        {
            rt.moduleManager->UnloadAll();
            rt.moduleManager.reset();
        }
    }
    else
    {
        Spark::SimpleConsole::GetInstance().Shutdown();
        Spark::ConsoleProcessManager::GetInstance().Shutdown();
    }

    rt.audioEngine.reset();
    ShutdownPhysics();

    // Shut down the job system after all subsystems that submit jobs
    Spark::JobSystem::Get().Shutdown();

    EngineContext::ResetOwned();
    rt.eventBus.reset();
    rt.input.reset();
    rt.graphics.reset();
    rt.timer.reset();

    SPARK_DEBUG_HOOK(EnginePostShutdown, GetGameplayFrameCount(), 0.0f);
    Spark::DebugHookManager::GetInstance().Clear();
}

// Test automation: exit after N frames (0 = run indefinitely).
// Parsed from -test-frames N on the command line (both platforms).
int g_testFrameLimit = 0;

// JobSystem thread pool size override from command line (-threads N) or
// SPARK_MAX_WORKER_THREADS env var. 0 = use the default
// hardware_concurrency - 1. Primarily intended for running the engine
// under Wine on a sandbox where every extra worker thread is another
// roll of the dice against the gs.base race documented in
// .claude/knowledge/wine-gvisor-root-cause-found-2026-04-14.md — `-threads 1`
// minimises the number of Wine worker threads and maximises the chance
// of the engine reaching its main loop on a flaky sandbox run.
uint32_t g_maxWorkerThreads = 0;

// Skip the SparkConsole.exe subprocess spawn (see ConsoleProcessManager).
// Parsed from `-no-subprocess` on both platforms. Subprocess launches
// are the single biggest "extra Wine process" the engine creates after
// JobSystem initialization, and under a gVisor sandbox each new Wine
// process has to survive the gs.base race independently. Developers
// can pass this flag to minimise the chance of losing the race at the
// cost of losing access to the standalone SparkConsole UI. The engine-
// side in-process console (SimpleConsole) still works.
bool g_noSubprocess = false;

// Minimal init mode: skip everything non-essential to reaching the main
// loop. Parsed from `-minimal-init` on both platforms. When set:
//   * InitDebugSystems is a no-op (no LifecycleCompositionRoot registration).
//   * Detector singletons (Freeze/Deadlock/Hitch/AssetStall/NetworkHealth/
//     GPUResourceLeak/InvalidState/MemoryMonitor) are not registered or
//     started.
//   * LoadHeadlessModules skips dlopen'ing game modules entirely.
//   * The engine still initializes Timer, EventBus, EngineContext,
//     FileCache, JobSystem (capped at -threads N), SaveSystem, and
//     SimpleConsole — enough to reach RunHeadlessWindows's main loop.
//
// Primary use case: validating that the engine's core init path reaches
// the main loop on a flaky Wine sandbox where the gs.base race kills
// every extra thread / subprocess the normal init path would spawn.
// This is also what the engine's startup path looked like in earlier
// sessions before the broad wiring of gameplay and debug subsystems —
// the user ran into "it all worked flawlessly" historically and the
// breakage is specifically accumulated subsystem registrations.
bool g_minimalInit = false;

// Skip JobSystem initialization entirely, so the engine runs everything
// on the main thread and spawns zero worker threads. Parsed from
// `-no-jobsystem` on both platforms.
//
// Rationale: under a gVisor-backed Wine sandbox, every new Windows
// thread rolls the dice on the gs.base race (see
// tools/gvisor-wine-shim.c). `-threads 1` still spawns one worker,
// which is enough to trigger the race. `-no-jobsystem` eliminates
// worker threads completely, and code paths that dispatch work via
// JobSystem::Get().Dispatch(...) fall back to inline execution on
// the main thread because JobSystem::IsInitialized() returns false.
bool g_noJobSystem = false;

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
