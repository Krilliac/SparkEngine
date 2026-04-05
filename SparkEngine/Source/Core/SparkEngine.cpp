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
#include "Utils/DebugHookManager.h"
#include "Utils/Logger.h"
#include "Utils/JobSystem.h"
#include "Utils/FreezeDetector.h"
#include "Utils/DeadlockDetector.h"
#include "Utils/HitchDetector.h"
#include "Utils/AssetStallDetector.h"
#include "Utils/NetworkHealthMonitor.h"
#include "Utils/GPUResourceLeakDetector.h"
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

// Platform-specific includes
#ifdef SPARK_PLATFORM_WINDOWS
#include "Utils/Assert.h"
#include "Utils/SparkError.h"
#include "Utils/Validate.h"
#include "Utils/DeltaSmoother.h"
#include "Utils/CrashHandler.h"
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


static void InitPhysics()
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
static void InitConsole()
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

static void ShutdownPhysics()
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
static void ShutdownEngine()
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

        g_moduleManager->UnloadAll();
        g_moduleManager.reset();
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
static int g_testFrameLimit = 0;

// Window size override from command line (-window-size WxH).
// 0 means use default from EngineSettings.
static int g_windowWidthOverride = 0;
static int g_windowHeightOverride = 0;

// ============================================================================
// Windows platform
// ============================================================================
#ifdef SPARK_PLATFORM_WINDOWS

/**
 * @brief Parse -test-frames N from a wide command line string (Windows).
 */
static int ParseTestFrameLimit(LPWSTR cmdLine)
{
    std::wstring cmd(cmdLine);
    auto pos = cmd.find(L"-test-frames");
    if (pos == std::wstring::npos)
        return 0;
    pos += 12; // length of "-test-frames"
    while (pos < cmd.size() && cmd[pos] == L' ')
        ++pos;
    if (pos >= cmd.size())
        return 0;
    try
    {
        return std::max(0, std::stoi(std::wstring(cmd.substr(pos))));
    }
    catch (const std::exception&)
    {
        return 0;
    }
}

/**
 * @brief Parse -window-size WxH from a wide command line string (Windows).
 */
static void ParseWindowSizeOverride(LPWSTR cmdLine)
{
    std::wstring cmd(cmdLine);
    auto pos = cmd.find(L"-window-size");
    if (pos == std::wstring::npos)
        return;
    pos += 12;
    while (pos < cmd.size() && cmd[pos] == L' ')
        ++pos;
    if (pos >= cmd.size())
        return;
    auto sizeStr = cmd.substr(pos);
    auto xPos = sizeStr.find(L'x');
    if (xPos == std::wstring::npos)
        xPos = sizeStr.find(L'X');
    if (xPos != std::wstring::npos)
    {
        try
        {
            g_windowWidthOverride = std::max(320, std::stoi(sizeStr.substr(0, xPos)));
            g_windowHeightOverride = std::max(240, std::stoi(sizeStr.substr(xPos + 1)));
        }
        catch (const std::exception&)
        {
            // Ignore malformed window size arguments
        }
    }
}

// Windows-specific globals
constexpr int MAX_LOADSTRING = 100;
HINSTANCE g_hInst;
WCHAR g_szTitle[MAX_LOADSTRING];
WCHAR g_szClass[MAX_LOADSTRING];
std::unique_ptr<Spark::LocalFileCache> g_fileCache;
std::unique_ptr<Spark::WeatherSystem> g_weatherSystem;
std::unique_ptr<Spark::UI::UISystem> g_uiSystem;
std::unique_ptr<Spark::DialogueSystem> g_dialogueSystem;
std::unique_ptr<Spark::ModSystem> g_modSystem;
static Spark::DeltaSmoother g_deltaSmoother(10);

#ifdef SPARK_HEADLESS_SUPPORT
// g_headlessMode is defined in EngineContext.cpp (SparkEngineLib)
static std::atomic<bool> g_shutdownRequested{false};

/**
 * @brief Parse command line for -headless or -dedicated flags
 */
static bool ParseHeadlessFlag(LPWSTR cmdLine)
{
    std::wstring cmd(cmdLine);
    return cmd.find(L"-headless") != std::wstring::npos || cmd.find(L"-dedicated") != std::wstring::npos;
}

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
#endif // SPARK_HEADLESS_SUPPORT

// Win32 forward declarations
ATOM MyRegisterClass(HINSTANCE);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

// Graphics console commands are registered via GraphicsConsoleCommands.cpp

/**
 * @brief Get the executable directory
 */
static std::filesystem::path GetExecutableDirectory()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return std::filesystem::path(exePath).parent_path();
}

/**
 * @brief Find a specific game module DLL from command line
 *
 * Checks for -game <path> on the command line.
 * Returns empty string if not specified.
 */
static std::string FindGameModuleFromCmdLine(LPWSTR cmdLine)
{
    std::wstring cmd(cmdLine);
    size_t pos = cmd.find(L"-game ");
    if (pos != std::wstring::npos)
    {
        size_t start = pos + 6;
        size_t end = cmd.find(L' ', start);
        std::wstring wpath = cmd.substr(start, end - start);
        std::string path(wpath.begin(), wpath.end());
        if (std::filesystem::exists(path))
            return path;
    }
    return "";
}

/**
 * @brief Find the module manifest or fall back to directory scan
 *
 * Loading priority:
 * 1. Command line: -game <path> (loads single module)
 * 2. spark.modules.json manifest next to the engine exe
 * 3. Directory scan for *Game*.dll / *Module*.dll
 */
static bool LoadGameModules(ModuleManager& manager, LPWSTR cmdLine)
{
    auto exeDir = GetExecutableDirectory();

    // 1. Check command line for specific module
    std::string cmdLineModule = FindGameModuleFromCmdLine(cmdLine);
    if (!cmdLineModule.empty())
        return manager.LoadModule(cmdLineModule);

    // 2. Check for module manifest
    auto manifestPath = exeDir / "spark.modules.json";
    if (std::filesystem::exists(manifestPath))
        return manager.LoadModulesFromManifest(manifestPath.string());

    // 3. Fall back to directory scan
    return manager.LoadModulesFromDirectory(exeDir.string());
}

// ===================================================================================
//                       Windows extracted helpers
// ===================================================================================

/**
 * @brief Configure and install the crash handler from EngineSettings + env vars.
 *
 * Settings are read from [CrashReporting] in settings.ini, with env var overrides:
 *   SPARK_GITHUB_REPO, SPARK_GITHUB_TOKEN, SPARK_CRASH_PROXY_URL, SPARK_CRASH_UPLOAD_URL
 */
static void SetupCrashHandler()
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

    // Settings file values
    crashCfg.uploadURL = cr.uploadURL;
    crashCfg.proxyURL = cr.proxyURL;
    crashCfg.githubRepo = cr.githubRepo;
    crashCfg.githubToken = cr.githubToken;

    // Env var overrides (take precedence over settings file)
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
    const char* envHeadless = std::getenv("SPARK_CRASH_HEADLESS");
    if (envHeadless && std::string(envHeadless) == "1")
        crashCfg.headlessMode = true;

    // Auto-detect CI environments — skip dialogs
    if (std::getenv("CI") || std::getenv("GITHUB_ACTIONS") || std::getenv("JENKINS_URL"))
        crashCfg.headlessMode = true;

    InstallCrashHandler(crashCfg);
}

#ifdef SPARK_HEADLESS_SUPPORT

/**
 * @brief Attach a Win32 console for headless stdout/stderr/stdin.
 */
static void AllocHeadlessConsole()
{
    AllocConsole();
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);
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
    g_timer = std::make_unique<Timer>();
    g_eventBus = std::make_unique<Spark::EventBus>();
    EngineContext::SetOwned(std::make_unique<EngineContext>(nullptr, nullptr, g_timer.get(), g_eventBus.get()));

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
    Spark::EngineSetup::InitializeJobSystem();

    ctx->SetSaveSystem(&Spark::SaveSystem::GetInstance());
    ctx->SetCoroutineScheduler(&Spark::CoroutineScheduler::GetInstance());

    return true;
}

/**
 * @brief Load modules and register console commands for headless mode.
 */
static void LoadHeadlessModules(LPWSTR lpCmdLine)
{
    g_moduleManager = std::make_unique<ModuleManager>();
    auto& console = Spark::SimpleConsole::GetInstance();

    if (LoadGameModules(*g_moduleManager, lpCmdLine))
    {
        g_moduleManager->InitializeAll(EngineContext::Get());
        console.LogSuccess("Loaded " + std::to_string(g_moduleManager->GetModuleCount()) + " module(s)");
    }
    else
    {
        console.LogWarning("No game modules found. Running engine-only headless mode.");
    }

    g_moduleHotReload = std::make_unique<Spark::ModuleHotReloadManager>();
    g_moduleHotReload->Initialize(g_moduleManager.get(), EngineContext::Get());
    g_moduleHotReload->WatchAllLoadedModules();
    g_moduleHotReload->Start();

    Spark::RegisterEngineConsoleCommands(g_moduleManager.get(), g_audioEngine.get(), g_moduleHotReload.get());
    Spark::SubsystemFaultIsolator::GetInstance().RegisterConsoleCommands();
    Assert::RegisterConsoleCommands();
}

/**
 * @brief Run the engine in headless/dedicated server mode (Windows).
 *
 * Allocates a console, initializes server-only subsystems, runs a fixed 60 Hz
 * tick loop, and shuts down cleanly on Ctrl+C.
 */
static int RunHeadlessWindows(LPWSTR lpCmdLine)
{
    AllocHeadlessConsole();
    Spark::SimpleConsole::GetInstance().LogInfo("=== Spark Engine (Headless/Dedicated Server) ===");

    if (!InitHeadlessEngineContext())
        return 1;

    if (!Spark::SaveSystem::GetInstance().Initialize("Saves"))
        Spark::SimpleConsole::GetInstance().LogWarning("SaveSystem initialization failed — save/load unavailable");
    Spark::SimpleConsole::GetInstance().LogInfo("SaveSystem initialized");

    InitConsole();
    LoadHeadlessModules(lpCmdLine);
    Spark::FreezeDetector::GetInstance().RegisterConsoleCommands();
    Spark::FreezeDetector::GetInstance().Start();
    Spark::DeadlockDetector::GetInstance().RegisterConsoleCommands();
    Spark::HitchDetector::GetInstance().RegisterConsoleCommands();
    Spark::AssetStallDetector::GetInstance().RegisterConsoleCommands();
    Spark::NetworkHealthMonitor::GetInstance().RegisterConsoleCommands();
    Spark::GPUResourceLeakDetector::GetInstance().RegisterConsoleCommands();
    Assert::RegisterConsoleCommands();

    // Fixed 60 Hz server loop
    constexpr auto TICK_INTERVAL = std::chrono::microseconds(16667);
    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("Starting headless server loop (60 Hz)...");
    console.LogInfo("Press Ctrl+C or type 'quit' to stop.");

    while (!g_shutdownRequested)
    {
        SPARK_HEARTBEAT();
        auto tickStart = std::chrono::steady_clock::now();

        float dt = g_timer ? g_timer->GetDeltaTime() : (1.0f / 60.0f);

        Spark::FixedTimestepAccumulator::GetInstance().Advance(dt);

        SPARK_GUARDED_UPDATE("Modules", "Core", {
            if (g_moduleManager && g_moduleManager->HasModules())
                g_moduleManager->UpdateAll(dt);
        });

        if (g_moduleHotReload)
            g_moduleHotReload->PollChanges();

        UpdateGameplaySystems(dt);
        UpdateDebugSystems(dt);
        SPARK_GUARDED_UPDATE("Console", "Core", {
            Spark::ConsoleProcessManager::GetInstance().ProcessCommands();
            console.Update();
        });

        auto elapsed = std::chrono::steady_clock::now() - tickStart;
        if (elapsed < TICK_INTERVAL)
            std::this_thread::sleep_for(TICK_INTERVAL - elapsed);
    }

    // Shutdown
    g_moduleHotReload.reset();
    console.LogInfo("Headless server shutting down...");
    g_fileCache.reset();
    ShutdownEngine();

    FreeConsole();
    return 0;
}
#endif // SPARK_HEADLESS_SUPPORT

/**
 * @brief Initialize windowed-mode subsystems: engine context, physics, modules,
 *        audio, save system, console commands, and debug/gameplay systems.
 *
 * Called after the Win32 window has been created and InitInstance() succeeded.
 */
static void InitEngineContext()
{
    g_eventBus = std::make_unique<Spark::EventBus>();
    EngineContext::SetOwned(
        std::make_unique<EngineContext>(g_graphics.get(), g_input.get(), g_timer.get(), g_eventBus.get()));

    auto* ctx = EngineContext::Get();
    if (!ctx)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext is null after SetOwned — cannot initialize");
        return;
    }

    g_fileCache = std::make_unique<Spark::LocalFileCache>();
    ctx->SetFileCache(g_fileCache.get());

    InitPhysics();

    Spark::EngineSetup::RegisterCoreSubsystems(*ctx);
    Spark::EngineSetup::InitializeJobSystem();

    ctx->SetSaveSystem(&Spark::SaveSystem::GetInstance());
    ctx->SetCoroutineScheduler(&Spark::CoroutineScheduler::GetInstance());

    static Spark::AssetRegistry g_assetRegistry;
    ctx->SetAssetRegistry(&g_assetRegistry);

    if (g_graphics && g_graphics->GetAssetPipeline())
    {
        ctx->SetAssetPipeline(g_graphics->GetAssetPipeline());
    }
}

static void InitGameplaySubsystems()
{
    auto* ctx = EngineContext::Get();
    if (!ctx)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext is null — gameplay subsystems skipped");
        return;
    }

    g_weatherSystem = std::make_unique<Spark::WeatherSystem>();
    ctx->SetWeather(g_weatherSystem.get());
    ctx->SetTimeOfDay(&Spark::TimeOfDaySystem::GetInstance());

    g_uiSystem = std::make_unique<Spark::UI::UISystem>();
    ctx->SetUI(g_uiSystem.get());

    g_dialogueSystem = std::make_unique<Spark::DialogueSystem>();
    ctx->SetDialogue(g_dialogueSystem.get());

    g_modSystem = std::make_unique<Spark::ModSystem>();
    ctx->SetModSystem(g_modSystem.get());
}

static void LoadAndInitModules(LPWSTR lpCmdLine)
{
    g_moduleManager = std::make_unique<ModuleManager>();
    auto& console = Spark::SimpleConsole::GetInstance();

    if (LoadGameModules(*g_moduleManager, lpCmdLine))
    {
        g_moduleManager->InitializeAll(EngineContext::Get());

        auto* primary = g_moduleManager->GetPrimaryModule();
        if (primary)
        {
            auto info = primary->GetModuleInfo();
            std::wstring title = L"Spark Engine - ";
            std::string modName(info.name);
            title.append(modName.begin(), modName.end());
            HWND hWnd = FindWindowW(g_szClass, g_szTitle);
            if (hWnd)
                SetWindowTextW(hWnd, title.c_str());
        }

        console.LogSuccess("Loaded " + std::to_string(g_moduleManager->GetModuleCount()) + " module(s)");
    }
    else
    {
        console.LogWarning("No game modules found. Running engine-only mode.");
        console.LogInfo("Place a game DLL (e.g. SparkGame.dll) next to the engine executable,");
        console.LogInfo("use -game <path> on the command line,");
        console.LogInfo("or create a spark.modules.json manifest.");
    }

    g_moduleHotReload = std::make_unique<Spark::ModuleHotReloadManager>();
    g_moduleHotReload->Initialize(g_moduleManager.get(), EngineContext::Get());
    g_moduleHotReload->WatchAllLoadedModules();
    g_moduleHotReload->Start();
}

static void InitializeWindowedSubsystems(HINSTANCE hInstance, LPWSTR lpCmdLine)
{
    InitEngineContext();
    InitGameplaySubsystems();

    auto& console = Spark::SimpleConsole::GetInstance();

    if (!Spark::SaveSystem::GetInstance().Initialize("Saves"))
    {
        console.LogWarning("SaveSystem initialization failed — save/load unavailable");
    }
    console.LogInfo("SaveSystem initialized");

    // Initialize AudioEngine before modules so modules can use EngineContext::Get()->GetAudio()
    g_audioEngine = std::make_unique<AudioEngine>();
    if (SUCCEEDED(g_audioEngine->Initialize(32)))
    {
        console.LogInfo("AudioEngine initialized (32 sources)");
        if (auto* ctx = EngineContext::Get())
            ctx->SetAudio(g_audioEngine.get());
    }
    else
    {
        console.LogWarning("AudioEngine initialization failed - audio commands will be unavailable");
        g_audioEngine.reset();
    }

    // Create cross-platform audio backend (wraps AudioEngine on Windows, OpenAL on Linux)
    g_audioBackend = Spark::Audio::CreateAudioBackend(Spark::Audio::AudioBackendType::Auto, g_audioEngine.get());

    LoadAndInitModules(lpCmdLine);

    if (g_graphics)
        Spark::Graphics::RegisterGraphicsConsoleCommands(*g_graphics);
    Spark::RegisterEngineConsoleCommands(g_moduleManager.get(), g_audioEngine.get(), g_moduleHotReload.get());
    Spark::SubsystemFaultIsolator::GetInstance().RegisterConsoleCommands();
    Spark::FreezeDetector::GetInstance().RegisterConsoleCommands();
    Spark::FreezeDetector::GetInstance().Start();
    Spark::DeadlockDetector::GetInstance().RegisterConsoleCommands();
    Spark::HitchDetector::GetInstance().RegisterConsoleCommands();
    Spark::AssetStallDetector::GetInstance().RegisterConsoleCommands();
    Spark::NetworkHealthMonitor::GetInstance().RegisterConsoleCommands();
    Spark::GPUResourceLeakDetector::GetInstance().RegisterConsoleCommands();
    Assert::RegisterConsoleCommands();
    EngineSettings::GetInstance().RegisterConsoleCommands();

    LogMissingModuleWarnings();

    if (g_weatherSystem && g_eventBus)
    {
        g_weatherSystem->SetEventBus(g_eventBus.get());
    }

    InitConsole();
}

/**
 * @brief Run the Win32 message pump and per-frame engine tick loop.
 *
 * Returns the wParam from the WM_QUIT message for use as the process exit code.
 */
static int RunWindowedMainLoop(HINSTANCE hInstance)
// NOTE: Intentionally exceeds 50-line guideline — linear main loop dispatch
{
    HACCEL accel = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_SparkEngine));
    MSG msg = {};
    ASSERT(g_timer);

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("Starting main engine loop...");

    if (g_testFrameLimit > 0)
        console.LogInfo(std::format("Test mode: will exit after {} frames", g_testFrameLimit));

    int frameCount = 0;

    // Win32 message pump: PeekMessage with PM_REMOVE gives us non-blocking
    // message processing — the engine ticks in the else branch whenever
    // there are no pending OS messages (resize, input, focus, etc.).
    while (msg.message != WM_QUIT)
    {
        // Test frame limit: post WM_QUIT to exit cleanly
        if (g_testFrameLimit > 0 && frameCount >= g_testFrameLimit)
        {
            console.LogInfo(std::format("[TEST] Frame limit reached ({} frames). Exiting.", g_testFrameLimit));
            PostQuitMessage(0);
            continue;
        }
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (!TranslateAccelerator(msg.hwnd, accel, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
        else
        {
            SPARK_HEARTBEAT();

            // If the freeze detector requested recovery, skip this frame
            if (SPARK_FREEZE_RECOVERY_REQUESTED())
            {
                SPARK_FREEZE_RECOVERY_ACK();
                continue;
            }

            // Smooth delta time over the last N frames to prevent physics/animation
            // jitter caused by single-frame spikes (e.g. shader compilation stalls,
            // OS scheduling delays). Raw dt is preserved for profiling accuracy.
            float rawDt = g_timer ? g_timer->GetDeltaTime() : 0.016f;
            float dt = g_deltaSmoother.Smooth(rawDt);

            // Advance the global fixed-timestep accumulator so all systems can
            // query GetFixedStepCount() for deterministic fixed-rate updates.
            Spark::FixedTimestepAccumulator::GetInstance().Advance(rawDt);

            SPARK_GUARDED_UPDATE("Input", "Core", {
                if (g_input)
                    g_input->Update();
            });

            if (g_moduleManager && g_moduleManager->HasModules())
            {
                SPARK_GUARDED_UPDATE("Modules", "Core", {
                    g_moduleManager->UpdateAll(dt);
                    g_moduleManager->RenderAll();
                });
            }
            else if (g_graphics)
            {
                // Engine-only mode: just clear and present
                g_graphics->BeginFrame();
                g_graphics->EndFrame();
            }

            if (g_moduleHotReload)
                g_moduleHotReload->PollChanges();

            UpdateGameplaySystems(dt);
            UpdateDebugSystems(dt);
            SPARK_GUARDED_UPDATE("Console", "Core", {
                Spark::ConsoleProcessManager::GetInstance().ProcessCommands();
                console.Update();
            });

            ++frameCount;
        }
    }

    // Shutdown
    g_moduleHotReload.reset();
    console.LogInfo("Shutting down...");
    g_fileCache.reset();
    ShutdownEngine();

    return static_cast<int>(msg.wParam);
}

// ===================================================================================
//                                    wWinMain
// ===================================================================================
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Core);
    ASSERT(hInstance != nullptr);

    SetupCrashHandler();

    g_testFrameLimit = ParseTestFrameLimit(lpCmdLine);
    ParseWindowSizeOverride(lpCmdLine);

#ifdef SPARK_HEADLESS_SUPPORT
    g_headlessMode = ParseHeadlessFlag(lpCmdLine);
    if (g_headlessMode)
        return RunHeadlessWindows(lpCmdLine);
#endif

    // Register window class and title
    ASSERT(MAX_LOADSTRING <= _countof(g_szClass) && MAX_LOADSTRING <= _countof(g_szTitle));
    wcscpy_s(g_szClass, MAX_LOADSTRING, L"SparkEngineWindowClass");
    wcscpy_s(g_szTitle, MAX_LOADSTRING, L"Spark Engine");

    ATOM cls = MyRegisterClass(hInstance);
    ASSERT_MSG(cls != 0, "MyRegisterClass failed");
    if (cls == 0)
    {
        MessageBoxW(nullptr, L"RegisterClassExW failed", L"Fatal Error", MB_ICONERROR);
        return -1;
    }

    // Create window and init graphics/input/timer
    if (!InitInstance(hInstance, nCmdShow))
        return -1;

    // Initialize all engine subsystems, load modules, register commands
    InitializeWindowedSubsystems(hInstance, lpCmdLine);

    // Run the message pump + tick loop until WM_QUIT
    return RunWindowedMainLoop(hInstance);
}

// ===================================================================================
//                       Win32 boilerplate
// ===================================================================================
ATOM MyRegisterClass(HINSTANCE hInst)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_SparkEngine));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDC_SparkEngine);
    wc.lpszClassName = g_szClass;
    wc.hIconSm = LoadIcon(hInst, MAKEINTRESOURCE(IDI_SMALL));

    ATOM result = RegisterClassExW(&wc);
    ASSERT_MSG(result != 0, "RegisterClassExW returned zero");
    return result;
}

BOOL InitInstance(HINSTANCE hInst, int nCmdShow)
{
    ASSERT(hInst != nullptr);
    g_hInst = hInst;

    // Load engine settings from INI (before window creation so we can use the dimensions)
    auto& settings = EngineSettings::GetInstance();
    settings.Load();

    int winW = g_windowWidthOverride > 0 ? g_windowWidthOverride : settings.Graphics().windowWidth;
    int winH = g_windowHeightOverride > 0 ? g_windowHeightOverride : settings.Graphics().windowHeight;

    HWND hWnd = CreateWindowW(g_szClass, g_szTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, winW, winH, nullptr, nullptr,
                              hInst, nullptr);

    if (!hWnd)
    {
        DWORD err = GetLastError();
        wchar_t buf[256];
        swprintf_s(buf, L"CreateWindowW failed (0x%08X)", static_cast<unsigned>(err));
        MessageBoxW(nullptr, buf, L"Fatal Error", MB_ICONERROR);
        return FALSE;
    }

    g_timer = std::make_unique<Timer>();
    ASSERT(g_timer);

    g_graphics = std::make_unique<GraphicsEngine>();
    ASSERT(g_graphics);
    HRESULT hr = g_graphics->Initialize(hWnd);
    if (FAILED(hr))
    {
        wchar_t buf[256];
        swprintf_s(buf, L"Graphics initialization failed (HR=0x%08X)", static_cast<unsigned>(hr));
        MessageBoxW(hWnd, buf, L"Fatal Error", MB_ICONERROR);
        return FALSE;
    }

    // Apply VSync setting from INI
    g_graphics->Console_SetVSync(settings.Graphics().vsync);

    g_input = std::make_unique<InputManager>();
    ASSERT(g_input);
    g_input->Initialize(hWnd);

    // Apply input settings from INI
    g_input->Console_SetMouseSensitivity(settings.Controls().mouseSensitivity);
    g_input->Console_SetInvertMouseY(settings.Controls().invertMouseY);
    g_input->Console_SetMouseDeadZone(settings.Controls().mouseDeadZone);
    g_input->Console_SetRawMouseInput(settings.Controls().rawMouseInput);
    g_input->Console_SetMouseAcceleration(settings.Controls().mouseAcceleration);

    // Console init is handled by InitConsole() in InitializeWindowedSubsystems()
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);

    return TRUE;
}

// ===================================================================================
//                          Window procedure
// ===================================================================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        if (g_input)
            g_input->HandleMessage(msg, wParam, lParam);
        break;

    case WM_SIZE:
        if (g_graphics)
            g_graphics->OnResize(LOWORD(lParam), HIWORD(lParam));
        if (g_moduleManager)
            g_moduleManager->ResizeAll(LOWORD(lParam), HIWORD(lParam));
        if (g_eventBus)
            g_eventBus->Publish(Spark::WindowResizeEvent{LOWORD(lParam), HIWORD(lParam)});
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

INT_PTR CALLBACK About(HWND hDlg, UINT msg, WPARAM wParam, LPARAM)
{
    if (msg == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL))
    {
        EndDialog(hDlg, LOWORD(wParam));
        return TRUE;
    }
    return FALSE;
}


#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// Linux platform
// ============================================================================
#ifndef SPARK_PLATFORM_WINDOWS

static std::atomic<bool> g_shutdownRequested{false};

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

static std::filesystem::path GetExecutableDirectoryLinux()
{
    // /proc/self/exe is the canonical way on Linux
    std::error_code ec;
    auto exePath = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
        return exePath.parent_path();
    return std::filesystem::current_path();
}

static std::string FindGameModuleFromArgs(int argc, char* argv[])
{
    for (int i = 1; i < argc - 1; ++i)
    {
        if (strcmp(argv[i], "-game") == 0)
        {
            std::string path = argv[i + 1];
            if (std::filesystem::exists(path))
                return path;
        }
    }
    return "";
}

static bool LoadGameModulesLinux(ModuleManager& manager, int argc, char* argv[])
{
    auto exeDir = GetExecutableDirectoryLinux();

    // 1. Check command line for specific module
    std::string cmdLineModule = FindGameModuleFromArgs(argc, argv);
    if (!cmdLineModule.empty())
        return manager.LoadModule(cmdLineModule);

    // 2. Check for module manifest
    auto manifestPath = exeDir / "spark.modules.json";
    if (std::filesystem::exists(manifestPath))
        return manager.LoadModulesFromManifest(manifestPath.string());

    // 3. Fall back to directory scan
    return manager.LoadModulesFromDirectory(exeDir.string());
}


// ===================================================================================
//                    Linux extracted helpers
// ===================================================================================

/**
 * @brief Common per-frame tick logic shared by SDL2 windowed and no-SDL2 fallback modes.
 *
 * Updates input, modules, gameplay/debug systems, and console processing.
 */
static void TickFrame(float dt)
{
    SPARK_HEARTBEAT();

    // If the freeze detector requested recovery, skip this frame
    if (SPARK_FREEZE_RECOVERY_REQUESTED())
    {
        SPARK_FREEZE_RECOVERY_ACK();
        return;
    }

    // Advance the global fixed-timestep accumulator for deterministic fixed-rate updates.
    Spark::FixedTimestepAccumulator::GetInstance().Advance(dt);

    SPARK_GUARDED_UPDATE("Input", "Core", {
        if (g_input)
            g_input->Update();
    });

    if (g_moduleManager && g_moduleManager->HasModules())
    {
        SPARK_GUARDED_UPDATE("Modules", "Core", {
            g_moduleManager->UpdateAll(dt);
            g_moduleManager->RenderAll();
        });
    }
    else if (g_graphics)
    {
        g_graphics->BeginFrame();
        g_graphics->EndFrame();
    }

    if (g_moduleHotReload)
        g_moduleHotReload->PollChanges();

    UpdateGameplaySystems(dt);
    UpdateDebugSystems(dt);
    SPARK_GUARDED_UPDATE("Console", "Core", {
        Spark::ConsoleProcessManager::GetInstance().ProcessCommands();
        Spark::SimpleConsole::GetInstance().Update();
    });
}

/**
 * @brief Register gameplay subsystems (Weather, UI, Dialogue, Modding) with EngineContext.
 *
 * Uses function-local statics so these objects live for the process lifetime
 * without polluting the global namespace.
 */
static void RegisterGameplaySubsystems()
{
    auto* ctx = EngineContext::Get();
    if (!ctx)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext is null — gameplay subsystems skipped");
        return;
    }

    static Spark::WeatherSystem s_weatherSystem;
    ctx->SetWeather(&s_weatherSystem);

    // TimeOfDay — singleton, registered with context for game-module access
    ctx->SetTimeOfDay(&Spark::TimeOfDaySystem::GetInstance());

    // Wire WeatherSystem to EventBus for WeatherChangedEvent publishing
    if (g_eventBus)
    {
        s_weatherSystem.SetEventBus(g_eventBus.get());
    }

    static Spark::UI::UISystem s_uiSystem;
    ctx->SetUI(&s_uiSystem);

    static Spark::DialogueSystem s_dialogueSystem;
    ctx->SetDialogue(&s_dialogueSystem);

    static Spark::ModSystem s_modSystem;
    ctx->SetModSystem(&s_modSystem);
}

/**
 * @brief Initialize engine core subsystems common to all Linux startup paths.
 *
 * Creates EngineContext, physics, core subsystem registration, save system,
 * coroutine scheduler, and gameplay subsystem registration.
 *
 * @param registerGameplay If true, registers Weather/UI/Dialogue/Modding and AssetRegistry.
 */
static void InitLinuxCoreSubsystems(bool registerGameplay)
{
    EngineContext::SetOwned(
        std::make_unique<EngineContext>(g_graphics.get(), g_input.get(), g_timer.get(), g_eventBus.get()));

    auto* ctx = EngineContext::Get();
    if (!ctx)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext is null after SetOwned — Linux init aborted");
        return;
    }

    InitPhysics();

    Spark::EngineSetup::RegisterCoreSubsystems(*ctx);
    Spark::EngineSetup::InitializeJobSystem();

    if (!Spark::SaveSystem::GetInstance().Initialize("Saves"))
    {
        Spark::SimpleConsole::GetInstance().LogWarning("SaveSystem initialization failed — save/load unavailable");
    }
    ctx->SetSaveSystem(&Spark::SaveSystem::GetInstance());
    ctx->SetCoroutineScheduler(&Spark::CoroutineScheduler::GetInstance());

    // AssetPipeline (owned by GraphicsEngine, exposed via EngineContext for SDK access)
    if (g_graphics && g_graphics->GetAssetPipeline())
    {
        ctx->SetAssetPipeline(g_graphics->GetAssetPipeline());
    }

    if (registerGameplay)
    {
        static Spark::AssetRegistry s_assetRegistry;
        ctx->SetAssetRegistry(&s_assetRegistry);
        RegisterGameplaySubsystems();
    }
}

/**
 * @brief Load game modules, initialize hot-reload watcher, and register console commands.
 *
 * Shared by all Linux startup paths. Handles module loading via LoadGameModulesLinux,
 * hot-reload setup, audio engine init (if windowed), and console command registration.
 *
 * @param argc Argument count from main().
 * @param argv Argument values from main().
 * @param initAudio If true, creates and initializes AudioEngine.
 */
static void InitLinuxModulesAndCommands(int argc, char* argv[], bool initAudio)
{
    auto& console = Spark::SimpleConsole::GetInstance();

    // Initialize AudioEngine before modules so modules can use EngineContext::Get()->GetAudio()
    if (initAudio)
    {
        g_audioEngine = std::make_unique<AudioEngine>();
        if (SUCCEEDED(g_audioEngine->Initialize(32)))
        {
            console.LogInfo("AudioEngine initialized (32 sources)");
            if (auto* ctx = EngineContext::Get())
                ctx->SetAudio(g_audioEngine.get());
        }
        else
        {
            console.LogWarning("AudioEngine initialization failed");
            g_audioEngine.reset();
        }

        // Create cross-platform audio backend (OpenAL on Linux, wraps AudioEngine on Windows)
        g_audioBackend = Spark::Audio::CreateAudioBackend(Spark::Audio::AudioBackendType::Auto, g_audioEngine.get());
    }

    g_moduleManager = std::make_unique<ModuleManager>();

    if (LoadGameModulesLinux(*g_moduleManager, argc, argv))
    {
        g_moduleManager->InitializeAll(EngineContext::Get());
        console.LogSuccess("Loaded " + std::to_string(g_moduleManager->GetModuleCount()) + " module(s)");
    }
    else
    {
        console.LogWarning("No game modules found. Running engine-only mode.");
    }

    // Module hot-reload watcher
    g_moduleHotReload = std::make_unique<Spark::ModuleHotReloadManager>();
    g_moduleHotReload->Initialize(g_moduleManager.get(), EngineContext::Get());
    g_moduleHotReload->WatchAllLoadedModules();
    g_moduleHotReload->Start();

    // Console commands
    if (g_graphics)
        Spark::Graphics::RegisterGraphicsConsoleCommands(*g_graphics);
    Spark::RegisterEngineConsoleCommands(g_moduleManager.get(), g_audioEngine.get(), g_moduleHotReload.get());
    Spark::SubsystemFaultIsolator::GetInstance().RegisterConsoleCommands();
    Assert::RegisterConsoleCommands();

    LogMissingModuleWarnings();
}

/**
 * @brief Common shutdown sequence for all Linux startup paths.
 */
static void ShutdownLinux()
{
    g_moduleHotReload.reset();
    Spark::SimpleConsole::GetInstance().LogInfo("Shutting down...");
    ShutdownEngine();
}

#ifdef SPARK_HEADLESS_SUPPORT
/**
 * @brief Run the engine in headless/dedicated server mode (Linux).
 *
 * Initializes server-only subsystems (no graphics, no audio), runs a fixed 60 Hz
 * tick loop, and shuts down cleanly on SIGINT/SIGTERM.
 */
static int RunHeadlessLinux(int argc, char* argv[])
{
    Spark::SimpleConsole::GetInstance().LogInfo("=== Spark Engine (Headless/Dedicated Server - Linux) ===");

    g_eventBus = std::make_unique<Spark::EventBus>();
    g_timer = std::make_unique<Timer>();

    // Headless: no gameplay subsystems (no Weather/UI/Dialogue/Modding)
    InitLinuxCoreSubsystems(/*registerGameplay=*/false);

    InitConsole();

    InitLinuxModulesAndCommands(argc, argv, /*initAudio=*/false);
    Spark::FreezeDetector::GetInstance().RegisterConsoleCommands();
    Spark::FreezeDetector::GetInstance().Start();
    Spark::DeadlockDetector::GetInstance().RegisterConsoleCommands();
    Spark::HitchDetector::GetInstance().RegisterConsoleCommands();
    Spark::AssetStallDetector::GetInstance().RegisterConsoleCommands();
    Spark::NetworkHealthMonitor::GetInstance().RegisterConsoleCommands();
    Spark::GPUResourceLeakDetector::GetInstance().RegisterConsoleCommands();
    Assert::RegisterConsoleCommands();

    // Fixed 60 Hz server loop
    constexpr auto TICK_INTERVAL = std::chrono::microseconds(16667);
    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("Starting headless server loop (60 Hz)...");
    console.LogInfo("Press Ctrl+C to stop.");

    if (g_testFrameLimit > 0)
        console.LogInfo(std::format("Test mode: will exit after {} frames", g_testFrameLimit));

    int frameCount = 0;

    while (!g_shutdownRequested)
    {
        if (g_testFrameLimit > 0 && frameCount >= g_testFrameLimit)
        {
            console.LogInfo(std::format("[TEST] Frame limit reached ({} frames). Exiting.", g_testFrameLimit));
            break;
        }

        SPARK_HEARTBEAT();
        auto tickStart = std::chrono::steady_clock::now();
        float dt = g_timer ? g_timer->GetDeltaTime() : (1.0f / 60.0f);

        Spark::FixedTimestepAccumulator::GetInstance().Advance(dt);

        SPARK_GUARDED_UPDATE("Modules", "Core", {
            if (g_moduleManager && g_moduleManager->HasModules())
                g_moduleManager->UpdateAll(dt);
        });

        if (g_moduleHotReload)
            g_moduleHotReload->PollChanges();

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

    ShutdownLinux();
    Spark::SimpleConsole::GetInstance().LogInfo("Headless server shut down cleanly.");
    return 0;
}
#endif // SPARK_HEADLESS_SUPPORT

#ifdef SPARK_SDL2_AVAILABLE
/**
 * @brief Translate an SDL key symbol to a Win32 virtual key code.
 *
 * InputManager uses WM_KEYDOWN/WM_KEYUP style messages internally,
 * so SDL key events must be translated to VK_* codes for consistent handling.
 *
 * @return The corresponding VK_* code, or 0 if the key is not mapped.
 */
static int TranslateSDLKeyToVK(SDL_Keycode sym)
{
    // Alphabetic keys
    if (sym >= SDLK_a && sym <= SDLK_z)
        return 'A' + (sym - SDLK_a);

    // Numeric keys
    if (sym >= SDLK_0 && sym <= SDLK_9)
        return '0' + (sym - SDLK_0);

    // Function keys
    if (sym >= SDLK_F1 && sym <= SDLK_F12)
        return VK_F1 + (sym - SDLK_F1);

    // Named keys
    switch (sym)
    {
    case SDLK_SPACE:
        return VK_SPACE;
    case SDLK_ESCAPE:
        return VK_ESCAPE;
    case SDLK_RETURN:
        return VK_RETURN;
    case SDLK_TAB:
        return VK_TAB;
    case SDLK_BACKSPACE:
        return VK_BACK;
    case SDLK_UP:
        return VK_UP;
    case SDLK_DOWN:
        return VK_DOWN;
    case SDLK_LEFT:
        return VK_LEFT;
    case SDLK_RIGHT:
        return VK_RIGHT;
    case SDLK_LSHIFT:
        return VK_LSHIFT;
    case SDLK_RSHIFT:
        return VK_RSHIFT;
    case SDLK_LCTRL:
        return VK_LCONTROL;
    case SDLK_RCTRL:
        return VK_RCONTROL;
    case SDLK_LALT:
        return VK_LMENU;
    case SDLK_RALT:
        return VK_RMENU;
    case SDLK_DELETE:
        return VK_DELETE;
    default:
        return 0;
    }
}

/**
 * @brief Dispatch a single SDL event to the appropriate engine subsystem.
 *
 * Handles window close/resize, keyboard, and mouse events by translating
 * them into the InputManager's message format.
 *
 * @param event The SDL event to process.
 * @return false if the application should quit, true otherwise.
 */
static bool HandleSDLEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    case SDL_QUIT:
        return false;

    case SDL_WINDOWEVENT:
        if (event.window.event == SDL_WINDOWEVENT_CLOSE)
            return false;
        if (event.window.event == SDL_WINDOWEVENT_RESIZED)
        {
            int w = event.window.data1;
            int h = event.window.data2;
            if (g_graphics)
                g_graphics->OnResize(w, h);
            if (g_moduleManager)
                g_moduleManager->ResizeAll(w, h);
            if (g_eventBus)
                g_eventBus->Publish(Spark::WindowResizeEvent{static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
        }
        break;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (g_input)
        {
            UINT msg = (event.type == SDL_KEYDOWN) ? WM_KEYDOWN : WM_KEYUP;
            int vk = TranslateSDLKeyToVK(event.key.keysym.sym);
            if (vk != 0)
                g_input->HandleMessage(msg, static_cast<WPARAM>(vk), 0);
        }
        break;

    case SDL_MOUSEMOTION:
        if (g_input)
            g_input->HandleMessage(WM_MOUSEMOVE, 0,
                                   static_cast<LPARAM>((event.motion.y << 16) | (event.motion.x & 0xFFFF)));
        break;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (g_input)
        {
            UINT msg = 0;
            if (event.button.button == SDL_BUTTON_LEFT)
                msg = (event.type == SDL_MOUSEBUTTONDOWN) ? WM_LBUTTONDOWN : WM_LBUTTONUP;
            else if (event.button.button == SDL_BUTTON_RIGHT)
                msg = (event.type == SDL_MOUSEBUTTONDOWN) ? WM_RBUTTONDOWN : WM_RBUTTONUP;
            else if (event.button.button == SDL_BUTTON_MIDDLE)
                msg = (event.type == SDL_MOUSEBUTTONDOWN) ? WM_MBUTTONDOWN : WM_MBUTTONUP;
            if (msg)
                g_input->HandleMessage(msg, 0, 0);
        }
        break;
    }

    return true;
}

/**
 * @brief Initialize SDL2 windowed-mode subsystems: window, graphics, input,
 *        engine context, modules, audio, and console commands.
 *
 * @param window The SDL2 window (already created by the caller).
 * @param argc Argument count from main().
 * @param argv Argument values from main().
 */
static void InitializeSDL2Subsystems(SDL_Window* window, int argc, char* argv[])
{
    auto& settings = EngineSettings::GetInstance();

    // Core engine objects
    g_timer = std::make_unique<Timer>();
    g_eventBus = std::make_unique<Spark::EventBus>();
    g_input = std::make_unique<InputManager>();
    g_input->Initialize(static_cast<HWND>(window));
    g_graphics = std::make_unique<GraphicsEngine>();

    HRESULT hr = g_graphics->Initialize(static_cast<Spark::NativeWindowHandle>(window));
    auto& console = Spark::SimpleConsole::GetInstance();
    if (SUCCEEDED(hr))
        console.LogInfo("Graphics engine initialized (RHI backend).");
    else
        console.LogWarning("Graphics engine initialization deferred (headless fallback).");

    // Engine context, physics, core subsystems, gameplay subsystems
    InitLinuxCoreSubsystems(/*registerGameplay=*/true);

    // Modules, audio, console commands
    InitLinuxModulesAndCommands(argc, argv, /*initAudio=*/true);

    // Update window title with primary module name
    if (g_moduleManager)
    {
        auto* primary = g_moduleManager->GetPrimaryModule();
        if (primary)
        {
            auto info = primary->GetModuleInfo();
            std::string title = std::string("Spark Engine - ") + info.name;
            SDL_SetWindowTitle(window, title.c_str());
        }
    }

    settings.RegisterConsoleCommands();
    Spark::FreezeDetector::GetInstance().RegisterConsoleCommands();
    Spark::FreezeDetector::GetInstance().Start();
    Spark::HitchDetector::GetInstance().RegisterConsoleCommands();
    Spark::AssetStallDetector::GetInstance().RegisterConsoleCommands();
    Spark::NetworkHealthMonitor::GetInstance().RegisterConsoleCommands();
    Spark::GPUResourceLeakDetector::GetInstance().RegisterConsoleCommands();
    Assert::RegisterConsoleCommands();

    // Initialize console, debug, and gameplay systems in one call
    // (also publishes EngineStartEvent when complete)
    InitConsole();
}

/**
 * @brief Run the SDL2 event pump and per-frame engine tick loop.
 *
 * Processes SDL events via HandleSDLEvent(), then calls TickFrame() for
 * the engine update. Returns when the window is closed or SIGINT is received.
 */
static void RunSDL2MainLoop()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("Starting main engine loop (SDL2)...");

    if (g_testFrameLimit > 0)
        console.LogInfo(std::format("Test mode: will exit after {} frames", g_testFrameLimit));

    int frameCount = 0;

    while (!g_shutdownRequested)
    {
        if (g_testFrameLimit > 0 && frameCount >= g_testFrameLimit)
        {
            console.LogInfo(std::format("[TEST] Frame limit reached ({} frames). Exiting.", g_testFrameLimit));
            break;
        }

        SDL_Event event;
        bool running = true;

        while (SDL_PollEvent(&event))
        {
            if (!HandleSDLEvent(event))
            {
                running = false;
                break;
            }
        }

        if (!running)
            break;

        float dt = g_timer ? g_timer->GetDeltaTime() : 0.016f;
        TickFrame(dt);
        ++frameCount;
    }
}

/**
 * @brief Run the engine in SDL2 windowed mode (Linux).
 *
 * Creates an SDL2 window, initializes all engine subsystems, runs the
 * main loop, and cleans up SDL resources on exit.
 */
static int RunSDL2Windowed(int argc, char* argv[])
{
    Spark::SimpleConsole::GetInstance().LogInfo("=== Spark Engine (Linux Build) ===");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
    {
        Spark::SimpleConsole::GetInstance().LogError(std::string("SDL_Init failed: ") + SDL_GetError());
        return -1;
    }

    auto& settings = EngineSettings::GetInstance();
    settings.Load();

    int winW = g_windowWidthOverride > 0 ? g_windowWidthOverride : settings.Graphics().windowWidth;
    int winH = g_windowHeightOverride > 0 ? g_windowHeightOverride : settings.Graphics().windowHeight;

    // Set OpenGL attributes before window creation (required for Mesa/llvmpipe)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (settings.Graphics().fullscreen)
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    windowFlags |= SDL_WINDOW_OPENGL;

    SDL_Window* window =
        SDL_CreateWindow("Spark Engine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW, winH, windowFlags);
    if (!window)
    {
        Spark::SimpleConsole::GetInstance().LogError(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        SDL_Quit();
        return -1;
    }

    // Create SDL GL context and make it current before engine graphics init.
    // This ensures Mesa llvmpipe and other software renderers work correctly —
    // the GraphicsEngine can then share or skip its own bootstrap context.
    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    if (!glContext)
    {
        Spark::SimpleConsole::GetInstance().LogWarning(std::string("SDL_GL_CreateContext failed: ") + SDL_GetError() +
                                                       " — engine will try headless fallback");
    }
    else
    {
        SDL_GL_MakeCurrent(window, glContext);
        SDL_GL_SetSwapInterval(1);
        Spark::SimpleConsole::GetInstance().LogInfo("SDL2 OpenGL context created successfully");
    }

    InitializeSDL2Subsystems(window, argc, argv);
    RunSDL2MainLoop();

    ShutdownLinux();
    if (glContext)
        SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
#endif // SPARK_SDL2_AVAILABLE

#ifndef SPARK_SDL2_AVAILABLE
/**
 * @brief Run the engine without SDL2 (no-window fallback).
 *
 * Initializes engine subsystems in headless-like mode, processes a few ticks
 * to validate initialization, then exits. Used when SDL2 is not available
 * and the engine was not explicitly started in headless mode.
 */
static int RunNoSDL2Fallback(int argc, char* argv[])
{
    auto& noSdlConsole = Spark::SimpleConsole::GetInstance();
    noSdlConsole.LogInfo("=== Spark Engine (Linux Build) ===");
    noSdlConsole.LogWarning("SDL2 not available. Running without a window.");
    noSdlConsole.LogWarning("Install SDL2 and rebuild with -DENABLE_SDL2=ON for windowed mode.");

    g_eventBus = std::make_unique<Spark::EventBus>();
    g_timer = std::make_unique<Timer>();
    g_input = std::make_unique<InputManager>();
    g_graphics = std::make_unique<GraphicsEngine>();

    HRESULT hr = g_graphics->Initialize(nullptr);
    if (FAILED(hr))
        noSdlConsole.LogWarning("Graphics initialization failed (fallback mode).");

    // Engine context, physics, core subsystems, gameplay subsystems
    InitLinuxCoreSubsystems(/*registerGameplay=*/true);

    InitConsole();
    Spark::SimpleConsole::GetInstance().LogWarning("No SDL2 - engine will exit after initialization.");

    InitLinuxModulesAndCommands(argc, argv, /*initAudio=*/false);

    // Minimal loop — process a few ticks to validate initialization, then exit
    for (int frame = 0; frame < 10 && !g_shutdownRequested; ++frame)
    {
        float dt = g_timer ? g_timer->GetDeltaTime() : 0.016f;
        TickFrame(dt);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    ShutdownLinux();
    return 0;
}
#endif // !SPARK_SDL2_AVAILABLE

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

    try
    {
        g_testFrameLimit = ParseTestFrameLimitArgs(argc, argv);
        ParseWindowSizeOverrideArgs(argc, argv);

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
    if (g_eventBus)
    {
        try
        {
            g_eventBus->ClearAll();
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
    g_eventBus.release();
    g_moduleManager.release();
    return 1;
}
#endif // !SPARK_PLATFORM_WINDOWS
