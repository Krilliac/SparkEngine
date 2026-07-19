/**
 * @file SparkEngineWindowsHeadless.cpp
 * @brief Headless/dedicated-server loop (Windows).
 *
 * Split from SparkEngineWindows.cpp to keep files under the ~500-line guideline.
 * Contains RunHeadlessWindows (fixed 60 Hz server loop, no graphics/input),
 * its console allocation, Ctrl+C handling, and headless module loading.
 * Linux counterpart lives in SparkEngineLinuxHeadless.cpp. The wWinMain entry
 * point stays in SparkEngineWindows.cpp.
 */
#include "SparkEngine.h"
#include "Platform.h"
#include "framework.h"
#include "SparkEngineWindowsInternal.h"
#include "Audio/AudioEngine.h"
#include "Core/AssetValidator.h"
#include "Engine/Coroutine/CoroutineScheduler.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Engine/ECS/Components.h" // ::World — engine-owned ECS world service
#include "Engine/Events/EventSystem.h"
#include "Engine/Modding/ModSystem.h"
#include "Engine/UI/UISystem.h"
#include "Graphics/WeatherSystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/Networking/ClientPrediction.h"
#include "Engine/Networking/ConnectionScopeFilter.h"
#ifdef ENABLE_NETWORKING
#include "Engine/Networking/AreaServer.h"
#include "Engine/Networking/WorldServer.h"
#include "Engine/Networking/DedicatedServer.h"
#endif
#include "EngineConsoleCommands.h"
#include "EngineContext.h"
#include "EngineRuntime.h"
#include "EngineSetup.h"
#include "FaultIsolation.h"
#include "FixedTimestepAccumulator.h"
#include "GameplaySystemLifecycle.h"
#include "ModuleHotReload.h"
#include "ModuleManager.h"
#include "Utils/Assert.h"
#include "Utils/AssetStallDetector.h"
#include "Utils/BenchmarkFramework.h"
#include "Utils/ConsoleProcessManager.h"
#include "Utils/DeadlockDetector.h"
#include "Utils/FreezeDetector.h"
#include "Utils/GPUResourceLeakDetector.h"
#include "Utils/HitchDetector.h"
#include "Utils/InvalidStateDetector.h"
#include "Utils/LocalFileCache.h"
#include "Utils/Logger.h"
#include "Utils/NetworkHealthMonitor.h"
#include "Utils/SparkConsole.h"
#include "Utils/Timer.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <thread>

#ifdef SPARK_PLATFORM_WINDOWS
#ifdef SPARK_HEADLESS_SUPPORT

// g_headlessMode is defined in EngineContext.cpp (SparkEngineLib)
static std::atomic<bool> g_shutdownRequested{false};

/**
 * @brief Console Ctrl handler for graceful headless shutdown (Windows)
 */
static BOOL WINAPI HeadlessCtrlHandler(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_BREAK_EVENT)
    {
        g_shutdownRequested = true;
        return TRUE;
    }
    return FALSE;
}

/**
 * @brief Attach a Win32 console for headless stdout/stderr/stdin.
 *
 * AllocConsole() may fail when the process is already attached to a console
 * (normal case when invoked from a terminal, including Wine running from a
 * Linux shell) — fall through to leaving the inherited stdio alone. We only
 * rebind stdio to CONOUT$/CONIN$ when AllocConsole actually creates a new
 * console, otherwise freopen_s blocks waiting for a console we don't have.
 * Under Wine in a headless sandbox (no stdin), the stdin rebind would also
 * hang, so we try it last and tolerate failure.
 */
static void AllocHeadlessConsole()
{
    if (AllocConsole())
    {
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        freopen_s(&fp, "CONIN$", "r", stdin);
    }
    // SetConsoleCtrlHandler still works with an inherited console and is
    // the primary way we catch Ctrl+C for graceful shutdown.
    SetConsoleCtrlHandler(HeadlessCtrlHandler, TRUE);
}

/**
 * @brief Initialize headless engine context (no graphics, no input).
 *
 * Reuses the same pattern as InitEngineContext() but passes nullptr for
 * graphics and input — headless mode has no GPU or window.
 */
static bool InitHeadlessEngineContext()
{
    GetEngineRuntime().timer = std::make_unique<Timer>();
    GetEngineRuntime().eventBus = std::make_unique<Spark::EventBus>();
    EngineContext::SetOwned(std::make_unique<EngineContext>(nullptr, nullptr, GetEngineRuntime().timer.get(),
                                                            GetEngineRuntime().eventBus.get()));

    auto* ctx = EngineContext::Get();
    if (!ctx)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext is null after SetOwned — headless init aborted");
        return false;
    }

    g_fileCache = std::make_unique<Spark::LocalFileCache>();
    ctx->SetFileCache(g_fileCache.get());

    InitPhysics();

    Spark::EngineSetup::RegisterCoreSubsystems(*ctx);
    if (!g_noJobSystem)
    {
        Spark::EngineSetup::InitializeJobSystem(g_maxWorkerThreads);
    }
    else
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "-no-jobsystem: JobSystem worker threads skipped");
    }

    ctx->SetSaveSystem(&Spark::SaveSystem::GetInstance());
    ctx->SetCoroutineScheduler(&Spark::CoroutineScheduler::GetInstance());

    // ECS world service — MUST mirror InitEngineContext (windowed): game
    // modules reach the ECS only through IEngineContext::GetWorld(). Without
    // this a headless/dedicated server silently hands modules a null World —
    // TERRAFRONT pawns then get no entities, every PawnInfo read collapses to
    // pos (0,0,0) / hp 0, and region capture never sees an occupant
    // (2026-07-10 play-test: "capturing isn't working" on the dedicated
    // server). Owned by g_engineEcsWorld (SparkEngine.cpp): the shared
    // ShutdownEngine() — which RunHeadlessWindows calls — destroys it after
    // module ShutdownAll but before the DLLs are unmapped.
    extern std::unique_ptr<::World> g_engineEcsWorld;
    g_engineEcsWorld = std::make_unique<::World>();
    ctx->SetWorld(g_engineEcsWorld.get());

    return true;
}

/**
 * @brief Load modules and register console commands for headless mode.
 */
static void LoadHeadlessModules(LPWSTR lpCmdLine)
{
    GetEngineRuntime().moduleManager = std::make_unique<ModuleManager>();
    auto& console = Spark::SimpleConsole::GetInstance();

    if (LoadGameModules(*GetEngineRuntime().moduleManager, lpCmdLine))
    {
        GetEngineRuntime().moduleManager->InitializeAll(EngineContext::Get());
        console.LogSuccess("Loaded " + std::to_string(GetEngineRuntime().moduleManager->GetModuleCount()) +
                           " module(s)");
    }
    else if (!g_projectSelectorCandidates.empty())
    {
        console.LogError("Headless launch found multiple game modules but has no project selector UI.");
        console.LogError("Pass -game <dll> to choose one (candidates listed above). Running engine-only.");
        g_projectSelectorCandidates.clear();
    }
    else
    {
        console.LogWarning("No game modules found. Running engine-only headless mode.");
    }

    GetEngineRuntime().moduleHotReload = std::make_unique<Spark::ModuleHotReloadManager>();
    GetEngineRuntime().moduleHotReload->Initialize(GetEngineRuntime().moduleManager.get(), EngineContext::Get());
    GetEngineRuntime().moduleHotReload->WatchAllLoadedModules();
    GetEngineRuntime().moduleHotReload->Start();

    Spark::RegisterEngineConsoleCommands(GetEngineRuntime().moduleManager.get(), GetEngineRuntime().audioEngine.get(),
                                         GetEngineRuntime().moduleHotReload.get());
    Spark::SubsystemFaultIsolator::GetInstance().RegisterConsoleCommands();
    Assert::RegisterConsoleCommands();
}

/**
 * @brief Run the engine in headless/dedicated server mode (Windows).
 *
 * Allocates a console, initializes server-only subsystems, runs a fixed 60 Hz
 * tick loop, and shuts down cleanly on Ctrl+C.
 */
int RunHeadlessWindows(LPWSTR lpCmdLine)
{
    AllocHeadlessConsole();
    Spark::SimpleConsole::GetInstance().LogInfo("=== Spark Engine (Headless/Dedicated Server) ===");

    if (!InitHeadlessEngineContext())
        return 1;

    // Progress breadcrumbs via Logger (SimpleConsole.LogInfo only writes
    // to an in-memory buffer and is invisible to terminal Wine runs).
    SPARK_LOG_INFO(Spark::LogCategory::Core, "RunHeadlessWindows: SaveSystem::Initialize");
    if (!Spark::SaveSystem::GetInstance().Initialize("Saves"))
        Spark::SimpleConsole::GetInstance().LogWarning("SaveSystem initialization failed — save/load unavailable");
    SPARK_LOG_INFO(Spark::LogCategory::Core, "RunHeadlessWindows: SaveSystem initialized");

    InitConsole();
    SPARK_LOG_INFO(Spark::LogCategory::Core, "RunHeadlessWindows: InitConsole returned");

    // Minimal-init mode skips module loading and all detector singletons.
    // Each detector's GetInstance() is a Meyers singleton construction and
    // most of them spawn a worker thread in Start() — every one is another
    // roll of the dice against the Wine gs.base race on a gVisor sandbox.
    // The main loop can still run useful engine update ticks without any
    // of them registered.
    if (!g_minimalInit)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "RunHeadlessWindows: LoadHeadlessModules");
        LoadHeadlessModules(lpCmdLine);
        SPARK_LOG_INFO(Spark::LogCategory::Core, "RunHeadlessWindows: detector singletons");
        Spark::FreezeDetector::GetInstance().RegisterConsoleCommands();
        // Server-role watchdog: a dedicated/headless server must RECOVER from
        // stalls (RAM paging on a loaded host froze the loop 5-9s in practice),
        // never self-terminate — killing the process mid-match punishes every
        // connected client. Looser thresholds than the interactive client and
        // terminate OFF; one dump is still taken for post-mortem.
        {
            Spark::FreezeDetectorConfig serverCfg;
            serverCfg.warningThresholdSec = 10.0f;
            serverCfg.recoveryThresholdSec = 20.0f;
            serverCfg.crashThresholdSec = 60.0f;
            serverCfg.terminateOnFreeze = false;
            Spark::FreezeDetector::GetInstance().Configure(serverCfg);
        }
        Spark::FreezeDetector::GetInstance().Start();
        Spark::DeadlockDetector::GetInstance().RegisterConsoleCommands();
        Spark::HitchDetector::GetInstance().RegisterConsoleCommands();
        Spark::BenchmarkFramework::GetInstance().RegisterConsoleCommands();
        Spark::AssetStallDetector::GetInstance().RegisterConsoleCommands();
        Spark::AssetValidator::GetInstance().RegisterConsoleCommands();
        Spark::NetworkHealthMonitor::GetInstance().RegisterConsoleCommands();
        Spark::GPUResourceLeakDetector::GetInstance().RegisterConsoleCommands();
        Spark::InvalidStateDetector::GetInstance().RegisterConsoleCommands();
        Assert::RegisterConsoleCommands();
        SPARK_LOG_INFO(Spark::LogCategory::Core, "RunHeadlessWindows: detectors registered");
    }
    else
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core,
                       "RunHeadlessWindows: LoadHeadlessModules + detectors skipped (-minimal-init)");
    }

    // Fixed 60 Hz server loop
    constexpr auto TICK_INTERVAL = std::chrono::microseconds(16667);
    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("Starting headless server loop (60 Hz)...");
    console.LogInfo("Press Ctrl+C or type 'quit' to stop.");

    if (g_testFrameLimit > 0)
        console.LogInfo(std::format("Test mode: will exit after {} frames", g_testFrameLimit));

    int frameCount = 0;
    bool quitPosted = false;

    while (!g_shutdownRequested)
    {
        // Honor -test-frames N for automated smoke testing under Wine/CI.
        // Matches the behaviour already present in SparkEngineLinux.cpp's
        // RunHeadlessLinux — without this parity the Windows headless loop
        // runs forever even on -test-frames and CI jobs time out.
        if ((g_testFrameLimit > 0 && frameCount >= g_testFrameLimit) ||
            (g_testSecondsLimit > 0.0 && ExecElapsedSeconds() >= g_testSecondsLimit))
        {
            console.LogInfo(
                std::format("[TEST] Limit reached (frame {} / t={:.1f}s). Exiting.", frameCount, ExecElapsedSeconds()));
            break;
        }

        SPARK_HEARTBEAT();
        auto tickStart = std::chrono::steady_clock::now();

        float dt = GetEngineRuntime().timer ? GetEngineRuntime().timer->GetDeltaTime() : (1.0f / 60.0f);

        Spark::FixedTimestepAccumulator::GetInstance().Advance(dt);

        SPARK_GUARDED_UPDATE("Modules", "Core", {
            if (GetEngineRuntime().moduleManager && GetEngineRuntime().moduleManager->HasModules())
            {
                GetEngineRuntime().moduleManager->UpdateAll(dt);
                auto& fixedAcc = Spark::FixedTimestepAccumulator::GetInstance();
                const float fixedDt = fixedAcc.GetFixedTimestep();
                for (uint32_t i = fixedAcc.GetFixedStepCount(); i > 0; --i)
                    GetEngineRuntime().moduleManager->FixedUpdateAll(fixedDt);
            }
        });

        if (GetEngineRuntime().moduleHotReload)
            GetEngineRuntime().moduleHotReload->PollChanges();

        // Pump the audio engine: advances source state machine, applies
        // 3D spatialization and distance attenuation. Pre-existing bug —
        // AudioEngine::Update was never called from the main loop.
        SPARK_GUARDED_UPDATE("Audio", "Core", {
            if (GetEngineRuntime().audioEngine)
                GetEngineRuntime().audioEngine->Update(dt);
        });

        UpdateGameplaySystems(dt);
        UpdateDebugSystems(dt);
        SPARK_GUARDED_UPDATE("Console", "Core", {
            Spark::ConsoleProcessManager::GetInstance().ProcessCommands();
            console.Update();
        });

        RunDueScriptedCommands(frameCount);
        ++frameCount;

        auto elapsed = std::chrono::steady_clock::now() - tickStart;
        if (elapsed < TICK_INTERVAL)
            std::this_thread::sleep_for(TICK_INTERVAL - elapsed);
    }

    // Shutdown
    GetEngineRuntime().moduleHotReload.reset();
    // These globals hold graphics/ImGui-adjacent state: destroy them here,
    // in reverse creation order, NOT at static teardown - the C runtime exit
    // path otherwise AVs in ~UIPanel (dead ImGui/graphics) and then hangs
    // inside the crash handler.
    g_modSystem.reset();
    g_dialogueSystem.reset();
    g_uiSystem.reset();
    g_weatherSystem.reset();
    console.LogInfo("Headless server shutting down...");
    g_fileCache.reset();
    ShutdownEngine();

    // Only free the console if we successfully allocated one in
    // AllocHeadlessConsole. Calling FreeConsole on an inherited console
    // detaches us from the parent's console, which we don't want.
    FreeConsole();
    return 0;
}
#endif // SPARK_HEADLESS_SUPPORT
#endif // SPARK_PLATFORM_WINDOWS
