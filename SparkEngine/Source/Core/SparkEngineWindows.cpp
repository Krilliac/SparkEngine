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
#include "EngineRuntime.h"
#include "ModuleManager.h"
#include "GameImGuiLayer.h"
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
#include "Engine/ECS/Components.h"                 // ::World — engine-owned ECS world service
#include "SceneManager/ReflectedSceneSerializer.h" // -scene: Spark::LoadWorld
#include "Graphics/WorldBasicRenderer.h"           // -scene: Spark::RenderWorldBasic
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
#include "FaultIsolation.h"
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
#include <format>
#include <fstream>
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
 * @brief Parse -threads N from a wide command line string (Windows).
 *
 * Controls the size of the JobSystem worker pool. Returns 0 (meaning
 * "use the default of hardware_concurrency - 1") when the flag is not
 * provided. Primarily intended for running the engine under Wine on
 * a sandbox where every worker thread is another roll of the dice
 * against the gs.base race documented in
 * `.claude/knowledge/wine-gvisor-root-cause-found-2026-04-14.md` —
 * a developer can pass `-threads 1` to minimise the number of Wine
 * worker threads and maximise the chance of reaching the main loop
 * on a flaky run. Also honoured via the `SPARK_MAX_WORKER_THREADS`
 * environment variable (command-line wins on conflict).
 */
static uint32_t ParseThreadCount(LPWSTR cmdLine)
{
    // Env var fallback first so -threads overrides it when both are set.
    uint32_t fromEnv = 0;
    if (const char* env = std::getenv("SPARK_MAX_WORKER_THREADS"))
    {
        try
        {
            fromEnv = static_cast<uint32_t>(std::max(0, std::atoi(env)));
        }
        catch (...)
        {
        }
    }

    std::wstring cmd(cmdLine);
    auto pos = cmd.find(L"-threads");
    if (pos == std::wstring::npos)
        return fromEnv;
    pos += 8; // length of "-threads"
    while (pos < cmd.size() && cmd[pos] == L' ')
        ++pos;
    if (pos >= cmd.size())
        return fromEnv;
    try
    {
        return static_cast<uint32_t>(std::max(0, std::stoi(std::wstring(cmd.substr(pos)))));
    }
    catch (const std::exception&)
    {
        return fromEnv;
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

/**
 * @brief Parse -scene <path> from a wide command line string (Windows).
 *
 * When present and no game module ends up loaded, RunWindowedMainLoop
 * renders this reflected-scene JSON via the shared WorldBasicRenderer —
 * the automatable render path used by smoke tests and (later) the editor.
 */
static std::string ParseScenePathOverride(LPWSTR cmdLine)
{
    std::wstring cmd(cmdLine);
    auto pos = cmd.find(L"-scene");
    if (pos == std::wstring::npos)
        return {};
    pos += 6; // length of "-scene"
    while (pos < cmd.size() && cmd[pos] == L' ')
        ++pos;
    if (pos >= cmd.size())
        return {};
    auto end = cmd.find(L' ', pos);
    std::wstring wpath = cmd.substr(pos, end - pos);
    return std::string(wpath.begin(), wpath.end());
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
static std::string g_scenePath; ///< -scene <path>: reflected-scene JSON rendered when no game module loads

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
 * @brief Scripted console playback: -exec <file>
 *
 * Each non-empty, non-# line is "<frame> <console command>" or
 * "t<seconds> <console command>"; frame entries run once the main loop
 * reaches that frame, t-entries run once that much wall-clock time has
 * elapsed since the loop started. Time entries exist because frame rate
 * varies wildly (vsync + window occlusion), while gameplay (bot travel,
 * capture timers) runs on real dt — wall-clock scheduling keeps automated
 * smokes deterministic. Lines without a prefix run at frame 0. When mixing
 * both forms, ordering assumes 60 fps for the frame entries.
 */
struct ScriptedCommand
{
    int frame = 0;
    double atSec = -1.0; ///< >= 0: wall-clock scheduled ("t<seconds>" prefix)
    std::string command;
};
static std::vector<ScriptedCommand> g_execScript;
static size_t g_execScriptNext = 0;
static double g_testSecondsLimit = 0.0; ///< -test-seconds N: exit after N wall seconds

/// Wall-clock since the first due-check of the main loop (lazy start so boot
/// time is excluded from both t-entries and -test-seconds).
static double ExecElapsedSeconds()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

static void LoadExecScriptFromCmdLine(LPWSTR cmdLine)
{
    std::wstring cmd(cmdLine);
    size_t pos = cmd.find(L"-exec ");
    if (pos == std::wstring::npos)
        return;
    size_t start = pos + 6;
    size_t end = cmd.find(L' ', start);
    std::wstring wpath = cmd.substr(start, end - start);
    std::string path(wpath.begin(), wpath.end());

    std::ifstream file(path);
    if (!file)
    {
        Spark::SimpleConsole::GetInstance().LogError("[exec] cannot open script: " + path);
        return;
    }
    std::string line;
    while (std::getline(file, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        ScriptedCommand sc;
        size_t idx = 0;
        if (line[0] == 't' && line.size() > 1 && isdigit(static_cast<unsigned char>(line[1])))
        {
            // wall-clock entry: t<seconds> <command>
            idx = 1;
            while (idx < line.size() && (isdigit(static_cast<unsigned char>(line[idx])) || line[idx] == '.'))
                ++idx;
            if (idx < line.size() && line[idx] == ' ')
            {
                sc.atSec = std::stod(line.substr(1, idx - 1));
                sc.command = line.substr(idx + 1);
                g_execScript.push_back(sc);
                continue;
            }
            idx = 0; // not "t<num> cmd" after all — fall through as plain command
        }
        // optional leading frame number
        while (idx < line.size() && isdigit(static_cast<unsigned char>(line[idx])))
            ++idx;
        if (idx > 0 && idx < line.size() && line[idx] == ' ')
        {
            sc.frame = std::stoi(line.substr(0, idx));
            sc.command = line.substr(idx + 1);
        }
        else
        {
            sc.command = line;
        }
        g_execScript.push_back(sc);
    }
    // Unified ordering: t-entries by their time, frame entries at a nominal
    // 60 fps equivalence (scripts should stick to one form per phase anyway).
    auto sortKey = [](const ScriptedCommand& c) { return c.atSec >= 0.0 ? c.atSec : c.frame / 60.0; };
    std::stable_sort(g_execScript.begin(), g_execScript.end(),
                     [&sortKey](const ScriptedCommand& a, const ScriptedCommand& b)
                     { return sortKey(a) < sortKey(b); });
    Spark::SimpleConsole::GetInstance().LogInfo(
        std::format("[exec] loaded {} scripted commands from {}", g_execScript.size(), path));
}

static void RunDueScriptedCommands(int frameCount)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    const double elapsed = ExecElapsedSeconds();
    auto isDue = [&](const ScriptedCommand& sc)
    { return sc.atSec >= 0.0 ? elapsed >= sc.atSec : sc.frame <= frameCount; };
    while (g_execScriptNext < g_execScript.size() && isDue(g_execScript[g_execScriptNext]))
    {
        const std::string& c = g_execScript[g_execScriptNext].command;
        console.LogInfo(std::format("[exec] frame {} (t={:.1f}s): {}", frameCount, elapsed, c));
        const bool ok = console.ExecuteCommand(c);
        // Persist an audit trail for automated smoke runs: the engine has no
        // stdout and the file logger doesn't carry console traffic.
        // exec_audit.log (renamed from exec_results.log: that file has a
        // broken ACL from a force-killed run and can no longer be opened).
        std::ofstream results("exec_audit.log", std::ios::app);
        if (results)
        {
            results << "frame " << frameCount << " t=" << std::format("{:.1f}", elapsed) << "s | "
                    << (ok ? "ok " : "ERR") << " | " << c << '\n';
            // append the command's console output (new entries since execution)
            const auto& history = console.GetLogHistory();
            // first scripted command dumps the whole boot history (module
            // loading diagnostics); later ones append just their own output
            const size_t window = (g_execScriptNext == 0) ? history.size() : 8;
            const size_t start = history.size() > window ? history.size() - window : 0;
            for (size_t i = start; i < history.size(); ++i)
                results << "    > " << history[i].message << '\n';
        }
        ++g_execScriptNext;
    }
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
static int RunHeadlessWindows(LPWSTR lpCmdLine)
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
        Spark::FreezeDetector::GetInstance().Start();
        Spark::DeadlockDetector::GetInstance().RegisterConsoleCommands();
        Spark::HitchDetector::GetInstance().RegisterConsoleCommands();
        Spark::AssetStallDetector::GetInstance().RegisterConsoleCommands();
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

/**
 * @brief Initialize windowed-mode subsystems: engine context, physics, modules,
 *        audio, save system, console commands, and debug/gameplay systems.
 *
 * Called after the Win32 window has been created and InitInstance() succeeded.
 */
static void InitEngineContext()
{
    GetEngineRuntime().eventBus = std::make_unique<Spark::EventBus>();
    EngineContext::SetOwned(
        std::make_unique<EngineContext>(GetEngineRuntime().graphics.get(), GetEngineRuntime().input.get(),
                                        GetEngineRuntime().timer.get(), GetEngineRuntime().eventBus.get()));

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

    static Spark::AssetRegistry g_assetRegistry;
    ctx->SetAssetRegistry(&g_assetRegistry);

    // ECS world service. Nothing else provides one in module mode, yet
    // IEngineContext::GetWorld() is the documented way for game modules to
    // reach the ECS — without this, ECS-driven modules (e.g. TERRAFRONT)
    // silently got nullptr and fell back to degenerate non-ECS stubs.
    // Owned by g_engineEcsWorld (SparkEngine.cpp): ShutdownEngine destroys it
    // BEFORE module DLLs are unmapped — a static here destructed after
    // FreeLibrary and called into unmapped module-instantiated entt pools,
    // hanging shutdown inside the crash handler.
    extern std::unique_ptr<::World> g_engineEcsWorld;
    g_engineEcsWorld = std::make_unique<::World>();
    ctx->SetWorld(g_engineEcsWorld.get());

    if (GetEngineRuntime().graphics && GetEngineRuntime().graphics->GetAssetPipeline())
    {
        ctx->SetAssetPipeline(GetEngineRuntime().graphics->GetAssetPipeline());
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
    GetEngineRuntime().moduleManager = std::make_unique<ModuleManager>();
    auto& console = Spark::SimpleConsole::GetInstance();

    if (LoadGameModules(*GetEngineRuntime().moduleManager, lpCmdLine))
    {
        GetEngineRuntime().moduleManager->InitializeAll(EngineContext::Get());

        auto* primary = GetEngineRuntime().moduleManager->GetPrimaryModule();
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

        console.LogSuccess("Loaded " + std::to_string(GetEngineRuntime().moduleManager->GetModuleCount()) +
                           " module(s)");
    }
    else
    {
        console.LogWarning("No game modules found. Running engine-only mode.");
        console.LogInfo("Place a game DLL (e.g. SparkGame.dll) next to the engine executable,");
        console.LogInfo("use -game <path> on the command line,");
        console.LogInfo("or create a spark.modules.json manifest.");
    }

    GetEngineRuntime().moduleHotReload = std::make_unique<Spark::ModuleHotReloadManager>();
    GetEngineRuntime().moduleHotReload->Initialize(GetEngineRuntime().moduleManager.get(), EngineContext::Get());
    GetEngineRuntime().moduleHotReload->WatchAllLoadedModules();
    GetEngineRuntime().moduleHotReload->Start();
}

static void InitializeWindowedSubsystems(HINSTANCE hInstance, LPWSTR lpCmdLine)
{
    InitEngineContext();
    SPARK_HEARTBEAT();
    InitGameplaySubsystems();
    SPARK_HEARTBEAT();

    auto& console = Spark::SimpleConsole::GetInstance();

    if (!Spark::SaveSystem::GetInstance().Initialize("Saves"))
    {
        console.LogWarning("SaveSystem initialization failed — save/load unavailable");
    }
    console.LogInfo("SaveSystem initialized");

    // Initialize AudioEngine before modules so modules can use EngineContext::Get()->GetAudio()
    GetEngineRuntime().audioEngine = std::make_unique<AudioEngine>();
    if (SUCCEEDED(GetEngineRuntime().audioEngine->Initialize(32)))
    {
        console.LogInfo("AudioEngine initialized (32 sources)");
        if (auto* ctx = EngineContext::Get())
            ctx->SetAudio(GetEngineRuntime().audioEngine.get());
    }
    else
    {
        console.LogWarning("AudioEngine initialization failed - audio commands will be unavailable");
        GetEngineRuntime().audioEngine.reset();
    }

    // Create cross-platform audio backend (wraps AudioEngine on Windows, OpenAL on Linux)
    GetEngineRuntime().audioBackend =
        Spark::Audio::CreateAudioBackend(Spark::Audio::AudioBackendType::Auto, GetEngineRuntime().audioEngine.get());

    // Game-mode ImGui overlay: init BEFORE modules load so the exe context +
    // allocators can be injected into each module DLL at load time, then hook
    // the overlay render into GraphicsEngine's pre-present callback so module
    // HUDs (IModule::OnImGui) draw every presented frame — no editor involved.
    if (GetEngineRuntime().graphics)
    {
        auto* gfx = GetEngineRuntime().graphics.get();
        HWND hWnd = FindWindowW(g_szClass, nullptr);
        if (Spark::GameImGui::Init(hWnd, gfx->GetDevice(), gfx->GetContext()))
        {
            void *imguiCtx = nullptr, *allocFn = nullptr, *freeFn = nullptr, *userData = nullptr;
            Spark::GameImGui::GetInjectionData(&imguiCtx, &allocFn, &freeFn, &userData);
            ModuleManager::SetImGuiInjection(imguiCtx, allocFn, freeFn, userData);
            gfx->SetPrePresentHook(
                [](void*) { Spark::GameImGui::RenderOverlay(GetEngineRuntime().moduleManager.get()); }, nullptr);
            Spark::SimpleConsole::GetInstance().LogSuccess("Game-mode ImGui overlay initialized");
        }
        else
        {
            Spark::SimpleConsole::GetInstance().LogWarning(std::string("Game-mode ImGui overlay init FAILED (hwnd=") +
                                                           (hWnd ? "ok" : "null") + ")");
        }
    }

    SPARK_HEARTBEAT();
    LoadAndInitModules(lpCmdLine);
    SPARK_HEARTBEAT();

    if (GetEngineRuntime().graphics)
        Spark::Graphics::RegisterGraphicsConsoleCommands(*GetEngineRuntime().graphics);
    Spark::RegisterEngineConsoleCommands(GetEngineRuntime().moduleManager.get(), GetEngineRuntime().audioEngine.get(),
                                         GetEngineRuntime().moduleHotReload.get());
    Spark::SubsystemFaultIsolator::GetInstance().RegisterConsoleCommands();
    Spark::FreezeDetector::GetInstance().RegisterConsoleCommands();
    // Init is done — switch the startup watchdog (started in wWinMain with
    // lenient thresholds) over to the tighter runtime config and clear any
    // recovery flag a slow init stage may have raised.
    Spark::FreezeDetector::GetInstance().Stop();
    Spark::FreezeDetector::GetInstance().Configure(Spark::FreezeDetectorConfig{});
    Spark::FreezeDetector::GetInstance().Start();
    Spark::FreezeDetector::GetInstance().AcknowledgeRecovery();
    Spark::DeadlockDetector::GetInstance().RegisterConsoleCommands();
    Spark::HitchDetector::GetInstance().RegisterConsoleCommands();
    Spark::AssetStallDetector::GetInstance().RegisterConsoleCommands();
    Spark::NetworkHealthMonitor::GetInstance().RegisterConsoleCommands();
    Spark::GPUResourceLeakDetector::GetInstance().RegisterConsoleCommands();
    Spark::InvalidStateDetector::GetInstance().RegisterConsoleCommands();
    Assert::RegisterConsoleCommands();
    EngineSettings::GetInstance().RegisterConsoleCommands();

    LogMissingModuleWarnings();

    if (g_weatherSystem && GetEngineRuntime().eventBus)
    {
        g_weatherSystem->SetEventBus(GetEngineRuntime().eventBus.get());
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
    ASSERT(GetEngineRuntime().timer);

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("Starting main engine loop...");

    // -scene <path>: load a reflected-scene JSON and render it via the
    // shared WorldBasicRenderer when no game module took over rendering.
    // File-scope statics so they outlive this stack frame for every tick.
    static World g_sceneWorld;
    static Spark::WorldMeshCache g_sceneCache;
    bool haveModules = GetEngineRuntime().moduleManager && GetEngineRuntime().moduleManager->HasModules();
    if (!g_scenePath.empty() && !haveModules)
    {
        if (Spark::LoadWorld(g_sceneWorld, g_scenePath))
        {
            console.LogSuccess(std::format("[-scene] Loaded '{}' ({} entities)", g_scenePath,
                                           g_sceneWorld.GetRegistry().storage<entt::entity>().size()));
        }
        else
        {
            console.LogError(std::format("[-scene] Failed to load '{}'", g_scenePath));
            g_scenePath.clear();
        }
    }

    if (g_testFrameLimit > 0)
        console.LogInfo(std::format("Test mode: will exit after {} frames", g_testFrameLimit));

    int frameCount = 0;
    bool quitPosted = false;

    // Win32 message pump: PeekMessage with PM_REMOVE gives us non-blocking
    // message processing — the engine ticks in the else branch whenever
    // there are no pending OS messages (resize, input, focus, etc.).
    while (msg.message != WM_QUIT)
    {
        // Test frame limit: post WM_QUIT once, then KEEP pumping messages so
        // the quit is actually consumed. The old `continue` skipped PeekMessage,
        // spinning forever without SPARK_HEARTBEAT until the FreezeDetector
        // killed the process (exit code 1) on every -test-frames run.
        if (((g_testFrameLimit > 0 && frameCount >= g_testFrameLimit) ||
             (g_testSecondsLimit > 0.0 && ExecElapsedSeconds() >= g_testSecondsLimit)) &&
            !quitPosted)
        {
            console.LogInfo(
                std::format("[TEST] Limit reached (frame {} / t={:.1f}s). Exiting.", frameCount, ExecElapsedSeconds()));
            PostQuitMessage(0);
            quitPosted = true;
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
            float rawDt = GetEngineRuntime().timer ? GetEngineRuntime().timer->GetDeltaTime() : 0.016f;
            float dt = g_deltaSmoother.Smooth(rawDt);

            // Advance the global fixed-timestep accumulator so all systems can
            // query GetFixedStepCount() for deterministic fixed-rate updates.
            Spark::FixedTimestepAccumulator::GetInstance().Advance(rawDt);

            SPARK_GUARDED_UPDATE("Input", "Core", {
                if (GetEngineRuntime().input)
                    GetEngineRuntime().input->Update();
            });

            if (GetEngineRuntime().moduleManager && GetEngineRuntime().moduleManager->HasModules())
            {
                SPARK_GUARDED_UPDATE("Modules", "Core", {
                    GetEngineRuntime().moduleManager->UpdateAll(dt);
                    auto& fixedAcc = Spark::FixedTimestepAccumulator::GetInstance();
                    const float fixedDt = fixedAcc.GetFixedTimestep();
                    for (uint32_t i = fixedAcc.GetFixedStepCount(); i > 0; --i)
                        GetEngineRuntime().moduleManager->FixedUpdateAll(fixedDt);
                    GetEngineRuntime().moduleManager->RenderAll();
                });
            }
            else if (GetEngineRuntime().graphics)
            {
                // Engine-only mode: clear, optionally render a -scene, present.
                auto* gfx = GetEngineRuntime().graphics.get();
                gfx->BeginFrame();
                if (!g_scenePath.empty())
                {
                    // Fixed framing camera looking at the scene origin.
                    using namespace DirectX;
                    XMVECTOR eye = XMVectorSet(2.0f, 1.7f, -3.5f, 1.0f);
                    XMVECTOR at = XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f);
                    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
                    XMMATRIX view = XMMatrixLookAtLH(eye, at, up);
                    int fbW = g_windowWidthOverride > 0 ? g_windowWidthOverride : 1280;
                    int fbH = g_windowHeightOverride > 0 ? g_windowHeightOverride : 720;
                    float aspect = static_cast<float>(fbW) / static_cast<float>(fbH);
                    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), aspect, 0.1f, 6000.0f);
                    Spark::RenderWorldBasic(g_sceneWorld, *gfx, g_sceneCache, view, proj);
                }
                gfx->EndFrame();
            }

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
        }
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
    console.LogInfo("Shutting down...");
    g_fileCache.reset();

    // Tear down the game-mode ImGui overlay while the D3D device is still
    // alive, and unhook it so no EndFrame during teardown re-enters ImGui.
    if (GetEngineRuntime().graphics)
        GetEngineRuntime().graphics->SetPrePresentHook(nullptr, nullptr);
    Spark::GameImGui::Shutdown();

    ShutdownEngine();

    return static_cast<int>(msg.wParam);
}

// ===================================================================================
//                                    wWinMain
// ===================================================================================
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
    // SparkEngine is linked as a GUI-subsystem PE (add_executable(... WIN32)),
    // which means stdout/stderr/stdin handles are NOT automatically connected
    // to the parent terminal — under Wine in a console, fprintf(stderr, ...)
    // from wWinMain silently discards its output, making early-init crashes
    // invisible in `tools/wine-run.sh`. AttachConsole(ATTACH_PARENT_PROCESS)
    // hooks us up to the parent's console if there is one, and we rebind
    // stdio via freopen so the CRT's stderr is pointed at the right HANDLE.
    // On a native Windows double-click launch there's no parent console,
    // AttachConsole returns FALSE, and we fall back to the usual GUI
    // behaviour (nothing visible on stdio, which is what GUI apps do).
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        // Don't rebind stdin: under Wine in a headless sandbox there's no
        // interactive input, and CONIN$ can block during open.
    }
    // Initialize the unified Logger with a stderr sink as the *very first*
    // engine action — before SetupCrashHandler, before anything that could
    // fault — so any crash in EngineSettings or the crash handler install
    // path itself is visible. Previously this block lived after
    // SetupCrashHandler and a crash during settings load would leave us
    // with no output at all. Matches the Linux path ordering in
    // SparkEngineLinux.cpp::main. The later InitializeDebugSystemsImpl
    // ClearSinks()+AddSink() keeps this idempotent.
    {
        auto& earlyLogger = Spark::Logger::Get();
        earlyLogger.Initialize(/*enableAsync=*/false);
        earlyLogger.AddSink(std::make_unique<Spark::StderrSink>());
    }

    // Log whether we're under Wine so operators can tell at a glance
    // when debugging a cross-host issue. No-op on native Windows.
    Spark::LogWineEnvironmentIfApplicable();

    SPARK_TRACE_ENTER(Spark::LogCategory::Core);
    ASSERT(hInstance != nullptr);

    SetupCrashHandler();

    g_testFrameLimit = ParseTestFrameLimit(lpCmdLine);
    {
        // -test-seconds N: wall-clock exit for smokes whose gameplay runs on
        // real dt (frame counts are meaningless when fps varies with vsync).
        std::wstring cmd(lpCmdLine);
        if (auto pos = cmd.find(L"-test-seconds"); pos != std::wstring::npos)
        {
            pos += 13;
            while (pos < cmd.size() && cmd[pos] == L' ')
                ++pos;
            try
            {
                if (pos < cmd.size())
                    g_testSecondsLimit = std::max(0.0, std::stod(std::wstring(cmd.substr(pos))));
            }
            catch (const std::exception&)
            {
            }
        }
    }
    g_maxWorkerThreads = ParseThreadCount(lpCmdLine);
    g_noSubprocess = (std::wstring(lpCmdLine).find(L"-no-subprocess") != std::wstring::npos);
    LoadExecScriptFromCmdLine(lpCmdLine);
    g_minimalInit = (std::wstring(lpCmdLine).find(L"-minimal-init") != std::wstring::npos);
    g_noJobSystem = (std::wstring(lpCmdLine).find(L"-no-jobsystem") != std::wstring::npos);
    ParseWindowSizeOverride(lpCmdLine);
    g_scenePath = ParseScenePathOverride(lpCmdLine);

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

    // Start the freeze watchdog BEFORE window/graphics init so a wedge during
    // device creation or module load produces a log/dump instead of a silent
    // "frozen" window. Thresholds are lenient here — slow driver init is
    // normal — and terminateOnFreeze is off so a slow-but-alive init is never
    // killed. Reconfigured to runtime thresholds at the end of
    // InitializeWindowedSubsystems.
    {
        Spark::FreezeDetectorConfig startupCfg;
        startupCfg.warningThresholdSec = 10.0f;
        startupCfg.recoveryThresholdSec = 30.0f;
        startupCfg.crashThresholdSec = 120.0f;
        startupCfg.terminateOnFreeze = false;
        Spark::FreezeDetector::GetInstance().Configure(startupCfg);
        Spark::FreezeDetector::GetInstance().Start();
    }

    // Create window and init graphics/input/timer
    if (!InitInstance(hInstance, nCmdShow))
        return -1;
    SPARK_HEARTBEAT();

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

    GetEngineRuntime().timer = std::make_unique<Timer>();
    ASSERT(GetEngineRuntime().timer);

    GetEngineRuntime().graphics = std::make_unique<GraphicsEngine>();
    ASSERT(GetEngineRuntime().graphics);
    HRESULT hr = GetEngineRuntime().graphics->Initialize(hWnd);
    if (FAILED(hr))
    {
        wchar_t buf[256];
        swprintf_s(buf, L"Graphics initialization failed (HR=0x%08X)", static_cast<unsigned>(hr));
        MessageBoxW(hWnd, buf, L"Fatal Error", MB_ICONERROR);
        return FALSE;
    }

    // Apply VSync setting from INI
    GetEngineRuntime().graphics->Console_SetVSync(settings.Graphics().vsync);

    GetEngineRuntime().input = std::make_unique<InputManager>();
    ASSERT(GetEngineRuntime().input);
    GetEngineRuntime().input->Initialize(hWnd);

    // Apply input settings from INI
    GetEngineRuntime().input->Console_SetMouseSensitivity(settings.Controls().mouseSensitivity);
    GetEngineRuntime().input->Console_SetInvertMouseY(settings.Controls().invertMouseY);
    GetEngineRuntime().input->Console_SetMouseDeadZone(settings.Controls().mouseDeadZone);
    GetEngineRuntime().input->Console_SetRawMouseInput(settings.Controls().rawMouseInput);
    GetEngineRuntime().input->Console_SetMouseAcceleration(settings.Controls().mouseAcceleration);

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
    // Game-mode ImGui overlay gets first look at input so HUD menus
    // (spawn screen, map) are clickable. Gameplay input still flows to
    // InputManager below; ImGui only "captures" when hovering its widgets.
    Spark::GameImGui::HandleWndProc(hWnd, msg, wParam, lParam);

    switch (msg)
    {
    case WM_KEYDOWN:
    case WM_KEYUP:
        if (GetEngineRuntime().input)
        {
            // While gameplay owns the mouse (FPS look mode) it keeps the
            // keyboard too; otherwise a focused ImGui text field eats keys.
            if (GetEngineRuntime().input->IsMouseCaptured() || !Spark::GameImGui::WantsKeyboard())
                GetEngineRuntime().input->HandleMessage(msg, wParam, lParam);
        }
        break;

    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        if (GetEngineRuntime().input)
        {
            // Clicks on HUD widgets must not fall through into gameplay
            // capture — but once gameplay has captured the mouse it keeps
            // receiving input even if ImGui windows sit under the hidden
            // cursor. Button-up always flows so gameplay never sees a
            // stuck-down button when ImGui grabs capture mid-press.
            const bool buttonUp = (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP);
            if (GetEngineRuntime().input->IsMouseCaptured() || buttonUp || !Spark::GameImGui::WantsMouse())
                GetEngineRuntime().input->HandleMessage(msg, wParam, lParam);
        }
        break;

    case WM_SIZE:
        if (GetEngineRuntime().graphics)
            GetEngineRuntime().graphics->OnResize(LOWORD(lParam), HIWORD(lParam));
        if (GetEngineRuntime().moduleManager)
            GetEngineRuntime().moduleManager->ResizeAll(LOWORD(lParam), HIWORD(lParam));
        if (GetEngineRuntime().eventBus)
            GetEngineRuntime().eventBus->Publish(Spark::WindowResizeEvent{LOWORD(lParam), HIWORD(lParam)});
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
