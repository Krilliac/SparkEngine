/**
 * @file SparkEngineLinuxHeadless.cpp
 * @brief Headless/dedicated-server loop and no-SDL2 fallback path (Linux + macOS).
 *
 * Split from SparkEngineLinux.cpp to keep files under the ~500-line guideline.
 * Contains RunHeadlessLinux (fixed 60 Hz server loop, no graphics/audio) and
 * RunNoSDL2Fallback (windowless validation loop when SDL2 is not compiled in).
 * Shared startup/tick/shutdown helpers live in SparkEngineLinuxInit.cpp.
 */
#include "SparkEngine.h"
#include "Platform.h"
#include "SparkEngineLinuxInternal.h"
#include "Engine/Cinematic/Sequencer.h"
#include "EngineRuntime.h"
#include "ModuleManager.h"
#include "FaultIsolation.h" // SPARK_GUARDED_UPDATE / SubsystemFaultIsolator (mirrors SparkEngineWindows.cpp)
#include "Engine/Events/EventSystem.h"
#include "Graphics/GraphicsEngine.h"
#include "Input/InputManager.h"
#include "Audio/AudioEngine.h"
#include "Utils/Timer.h"
#include "Utils/SparkConsole.h"
#include "Utils/ConsoleProcessManager.h"
#include "GameplaySystemLifecycle.h"
#include "ModuleHotReload.h"
#include "Utils/Logger.h"
#include "Utils/LogMacros.h" // SPARK_LOG_*
#include "Utils/FreezeDetector.h"
#include "Utils/DeadlockDetector.h"
#include "Utils/HitchDetector.h"
#include "Utils/BenchmarkFramework.h"
#include "Utils/AssetStallDetector.h"
#include "Core/AssetValidator.h"
#include "Utils/NetworkHealthMonitor.h"
#include "Utils/GPUResourceLeakDetector.h"
#include "Utils/InvalidStateDetector.h"
#include "Utils/Assert.h"
#include "FixedTimestepAccumulator.h"
#include <chrono>
#include <format>
#include <memory>
#include <string_view>
#include <thread>

#ifndef SPARK_PLATFORM_WINDOWS

#ifdef SPARK_HEADLESS_SUPPORT
/**
 * @brief Run the engine in headless/dedicated server mode (Linux).
 *
 * Initializes server-only subsystems (no graphics, no audio), runs a fixed 60 Hz
 * tick loop, and shuts down cleanly on SIGINT/SIGTERM.
 */
int RunHeadlessLinux(int argc, char* argv[])
{
    Spark::SimpleConsole::GetInstance().LogInfo("=== Spark Engine (Headless/Dedicated Server - Linux) ===");

    GetEngineRuntime().eventBus = std::make_unique<Spark::EventBus>();
    GetEngineRuntime().timer = std::make_unique<Timer>();

    // Headless: no gameplay subsystems (no Weather/UI/Dialogue/Modding)
    InitLinuxCoreSubsystems(/*registerGameplay=*/false);
    Spark::Cinematic::SequencerManager::GetInstance().SetAudioBackend(nullptr);

    InitConsole();

    // Minimal-init mode skips module loading and all detector singletons.
    // See SparkEngine.cpp::g_minimalInit for the full rationale — on a
    // gVisor sandbox every detector thread is another roll of the dice
    // against the Wine gs.base race, and the main loop runs fine without
    // any of them registered.
    if (!g_minimalInit)
    {
        InitLinuxModulesAndCommands(argc, argv, /*initAudio=*/false);
        Spark::FreezeDetector::GetInstance().RegisterConsoleCommands();
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
    }
    else
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "RunHeadlessLinux: modules + detectors skipped (-minimal-init)");
    }

    bool requireGameModule = false;
    for (int i = 1; i < argc; ++i)
        requireGameModule = requireGameModule || std::string_view(argv[i]) == "-require-game";

    int exitCode = 0;
    if (requireGameModule &&
        (!GetEngineRuntime().moduleManager || GetEngineRuntime().moduleManager->GetInitializedModuleCount() == 0))
    {
        Spark::SimpleConsole::GetInstance().LogError(
            "Required game module was not initialized; terminating with a failure status.");
        g_shutdownRequested.store(true, std::memory_order_relaxed);
        exitCode = 2;
    }

    // Fixed 60 Hz server loop
    constexpr auto TICK_INTERVAL = std::chrono::microseconds(16667);
    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("Starting headless server loop (60 Hz)...");
    console.LogInfo("Press Ctrl+C to stop.");

    if (g_testFrameLimit > 0)
        console.LogInfo(std::format("Test mode: will exit after {} frames", g_testFrameLimit));

    int frameCount = 0;

    while (true)
    {
        if (g_shutdownRequested)
        {
            if (CanShutdownEngine())
                break;
            console.LogError("Shutdown request cancelled: a module could not checkpoint for safe unload");
            g_shutdownRequested.store(false, std::memory_order_relaxed);
        }

        if (g_testFrameLimit > 0 && frameCount >= g_testFrameLimit)
        {
            if (CanShutdownEngine())
            {
                console.LogInfo(std::format("[TEST] Frame limit reached ({} frames). Exiting.", g_testFrameLimit));
                break;
            }
            console.LogError("[TEST] Exit postponed: a module could not checkpoint for unload");
        }

        SPARK_HEARTBEAT();
        auto tickStart = std::chrono::steady_clock::now();
        float dt = GetEngineRuntime().timer ? GetEngineRuntime().timer->GetDeltaTime() : (1.0f / 60.0f);

        Spark::FixedTimestepAccumulator::GetInstance().Advance(dt);

        SPARK_GUARDED_UPDATE("Modules", "Core", {
            if (GetEngineRuntime().moduleManager && GetEngineRuntime().moduleManager->HasInitializedModules())
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

        // Pump the audio engine — see TickFrame for the rationale.
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

        ++frameCount;

        auto elapsed = std::chrono::steady_clock::now() - tickStart;
        if (elapsed < TICK_INTERVAL)
            std::this_thread::sleep_for(TICK_INTERVAL - elapsed);
    }

    ShutdownLinuxAfterPreflight();
    Spark::SimpleConsole::GetInstance().LogInfo("Headless server shut down cleanly.");
    return exitCode;
}
#endif // SPARK_HEADLESS_SUPPORT

#ifndef SPARK_SDL2_AVAILABLE
/**
 * @brief Run the engine without SDL2 (no-window fallback).
 *
 * Initializes engine subsystems in headless-like mode, processes a few ticks
 * to validate initialization, then exits. Used when SDL2 is not available
 * and the engine was not explicitly started in headless mode.
 */
int RunNoSDL2Fallback(int argc, char* argv[])
{
    auto& noSdlConsole = Spark::SimpleConsole::GetInstance();
    noSdlConsole.LogInfo("=== Spark Engine (Linux Build) ===");
    noSdlConsole.LogWarning("SDL2 not available. Running without a window.");
    noSdlConsole.LogWarning("Install SDL2 and rebuild with -DENABLE_SDL2=ON for windowed mode.");

    GetEngineRuntime().eventBus = std::make_unique<Spark::EventBus>();
    GetEngineRuntime().timer = std::make_unique<Timer>();
    GetEngineRuntime().input = std::make_unique<InputManager>();
    GetEngineRuntime().graphics = std::make_unique<GraphicsEngine>();

    HRESULT hr = GetEngineRuntime().graphics->Initialize(nullptr);
    if (FAILED(hr))
        noSdlConsole.LogWarning("Graphics initialization failed (fallback mode).");

    // Engine context, physics, core subsystems, gameplay subsystems
    InitLinuxCoreSubsystems(/*registerGameplay=*/true);

    InitConsole();
    Spark::SimpleConsole::GetInstance().LogWarning("No SDL2 - engine will exit after initialization.");

    InitLinuxModulesAndCommands(argc, argv, /*initAudio=*/false);

    // Minimal loop — process a few ticks to validate initialization, then
    // exit only after every module reaches a safe unload checkpoint.  Keep
    // ticking on a veto so transient persistence failures can be retried;
    // returning from main would destroy runtime state despite the veto.
    int frame = 0;
    bool exitPostponedLogged = false;
    while (true)
    {
        if (g_shutdownRequested || frame >= 10)
        {
            if (CanShutdownEngine())
                break;
            if (!exitPostponedLogged)
            {
                noSdlConsole.LogError("Fallback exit postponed: a module could not checkpoint for safe unload");
                exitPostponedLogged = true;
            }
            g_shutdownRequested.store(false, std::memory_order_relaxed);
        }

        float dt = GetEngineRuntime().timer ? GetEngineRuntime().timer->GetDeltaTime() : 0.016f;
        TickFrame(dt);
        ++frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    ShutdownLinuxAfterPreflight();
    return 0;
}
#endif // !SPARK_SDL2_AVAILABLE

#endif // !SPARK_PLATFORM_WINDOWS
