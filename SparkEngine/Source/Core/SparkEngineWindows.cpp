/**
 * @file SparkEngineWindows.cpp
 * @brief Windows entry point (wWinMain) and platform helpers
 *
 * Contains the Win32 message loop, D3D11 initialization, and Windows-specific
 * helper functions. Linux counterpart lives in SparkEngineLinux.cpp.
 * Shared globals and SetupCrashHandler stay in SparkEngine.cpp.
 */
#include "SparkEngine.h"
#include "Platform.h"
#include "framework.h"
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
#include "Graphics/WeatherSystem.h"
#include "Engine/World/TimeOfDaySystem.h"
#include "Engine/UI/UISystem.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Engine/Modding/ModSystem.h"
#include "ModuleHotReload.h"
#include "Graphics/Neural/NeuralInference.h"
#include "Utils/DebugHookManager.h"
#include "Utils/Logger.h"
#include "Utils/WineDetection.h"
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
#include "Utils/Assert.h"
#include "Utils/SparkError.h"
#include "Utils/Validate.h"
#include "Utils/DeltaSmoother.h"
#include "Utils/D3DUtils.h"
#include "Utils/LocalFileCache.h"
#include "Utils/CrashHandler.h"
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

// Shared globals and functions defined in SparkEngine.cpp
extern std::unique_ptr<GraphicsEngine> g_graphics;
extern std::unique_ptr<InputManager> g_input;
extern std::unique_ptr<Timer> g_timer;
extern std::unique_ptr<Spark::EventBus> g_eventBus;
extern std::unique_ptr<ModuleManager> g_moduleManager;
extern std::unique_ptr<AudioEngine> g_audioEngine;
extern std::unique_ptr<Spark::Audio::IAudioBackend> g_audioBackend;
extern std::unique_ptr<Spark::ModuleHotReloadManager> g_moduleHotReload;
extern int g_testFrameLimit;
extern int g_windowWidthOverride;
extern int g_windowHeightOverride;
extern void InitPhysics();
extern void InitConsole();
extern void ShutdownPhysics();
extern void ShutdownEngine();
extern void SetupCrashHandler();

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
        // error_code overload: don't let a malformed -game value throw from main.
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && !ec)
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

#endif // SPARK_PLATFORM_WINDOWS — end of the block that started above; SetupCrashHandler

#ifdef SPARK_PLATFORM_WINDOWS

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
    Spark::InvalidStateDetector::GetInstance().RegisterConsoleCommands();
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

        // Pump the audio engine: advances source state machine, applies
        // 3D spatialization and distance attenuation. Pre-existing bug —
        // AudioEngine::Update was never called from the main loop.
        SPARK_GUARDED_UPDATE("Audio", "Core", {
            if (g_audioEngine)
                g_audioEngine->Update(dt);
        });

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

    // Initialize neural inference engine (GPU compute-based, no external ML deps)
    auto& neuralInference = Spark::Graphics::Neural::NeuralInferenceEngine::GetInstance();
    neuralInference.Initialize();
    ctx->RegisterSystem<Spark::Graphics::Neural::NeuralInferenceEngine>(&neuralInference);
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
    Spark::InvalidStateDetector::GetInstance().RegisterConsoleCommands();
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

            // Pump the audio engine: advances source state machine, applies
            // 3D spatialization and distance attenuation. Pre-existing bug —
            // AudioEngine::Update was never called from the main loop.
            SPARK_GUARDED_UPDATE("Audio", "Core", {
                if (g_audioEngine)
                    g_audioEngine->Update(dt);
            });

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

    // Initialize the unified Logger with a stderr sink as the very first
    // engine action so early init SPARK_LOG_* calls are visible. Matches
    // the same fix applied on the Linux path (SparkEngineLinux.cpp main).
    // The later InitializeDebugSystemsImpl ClearSinks()+AddSink() keeps
    // this idempotent.
    {
        auto& earlyLogger = Spark::Logger::Get();
        earlyLogger.Initialize(/*enableAsync=*/false);
        earlyLogger.AddSink(std::make_unique<Spark::StderrSink>());
    }

    // Log whether we're under Wine so operators can tell at a glance
    // when debugging a cross-host issue. No-op on native Windows.
    Spark::LogWineEnvironmentIfApplicable();

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
