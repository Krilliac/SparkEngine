/**
 * @file SparkEngineLinux.cpp
 * @brief POSIX entry point (main) and SDL2 event loop (Linux + macOS).
 *
 * Contains the SDL2 initialization, signal handling, and main loop shared
 * between Linux and macOS. macOS-specific bits (Metal view, _NSGetExecutablePath)
 * are isolated in SparkEngineMacOS.cpp behind the Spark::MacOS helper API.
 * Windows counterpart lives in SparkEngineWindows.cpp.
 * Shared globals and SetupCrashHandler stay in SparkEngine.cpp.
 */
#include "SparkEngine.h"
#include "Platform.h"
#include "SparkEngineMacOS.h"
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
#include "Graphics/RHI/RHIBridge.h"
#include "Graphics/RHI/RHIFactory.h"
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
#include "Utils/CrashHandler.h"
#include <csignal>
#include <cstring>
#ifdef SPARK_SDL2_AVAILABLE
#include <SDL.h>
#include <SDL_vulkan.h>
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

// Subsystem ownership lives in GetEngineRuntime() (see EngineRuntime.h).
// Command-line flags and other cross-file non-subsystem globals still
// live in SparkEngine.cpp and are declared here as extern.
extern int g_testFrameLimit;
extern uint32_t g_maxWorkerThreads;
extern bool g_noSubprocess;
extern bool g_minimalInit;
extern bool g_noJobSystem;
extern int g_windowWidthOverride;
extern int g_windowHeightOverride;
extern void InitPhysics();
extern void InitConsole();
extern void ShutdownPhysics();
extern void ShutdownEngine();
extern void SetupCrashHandler();

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

static std::filesystem::path GetExecutableDirectoryLinux()
{
    // On macOS the dedicated helper uses _NSGetExecutablePath; on other
    // platforms it returns an empty path so we fall through to /proc/self/exe.
    if (auto macDir = Spark::MacOS::GetExecutableDirectory(); !macDir.empty())
        return macDir;

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
            // Use the error_code overload so hostile -game values (e.g. paths longer
            // than NAME_MAX, null bytes, unreadable parents) don't throw and escape
            // to main() as a FATAL.
            std::error_code ec;
            if (std::filesystem::exists(path, ec) && !ec)
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
        if (GetEngineRuntime().input)
            GetEngineRuntime().input->Update();
    });

    if (GetEngineRuntime().moduleManager && GetEngineRuntime().moduleManager->HasModules())
    {
        SPARK_GUARDED_UPDATE("Modules", "Core", {
            GetEngineRuntime().moduleManager->UpdateAll(dt);
            GetEngineRuntime().moduleManager->RenderAll();
        });
    }
    else if (GetEngineRuntime().graphics)
    {
        GetEngineRuntime().graphics->BeginFrame();
        GetEngineRuntime().graphics->EndFrame();
    }

    if (GetEngineRuntime().moduleHotReload)
        GetEngineRuntime().moduleHotReload->PollChanges();

    // Pump the audio engine: advances source state machine (stops finished
    // sources), applies 3D spatialization, and processes distance attenuation.
    // Pre-existing bug: AudioEngine::Update was never called from the main
    // loop on any platform.
    SPARK_GUARDED_UPDATE("Audio", "Core", {
        if (GetEngineRuntime().audioEngine)
            GetEngineRuntime().audioEngine->Update(dt);
    });

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
    if (GetEngineRuntime().eventBus)
    {
        s_weatherSystem.SetEventBus(GetEngineRuntime().eventBus.get());
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
        std::make_unique<EngineContext>(GetEngineRuntime().graphics.get(), GetEngineRuntime().input.get(),
                                        GetEngineRuntime().timer.get(), GetEngineRuntime().eventBus.get()));

    auto* ctx = EngineContext::Get();
    if (!ctx)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext is null after SetOwned — Linux init aborted");
        return;
    }

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

    if (!Spark::SaveSystem::GetInstance().Initialize("Saves"))
    {
        Spark::SimpleConsole::GetInstance().LogWarning("SaveSystem initialization failed — save/load unavailable");
    }
    ctx->SetSaveSystem(&Spark::SaveSystem::GetInstance());
    ctx->SetCoroutineScheduler(&Spark::CoroutineScheduler::GetInstance());

    // AssetPipeline (owned by GraphicsEngine, exposed via EngineContext for SDK access)
    if (GetEngineRuntime().graphics && GetEngineRuntime().graphics->GetAssetPipeline())
    {
        ctx->SetAssetPipeline(GetEngineRuntime().graphics->GetAssetPipeline());
    }

    // Initialize neural inference engine (GPU compute-based, no external ML deps)
    auto& neuralInference = Spark::Graphics::Neural::NeuralInferenceEngine::GetInstance();
    if (!neuralInference.Initialize())
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core, "NeuralInferenceEngine::Initialize failed — continuing without ML");
    }
    ctx->RegisterSystem<Spark::Graphics::Neural::NeuralInferenceEngine>(&neuralInference);

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
        GetEngineRuntime().audioEngine = std::make_unique<AudioEngine>();
        if (SUCCEEDED(GetEngineRuntime().audioEngine->Initialize(32)))
        {
            console.LogInfo("AudioEngine initialized (32 sources)");
            if (auto* ctx = EngineContext::Get())
                ctx->SetAudio(GetEngineRuntime().audioEngine.get());
        }
        else
        {
            console.LogWarning("AudioEngine initialization failed");
            GetEngineRuntime().audioEngine.reset();
        }

        // Create cross-platform audio backend (OpenAL on Linux, wraps AudioEngine on Windows)
        GetEngineRuntime().audioBackend = Spark::Audio::CreateAudioBackend(Spark::Audio::AudioBackendType::Auto,
                                                                           GetEngineRuntime().audioEngine.get());
    }

    GetEngineRuntime().moduleManager = std::make_unique<ModuleManager>();

    if (LoadGameModulesLinux(*GetEngineRuntime().moduleManager, argc, argv))
    {
        GetEngineRuntime().moduleManager->InitializeAll(EngineContext::Get());
        console.LogSuccess("Loaded " + std::to_string(GetEngineRuntime().moduleManager->GetModuleCount()) +
                           " module(s)");
    }
    else
    {
        console.LogWarning("No game modules found. Running engine-only mode.");
    }

    // Module hot-reload watcher
    GetEngineRuntime().moduleHotReload = std::make_unique<Spark::ModuleHotReloadManager>();
    GetEngineRuntime().moduleHotReload->Initialize(GetEngineRuntime().moduleManager.get(), EngineContext::Get());
    GetEngineRuntime().moduleHotReload->WatchAllLoadedModules();
    GetEngineRuntime().moduleHotReload->Start();

    // Console commands
    if (GetEngineRuntime().graphics)
        Spark::Graphics::RegisterGraphicsConsoleCommands(*GetEngineRuntime().graphics);
    Spark::RegisterEngineConsoleCommands(GetEngineRuntime().moduleManager.get(), GetEngineRuntime().audioEngine.get(),
                                         GetEngineRuntime().moduleHotReload.get());
    Spark::SubsystemFaultIsolator::GetInstance().RegisterConsoleCommands();
    Assert::RegisterConsoleCommands();

    LogMissingModuleWarnings();
}

/**
 * @brief Common shutdown sequence for all Linux startup paths.
 */
static void ShutdownLinux()
{
    GetEngineRuntime().moduleHotReload.reset();
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

    GetEngineRuntime().eventBus = std::make_unique<Spark::EventBus>();
    GetEngineRuntime().timer = std::make_unique<Timer>();

    // Headless: no gameplay subsystems (no Weather/UI/Dialogue/Modding)
    InitLinuxCoreSubsystems(/*registerGameplay=*/false);

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
        Spark::AssetStallDetector::GetInstance().RegisterConsoleCommands();
        Spark::NetworkHealthMonitor::GetInstance().RegisterConsoleCommands();
        Spark::GPUResourceLeakDetector::GetInstance().RegisterConsoleCommands();
        Spark::InvalidStateDetector::GetInstance().RegisterConsoleCommands();
        Assert::RegisterConsoleCommands();
    }
    else
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "RunHeadlessLinux: modules + detectors skipped (-minimal-init)");
    }

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
        float dt = GetEngineRuntime().timer ? GetEngineRuntime().timer->GetDeltaTime() : (1.0f / 60.0f);

        Spark::FixedTimestepAccumulator::GetInstance().Advance(dt);

        SPARK_GUARDED_UPDATE("Modules", "Core", {
            if (GetEngineRuntime().moduleManager && GetEngineRuntime().moduleManager->HasModules())
                GetEngineRuntime().moduleManager->UpdateAll(dt);
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
            if (GetEngineRuntime().graphics)
                GetEngineRuntime().graphics->OnResize(w, h);
            if (GetEngineRuntime().moduleManager)
                GetEngineRuntime().moduleManager->ResizeAll(w, h);
            if (GetEngineRuntime().eventBus)
                GetEngineRuntime().eventBus->Publish(
                    Spark::WindowResizeEvent{static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
        }
        break;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (GetEngineRuntime().input)
        {
            UINT msg = (event.type == SDL_KEYDOWN) ? WM_KEYDOWN : WM_KEYUP;
            int vk = TranslateSDLKeyToVK(event.key.keysym.sym);
            if (vk != 0)
                GetEngineRuntime().input->HandleMessage(msg, static_cast<WPARAM>(vk), 0);
        }
        break;

    case SDL_MOUSEMOTION:
        if (GetEngineRuntime().input)
            GetEngineRuntime().input->HandleMessage(
                WM_MOUSEMOVE, 0, static_cast<LPARAM>((event.motion.y << 16) | (event.motion.x & 0xFFFF)));
        break;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (GetEngineRuntime().input)
        {
            UINT msg = 0;
            if (event.button.button == SDL_BUTTON_LEFT)
                msg = (event.type == SDL_MOUSEBUTTONDOWN) ? WM_LBUTTONDOWN : WM_LBUTTONUP;
            else if (event.button.button == SDL_BUTTON_RIGHT)
                msg = (event.type == SDL_MOUSEBUTTONDOWN) ? WM_RBUTTONDOWN : WM_RBUTTONUP;
            else if (event.button.button == SDL_BUTTON_MIDDLE)
                msg = (event.type == SDL_MOUSEBUTTONDOWN) ? WM_MBUTTONDOWN : WM_MBUTTONUP;
            if (msg)
                GetEngineRuntime().input->HandleMessage(msg, 0, 0);
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
static void InitializeSDL2Subsystems(SDL_Window* window, void* nativeRenderHandle, int argc, char* argv[])
{
    auto& settings = EngineSettings::GetInstance();

    // Core engine objects
    GetEngineRuntime().timer = std::make_unique<Timer>();
    GetEngineRuntime().eventBus = std::make_unique<Spark::EventBus>();
    GetEngineRuntime().input = std::make_unique<InputManager>();
    GetEngineRuntime().input->Initialize(static_cast<HWND>(window));
    GetEngineRuntime().graphics = std::make_unique<GraphicsEngine>();

    // On macOS+Metal the RHI needs an NSView/CAMetalLayer, not the SDL_Window.
    // nativeRenderHandle overrides the window when set; otherwise the window
    // itself is passed through (Vulkan/OpenGL/Linux paths).
    void* rhiHandle = nativeRenderHandle ? nativeRenderHandle : static_cast<void*>(window);
    HRESULT hr = GetEngineRuntime().graphics->Initialize(static_cast<Spark::NativeWindowHandle>(rhiHandle));
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
    if (GetEngineRuntime().moduleManager)
    {
        auto* primary = GetEngineRuntime().moduleManager->GetPrimaryModule();
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
    Spark::InvalidStateDetector::GetInstance().RegisterConsoleCommands();
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

        float dt = GetEngineRuntime().timer ? GetEngineRuntime().timer->GetDeltaTime() : 0.016f;
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

    // Force SDL2 to use EGL instead of GLX on X11 so that OpenGLDevice's
    // existing "detect host-owned EGL context and reuse it" path (see
    // OpenGLDevice.cpp:884) kicks in. With the default GLX backend the
    // OpenGLDevice falls back to its own EGL pbuffer bootstrap, which
    // conflicts with SDL2's current GLX context and produces an
    // eglMakeCurrent EGL_BAD_ACCESS (0x3002) error before the engine
    // falls back to NullRHIDevice. Forcing EGL makes the host context
    // directly reusable and lets software rasterizers (Mesa llvmpipe)
    // drive the engine under Xvfb / Wayland without a GPU.
    SDL_SetHint("SDL_VIDEO_X11_FORCE_EGL", "1");
    SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
    {
        Spark::SimpleConsole::GetInstance().LogError(std::string("SDL_Init failed: ") + SDL_GetError());
        return -1;
    }

    auto& settings = EngineSettings::GetInstance();
    settings.Load();

    int winW = g_windowWidthOverride > 0 ? g_windowWidthOverride : settings.Graphics().windowWidth;
    int winH = g_windowHeightOverride > 0 ? g_windowHeightOverride : settings.Graphics().windowHeight;

    // Decide which graphics backend to use *before* creating the window.
    // SDL2 requires the backend-specific flag at window creation time:
    // SDL_WINDOW_VULKAN for Vulkan, SDL_WINDOW_OPENGL for OpenGL. There
    // is no way to retrofit a Vulkan surface onto an OpenGL window (or
    // vice versa) after the fact, so this decision is one-shot.
    //
    // GetRecommendedBackend() honors SPARK_DISABLE_VULKAN / _OPENGL /
    // _D3D11 env-var escape hatches, so users who hit a broken Vulkan
    // ICD can set SPARK_DISABLE_VULKAN=1 and this branch will pick
    // OpenGL instead — matching what the RHIBridge will also choose.
    bool preferVulkan = false;
    bool preferMetal = Spark::MacOS::ShouldPreferMetal();
    {
        const char* sdlDriver = SDL_GetCurrentVideoDriver();
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "SDL2 video driver: %s", sdlDriver ? sdlDriver : "<null>");

        const auto recommended = Spark::RHI::RHIBridge::GetRecommendedBackend();
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "RunSDL2Windowed: recommended backend = %s",
                       Spark::RHI::GetBackendName(recommended));

        // Drivers that don't support Vulkan at all (SDL_Vulkan_LoadLibrary
        // will always fail against them). Detect upfront so we don't even
        // attempt the Vulkan path — makes the log cleaner on headless CI
        // and sandboxed hosts where x11 isn't reachable.
        const bool driverHasVulkan =
            sdlDriver && (std::strcmp(sdlDriver, "x11") == 0 || std::strcmp(sdlDriver, "wayland") == 0 ||
                          std::strcmp(sdlDriver, "cocoa") == 0 || std::strcmp(sdlDriver, "windows") == 0 ||
                          std::strcmp(sdlDriver, "KMSDRM") == 0);

        if (!preferMetal && recommended == Spark::RHI::GraphicsBackend::Vulkan && driverHasVulkan)
        {
            // Try to load libvulkan through SDL. If it isn't installed
            // on this host, fall straight back to OpenGL and tell the
            // engine the same via the existing env-var opt-out, so
            // RHIBridge doesn't try to spin up VulkanDevice only to
            // discover libvulkan isn't there.
            if (SDL_Vulkan_LoadLibrary(nullptr) == 0)
            {
                preferVulkan = true;
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "SDL2 Vulkan loader initialized — creating Vulkan window");
            }
            else
            {
                SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                               "SDL_Vulkan_LoadLibrary failed: %s — falling back to OpenGL", SDL_GetError());
                setenv("SPARK_DISABLE_VULKAN", "1", /*overwrite=*/1);
            }
        }
        else if (recommended == Spark::RHI::GraphicsBackend::Vulkan && !driverHasVulkan)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                           "SDL2 driver '%s' has no Vulkan support — falling back to OpenGL",
                           sdlDriver ? sdlDriver : "<null>");
            setenv("SPARK_DISABLE_VULKAN", "1", /*overwrite=*/1);
        }
    }

    if (!preferVulkan && !preferMetal)
    {
        // OpenGL path — set attributes before window creation (required
        // for Mesa llvmpipe and other software rasterizers).
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    }

    Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (settings.Graphics().fullscreen)
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (preferMetal)
        windowFlags |= Spark::MacOS::GetMetalWindowFlag();
    else if (preferVulkan)
        windowFlags |= SDL_WINDOW_VULKAN;
    else
        windowFlags |= SDL_WINDOW_OPENGL;

    SDL_Window* window =
        SDL_CreateWindow("Spark Engine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW, winH, windowFlags);
    if (!window)
    {
        Spark::SimpleConsole::GetInstance().LogError(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        if (preferVulkan)
            SDL_Vulkan_UnloadLibrary();
        SDL_Quit();
        return -1;
    }

    // On macOS, extract a Metal-capable view so MetalDevice can attach a
    // CAMetalLayer. The helper returns an NSView (opaque void*) whose layer
    // is already a CAMetalLayer; we pass it through as the NativeWindowHandle
    // so MetalSwapChain::ConfigureMetalLayer() reuses it. On non-macOS the
    // call is a no-op that returns nullptr.
    void* sdlMetalView = nullptr;
    if (preferMetal)
    {
        sdlMetalView = Spark::MacOS::CreateMetalView(window);
        if (!sdlMetalView)
        {
            SDL_DestroyWindow(window);
            SDL_Quit();
            return -1;
        }
    }

    // OpenGL needs an SDL-owned GL context up front so the engine's
    // OpenGLDevice can detect it and skip its own EGL/GLX bootstrap.
    // Vulkan has no matching pre-init step — VulkanDevice pulls the
    // surface out of SDL_Vulkan_CreateSurface() inside CreateSwapChain().
    // Metal has no pre-init step either — the Metal view was created above.
    SDL_GLContext glContext = nullptr;
    if (!preferVulkan && !preferMetal)
    {
        glContext = SDL_GL_CreateContext(window);
        if (!glContext)
        {
            Spark::SimpleConsole::GetInstance().LogWarning(std::string("SDL_GL_CreateContext failed: ") +
                                                           SDL_GetError() + " — engine will try headless fallback");
        }
        else
        {
            SDL_GL_MakeCurrent(window, glContext);
            SDL_GL_SetSwapInterval(1);
            Spark::SimpleConsole::GetInstance().LogInfo("SDL2 OpenGL context created successfully");
        }
    }
    else
    {
        // Vulkan was preferred but RHIBridge may fall back to OpenGL if
        // VulkanDevice fails. Pre-set GL attributes so a context can be
        // created on a recreated window if needed (see fallback below).
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    }

    // If the engine was created with a Vulkan window but GraphicsEngine
    // fell back to OpenGL (Vulkan loaded but couldn't create a usable
    // surface/device), recreate the window with OpenGL flags and create
    // a GL context so the fallback backend can actually render.
    if (preferVulkan && GetEngineRuntime().graphics)
    {
        auto* rhiDevice = GetEngineRuntime().graphics->GetRHIDevice();
        bool vulkanActive = rhiDevice && rhiDevice->GetBackendType() == Spark::RHI::GraphicsBackend::Vulkan;
        auto* rhiBridge = GetEngineRuntime().graphics->GetRHIBridge();
        bool headless = rhiBridge && rhiBridge->IsHeadless();
        if (!vulkanActive && !headless)
        {
            Spark::SimpleConsole::GetInstance().LogInfo(
                "Vulkan backend unavailable — recreating window with OpenGL support");
            SDL_DestroyWindow(window);
            windowFlags = (windowFlags & ~SDL_WINDOW_VULKAN) | SDL_WINDOW_OPENGL;
            window = SDL_CreateWindow("Spark Engine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW, winH,
                                      windowFlags);
            if (window)
            {
                glContext = SDL_GL_CreateContext(window);
                if (glContext)
                {
                    SDL_GL_MakeCurrent(window, glContext);
                    SDL_GL_SetSwapInterval(1);
                }
                GetEngineRuntime().graphics->Shutdown();
                GetEngineRuntime().graphics->Initialize(static_cast<Spark::NativeWindowHandle>(window));
            }
        }
    }

    void* nativeRenderHandle = (preferMetal && sdlMetalView) ? sdlMetalView : nullptr;

    InitializeSDL2Subsystems(window, nativeRenderHandle, argc, argv);
    RunSDL2MainLoop();

    ShutdownLinux();
    if (glContext)
        SDL_GL_DeleteContext(glContext);
    Spark::MacOS::DestroyMetalView(sdlMetalView);
    SDL_DestroyWindow(window);
    if (preferVulkan)
        SDL_Vulkan_UnloadLibrary();
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

    // Minimal loop — process a few ticks to validate initialization, then exit
    for (int frame = 0; frame < 10 && !g_shutdownRequested; ++frame)
    {
        float dt = GetEngineRuntime().timer ? GetEngineRuntime().timer->GetDeltaTime() : 0.016f;
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
