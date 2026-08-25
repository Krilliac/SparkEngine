/**
 * @file SparkEngineLinux.cpp
 * @brief POSIX entry point (main), signal handling, and command-line parsing (Linux + macOS).
 *
 * Contains main() plus SIGINT/SIGTERM handling and argv flag parsing shared
 * between Linux and macOS. The startup/tick/shutdown helpers live in
 * SparkEngineLinuxInit.cpp, the headless and no-SDL2 paths in
 * SparkEngineLinuxHeadless.cpp, and the SDL2 windowed path in
 * SparkEngineLinuxSDL2.cpp / SparkEngineLinuxSDL2Events.cpp. macOS-specific
 * bits (Metal view, _NSGetExecutablePath) are isolated in SparkEngineMacOS.cpp
 * behind the Spark::MacOS helper API. Windows counterpart lives in
 * SparkEngineWindows.cpp. Shared globals and SetupCrashHandler stay in
 * SparkEngine.cpp.
 */
#include "SparkEngine.h"
#include "Platform.h"
#include "SparkEngineLinuxInternal.h"
#include "EngineRuntime.h"
#include "ModuleManager.h"
#include "ModuleHotReload.h"
#include "RuntimePackage.h"
#include "Engine/Events/EventSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/Logger.h"
#include "Utils/LogMacros.h" // SPARK_LOG_*
#include "Utils/WineDetection.h"
#include "Utils/CrashHandler.h"
#include <csignal>
#include <cstring>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

#ifndef SPARK_PLATFORM_WINDOWS

std::atomic<bool> g_shutdownRequested{false};

static void SignalHandler(int)
{
    g_shutdownRequested.store(true, std::memory_order_relaxed);
}

static bool ParseFlag(int argc, char* argv[], const char* flag)
{
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], flag) == 0)
            return true;
    }
    return false;
}

static int ParseTestFrameLimitArgs(int argc, char* argv[])
{
    for (int i = 1; i < argc - 1; ++i)
    {
        if (strcmp(argv[i], "-test-frames") == 0 || strcmp(argv[i], "--test-frames") == 0)
            return std::max(0, std::atoi(argv[i + 1]));
    }
    return 0;
}

/**
 * @brief Parse -threads N from argv, with SPARK_MAX_WORKER_THREADS env
 *        fallback. Command line wins on conflict. See the Windows path's
 *        ParseThreadCount for the full rationale.
 */
static uint32_t ParseThreadCountArgs(int argc, char* argv[])
{
    uint32_t fromEnv = 0;
    if (const char* env = std::getenv("SPARK_MAX_WORKER_THREADS"))
        fromEnv = static_cast<uint32_t>(std::max(0, std::atoi(env)));

    for (int i = 1; i < argc - 1; ++i)
    {
        if (strcmp(argv[i], "-threads") == 0 || strcmp(argv[i], "--threads") == 0)
            return static_cast<uint32_t>(std::max(0, std::atoi(argv[i + 1])));
    }
    return fromEnv;
}

static void ParseWindowSizeOverrideArgs(int argc, char* argv[])
{
    for (int i = 1; i < argc - 1; ++i)
    {
        if (strcmp(argv[i], "-window-size") == 0 || strcmp(argv[i], "--window-size") == 0)
        {
            int w = 0, h = 0;
            if (sscanf(argv[i + 1], "%dx%d", &w, &h) == 2 || sscanf(argv[i + 1], "%dX%d", &w, &h) == 2)
            {
                g_windowWidthOverride = std::max(320, w);
                g_windowHeightOverride = std::max(240, h);
            }
            return;
        }
    }
}

// ===================================================================================
//                                    main
// ===================================================================================

int main(int argc, char* argv[])
{
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // Install SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE/SIGTRAP handlers so hard crashes
    // on Linux go through the same crash-report pipeline that wWinMain uses on
    // Windows. Without this, fatal signals on Linux bypass the reporter entirely.
    SetupCrashHandler();

    // Initialize the unified Logger with a stderr sink as the *very first* engine
    // action so every SPARK_LOG_* call during early init (graphics bring-up, core
    // subsystem registration, module loading, etc.) is captured. Without this,
    // the SDL2 windowed path silently dropped ~10 MB of log output during the
    // gap between subsystem construction and InitDebugSystemsImpl, because
    // Logger::Log() early-returns while m_initialized is still false. The later
    // InitDebugSystemsImpl::Logger::Initialize() call becomes a no-op (idempotent
    // via the m_initialized check). ApplyConfig / FileLogger / ChromeTracing all
    // still happen in InitDebugSystemsImpl as before.
    {
        auto& earlyLogger = Spark::Logger::Get();
        earlyLogger.Initialize(/*enableAsync=*/false);
        earlyLogger.AddSink(std::make_unique<Spark::StderrSink>());
    }

    // Log Wine environment if applicable. On native Linux this is a no-op
    // (Spark::IsRunningUnderWine always returns false in the non-Windows
    // build); it's here for symmetry and so builds compiled with MinGW
    // that fall through to the Linux main (via _WIN32 being undefined in
    // some cross-compile permutations) still get the banner.
    Spark::LogWineEnvironmentIfApplicable();

    try
    {
        g_testFrameLimit = ParseTestFrameLimitArgs(argc, argv);
        g_maxWorkerThreads = ParseThreadCountArgs(argc, argv);
        g_noSubprocess = ParseFlag(argc, argv, "-no-subprocess");
        g_minimalInit = ParseFlag(argc, argv, "-minimal-init");
        g_noJobSystem = ParseFlag(argc, argv, "-no-jobsystem");
        ParseWindowSizeOverrideArgs(argc, argv);

        const bool hasExplicitLaunchRoot =
            ParseFlag(argc, argv, "-game") || ParseFlag(argc, argv, "-manifest") || ParseFlag(argc, argv, "-scene");
        if (!hasExplicitLaunchRoot)
        {
            std::error_code packageError;
            const auto packageResult = Spark::RuntimePackage::AnchorWorkingDirectory(
                Spark::RuntimePackage::GetExecutableDirectory(), packageError);
            if (packageResult == Spark::RuntimePackage::WorkingDirectoryResult::Anchored)
                SPARK_LOG_INFO(Spark::LogCategory::Core, "Anchored packaged runtime to its executable directory");
            else if (packageResult == Spark::RuntimePackage::WorkingDirectoryResult::Failed)
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "Could not enter packaged runtime directory: %s",
                                packageError.message().c_str());
        }

#ifdef SPARK_HEADLESS_SUPPORT
        bool headless = ParseFlag(argc, argv, "-headless") || ParseFlag(argc, argv, "-dedicated");
        g_headlessMode = headless;
        if (headless)
            return RunHeadlessLinux(argc, argv);
#endif

#ifdef SPARK_SDL2_AVAILABLE
        int result = RunSDL2Windowed(argc, argv);
#else
        int result = RunNoSDL2Fallback(argc, argv);
#endif

        Spark::SimpleConsole::GetInstance().LogInfo("Spark Engine shut down cleanly.");

#ifndef _WIN32
        // Defensive finalization for Linux:
        // Dynamic module code can still be referenced by late static destructors
        // (e.g., event channels/handlers captured in global singletons), which can
        // trigger a use-after-dlclose segfault during process teardown.
        // We intentionally clear/release these pointers and terminate without
        // running additional static destruction code.
        if (GetEngineRuntime().eventBus)
        {
            try
            {
                GetEngineRuntime().eventBus->ClearAll();
            }
            catch (...)
            {
                // Best-effort only during final process teardown.
            }
        }
        GetEngineRuntime().moduleHotReload.release();
        GetEngineRuntime().moduleManager.release();
        GetEngineRuntime().eventBus.release();
        std::_Exit(result);
#endif

        return result;
    }
    catch (const std::system_error& e)
    {
        fprintf(stderr, "[FATAL] System error during engine execution: %s (code: %d)\n", e.what(), e.code().value());
    }
    catch (const std::bad_alloc&)
    {
        fprintf(stderr, "[FATAL] Out of memory during engine execution\n");
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "[FATAL] Unhandled exception: %s\n", e.what());
    }

    // Emergency cleanup: release globals to prevent segfault during static
    // destruction. EventBus channels hold vtable pointers into module .so code;
    // if modules are unloaded first, channel destructors will segfault.
    // Under resource exhaustion, module shutdown itself may throw (from
    // destructors calling thread join, etc.), so we leak rather than crash.
    if (GetEngineRuntime().eventBus)
    {
        try
        {
            GetEngineRuntime().eventBus->ClearAll();
        }
        catch (const std::exception& e)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Exception during eventBus cleanup: %s", e.what());
        }
        catch (...)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Unknown exception during eventBus cleanup");
        }
    }
    GetEngineRuntime().eventBus.release();
    GetEngineRuntime().moduleManager.release();
    return 1;
}
#endif // !SPARK_PLATFORM_WINDOWS
