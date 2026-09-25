/**
 * @file SparkEngineWindows.cpp
 * @brief Windows entry point (wWinMain), command-line parsing, and -exec script playback
 *
 * Split into SparkEngineWindows*.cpp to keep files under the ~500-line
 * guideline: windowed-subsystem init lives in SparkEngineWindowsInit.cpp,
 * module discovery + project selector in SparkEngineWindowsModules.cpp,
 * the headless loop in SparkEngineWindowsHeadless.cpp, and the Win32 message
 * loop + window boilerplate in SparkEngineWindowsWin32.cpp. Linux counterpart
 * lives in SparkEngineLinux.cpp. Shared globals and SetupCrashHandler stay in
 * SparkEngine.cpp.
 */
#include "SparkEngine.h"
#include "Platform.h"
#include "framework.h"
#include "SparkEngineWindowsInternal.h"
#include "WindowsCommandLine.h"
#include "RuntimePackage.h"
#include "StartupSplash.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Engine/Modding/ModSystem.h"
#include "Engine/UI/UISystem.h"
#include "EngineRuntime.h"
#include "Core/Lifecycle/GameplayLifecycleShared.h"
#include "Graphics/WeatherSystem.h"
#include "ModuleManager.h"
#include "Utils/Assert.h"
#include "Utils/FreezeDetector.h"
#include "Utils/LocalFileCache.h"
#include "Utils/Logger.h"
#include "Utils/MultiISA.h"
#include "Utils/SparkConsole.h"
#include "Utils/Validate.h"
#include "Utils/WineDetection.h"
#include <Spark/Version.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS
#include <shellapi.h>

namespace
{
    void WriteCommandOutput(std::string_view text)
    {
        // GUI-subsystem executables do not bind the CRT stdout stream to a
        // redirected parent pipe. CMake/PowerShell still provide an inherited
        // Win32 standard handle, so write to that handle directly for CLI
        // introspection and fall back to the CRT for attached consoles.
        const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (output != nullptr && output != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            if (WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                written == text.size())
                return;
        }

        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
    }
} // namespace

/**
 * @brief Parse -test-frames N from a wide command line string (Windows).
 */
static int ParseTestFrameLimit(LPWSTR cmdLine)
{
    // Exact-token parsing: substring matching used to fire on any argument that
    // merely contained the flag text (a module path, a scene name).
    const auto frames = Spark::Platform::FindWindowsCommandLineNumber<long long>(cmdLine, L"-test-frames");
    if (!frames)
        return 0;
    return static_cast<int>(std::clamp<long long>(*frames, 0, INT_MAX));
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

    // Exact token: `-threadsafe` must never be read as `-threads`.
    const auto threads = Spark::Platform::FindWindowsCommandLineNumber<long long>(cmdLine, L"-threads");
    if (!threads)
        return fromEnv;
    return static_cast<uint32_t>(std::clamp<long long>(*threads, 0, UINT_MAX));
}

/**
 * @brief Parse -window-size WxH from a wide command line string (Windows).
 */
static void ParseWindowSizeOverride(LPWSTR cmdLine)
{
    const auto sizeToken = Spark::Platform::FindWindowsCommandLineArgument(cmdLine, L"-window-size");
    if (!sizeToken)
        return;

    auto xPos = sizeToken->find(L'x');
    if (xPos == std::wstring::npos)
        xPos = sizeToken->find(L'X');
    if (xPos == std::wstring::npos)
        return;

    const auto width = Spark::Platform::ParseWholeWideNumber<long long>(sizeToken->substr(0, xPos));
    const auto height = Spark::Platform::ParseWholeWideNumber<long long>(sizeToken->substr(xPos + 1));
    if (!width || !height)
        return; // Ignore malformed window size arguments

    g_windowWidthOverride = static_cast<int>(std::clamp<long long>(*width, 320, INT_MAX));
    g_windowHeightOverride = static_cast<int>(std::clamp<long long>(*height, 240, INT_MAX));
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
    // WindowsCommandLine.h owns the tokenizer and the UTF-8 conversion; this
    // used to be a private duplicate of both.
    return Spark::Platform::FindWindowsCommandLineUtf8Argument(cmdLine, L"-scene").value_or(std::string{});
}

static std::string StartupSplashUtf8(const wchar_t* value)
{
    if (!value)
        return {};
    const int inputLength = static_cast<int>(wcslen(value));
    if (inputLength == 0)
        return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, inputLength, nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, inputLength, result.data(), length, nullptr,
                            nullptr) != length)
        return {};
    return result;
}

static Spark::StartupSplashContext BuildStartupSplashContext()
{
    Spark::StartupSplashContext context;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv)
    {
        context.arguments.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i)
            context.arguments.push_back(StartupSplashUtf8(argv[i]));
        LocalFree(argv);
    }
    context.executableDirectory = Spark::RuntimePackage::GetExecutableDirectory();
    context.headless = g_headlessMode;
    context.automatedTest = g_testFrameLimit > 0 || g_execScript.GetTestSecondsLimit() > 0.0;
    return context;
}

// Windows-specific globals (MAX_LOADSTRING lives in SparkEngineWindowsInternal.h)
HINSTANCE g_hInst;
WCHAR g_szTitle[MAX_LOADSTRING];
WCHAR g_szClass[MAX_LOADSTRING];
HWND g_mainWindow = nullptr;
std::unique_ptr<Spark::LocalFileCache> g_fileCache;
std::unique_ptr<Spark::WeatherSystem> g_weatherSystem;
std::unique_ptr<Spark::UI::UISystem> g_uiSystem;
std::unique_ptr<Spark::DialogueSystem> g_dialogueSystem;
std::unique_ptr<Spark::ModSystem> g_modSystem;
std::string g_scenePath; ///< -scene <path>: reflected-scene JSON rendered when no game module loads

/**
 * @brief Apply the authoritative engine/project caption to the main window.
 *
 * Module discovery can change the caption after InitInstance. Keeping title
 * construction here also gives startup and the late project-selector path the
 * same UTF-8-to-UTF-16 conversion and the same stored HWND.
 */
void ApplyRuntimeWindowCaption()
{
    std::wstring title = L"Spark Engine";
    auto& runtime = GetEngineRuntime();
    if (runtime.moduleManager)
        if (auto* primary = runtime.moduleManager->GetPrimaryModule())
        {
            const auto info = primary->GetModuleInfo();
            if (info.name && *info.name)
            {
                const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, info.name, -1, nullptr, 0);
                if (count > 1)
                {
                    std::wstring moduleName(static_cast<size_t>(count), L'\0');
                    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, info.name, -1, moduleName.data(), count);
                    moduleName.resize(static_cast<size_t>(count - 1));
                    title += L" - ";
                    title += moduleName;
                }
            }
        }

    if (g_mainWindow)
        SetWindowTextW(g_mainWindow, title.c_str());
}

#ifdef SPARK_HEADLESS_SUPPORT
/**
 * @brief Parse command line for -headless or -dedicated flags
 */
static bool ParseHeadlessFlag(LPWSTR cmdLine)
{
    // Exact tokens only: a `-game` path that merely contains "-headless" must
    // not switch the process into headless mode.
    return Spark::Platform::HasWindowsCommandLineOption(cmdLine, L"-headless") ||
           Spark::Platform::HasWindowsCommandLineOption(cmdLine, L"-dedicated");
}
#endif // SPARK_HEADLESS_SUPPORT

bool ShouldShowWindowsFatalDialog()
{
    return g_testFrameLimit <= 0 && g_execScript.GetTestSecondsLimit() <= 0.0;
}

/// -exec <file> and -exec-audit <path>; Core/ExecScript.h documents the script format.
static void LoadExecScriptFromCmdLine(LPWSTR cmdLine)
{
    const auto auditPath = Spark::Platform::FindWindowsCommandLineUtf8Argument(cmdLine, L"-exec-audit");
    if (auditPath && !auditPath->empty())
        g_execScript.SetAuditPath(*auditPath);

    const auto scriptPath = Spark::Platform::FindWindowsCommandLineUtf8Argument(cmdLine, L"-exec");
    if (scriptPath && !scriptPath->empty())
        g_execScript.LoadFile(*scriptPath, Spark::SimpleConsole::GetInstance());
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
    const HANDLE inheritedOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool hasRedirectedOutput = inheritedOutput != nullptr && inheritedOutput != INVALID_HANDLE_VALUE &&
                                     GetFileType(inheritedOutput) != FILE_TYPE_CHAR;
    bool attachedParentConsole = false;
    if (!hasRedirectedOutput && AttachConsole(ATTACH_PARENT_PROCESS))
    {
        attachedParentConsole = true;
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        // Don't rebind stdin: under Wine in a headless sandbox there's no
        // interactive input, and CONIN$ can block during open.
    }

    // BLD-100 / OD-04: refuse an x86-64 CPU below the SSE4.2 + POPCNT floor
    // before logging, crash hooks or any subsystem runs, so the user gets a
    // clear message rather than an illegal-instruction crash. A console or a
    // redirected stream receives it as text; a double-click launch has
    // neither, so it gets a message box.
    if (const std::string cpuFloorFailure = Spark::DescribeStableCpuFloorFailure(Spark::DetectCpuFeatures());
        !cpuFloorFailure.empty())
    {
        const std::string line = "SparkEngine: " + cpuFloorFailure + "\n";
        const HANDLE errorOutput = GetStdHandle(STD_ERROR_HANDLE);
        DWORD written = 0;
        if (attachedParentConsole)
        {
            std::fputs(line.c_str(), stderr);
            std::fflush(stderr);
        }
        else if (errorOutput == nullptr || errorOutput == INVALID_HANDLE_VALUE ||
                 !WriteFile(errorOutput, line.data(), static_cast<DWORD>(line.size()), &written, nullptr))
        {
            MessageBoxA(nullptr, cpuFloorFailure.c_str(), "SparkEngine", MB_OK | MB_ICONERROR);
        }
        return EXIT_FAILURE;
    }

    // Introspection must stay safe in staged packages and on machines without
    // graphics/audio drivers. Handle it before logging, crash hooks, settings,
    // package-root changes, or any subsystem initialization.
    const bool showHelp = Spark::Platform::HasWindowsCommandLineOption(lpCmdLine, L"--help") ||
                          Spark::Platform::HasWindowsCommandLineOption(lpCmdLine, L"-h") ||
                          Spark::Platform::HasWindowsCommandLineOption(lpCmdLine, L"-help");
    if (showHelp)
    {
        WriteCommandOutput(std::format("SparkEngine {}.{}.{}\n"
                                       "Usage: SparkEngine [options]\n\n"
                                       "Core options:\n"
                                       "  --help, -h                 Show this help and exit\n"
                                       "  --version                  Show the engine version and exit\n"
                                       "  -game <module>             Load a game module\n"
                                       "  -require-game              Fail if no game module initializes\n"
                                       "  -manifest <path>           Load a packaged runtime manifest\n"
                                       "  -scene <path>              Load a reflected-scene document\n"
                                       "  -headless, -dedicated      Run without a graphics window\n"
                                       "  -threads <count>           Set the worker-thread limit\n"
                                       "  -test-frames <count>       Exit after a fixed frame count\n"
                                       "  -window-size <WxH>         Override the initial window size\n",
                                       SPARK_ENGINE_VERSION_MAJOR, SPARK_ENGINE_VERSION_MINOR,
                                       SPARK_ENGINE_VERSION_PATCH));
        return 0;
    }
    if (Spark::Platform::HasWindowsCommandLineOption(lpCmdLine, L"--version") ||
        Spark::Platform::HasWindowsCommandLineOption(lpCmdLine, L"-version"))
    {
        WriteCommandOutput(std::format("SparkEngine {}.{}.{}\n", SPARK_ENGINE_VERSION_MAJOR, SPARK_ENGINE_VERSION_MINOR,
                                       SPARK_ENGINE_VERSION_PATCH));
        return 0;
    }

    // Initialize the unified Logger as the *very first* engine action — before
    // SetupCrashHandler, before anything that could fault — so any crash in
    // EngineSettings or the crash handler install path itself is visible.
    // Previously this block lived after SetupCrashHandler and a crash during
    // settings load would leave us with no output at all. Matches the Linux path
    // ordering in SparkEngineLinux.cpp::main.
    //
    // The full sink set (stderr + per-user log file + console bridge) goes in
    // here, not just stderr: wWinMain has no console attached, so everything
    // logged between here and InitializeDebugSystemsImpl — crash-handler install,
    // settings load, module manifest resolution, graphics bring-up — was written
    // to a stderr nobody reads and was missing from the log file entirely.
    // InstallEngineLogSinksImpl installs exactly once per process, so the later
    // InitializeDebugSystemsImpl call reuses this one instead of reopening.
    {
        auto& earlyLogger = Spark::Logger::Get();
        earlyLogger.Initialize(/*enableAsync=*/false);
        Spark::Core::Lifecycle::InstallEngineLogSinksImpl();
    }

    // Log whether we're under Wine so operators can tell at a glance
    // when debugging a cross-host issue. No-op on native Windows.
    Spark::LogWineEnvironmentIfApplicable();

    SPARK_TRACE_ENTER(Spark::LogCategory::Core);
    ASSERT(hInstance != nullptr);

    SetupCrashHandler();

    g_testFrameLimit = ParseTestFrameLimit(lpCmdLine);
    // -test-seconds N: wall-clock exit for smokes whose gameplay runs on
    // real dt (frame counts are meaningless when fps varies with vsync).
    if (const auto seconds = Spark::Platform::FindWindowsCommandLineNumber<double>(lpCmdLine, L"-test-seconds"))
        g_execScript.SetTestSecondsLimit(*seconds);
    g_maxWorkerThreads = ParseThreadCount(lpCmdLine);
    g_noSubprocess = Spark::Platform::HasWindowsCommandLineOption(lpCmdLine, L"-no-subprocess");
    LoadExecScriptFromCmdLine(lpCmdLine);
    g_minimalInit = Spark::Platform::HasWindowsCommandLineOption(lpCmdLine, L"-minimal-init");
    g_noJobSystem = Spark::Platform::HasWindowsCommandLineOption(lpCmdLine, L"-no-jobsystem");
    ParseWindowSizeOverride(lpCmdLine);
    g_scenePath = ParseScenePathOverride(lpCmdLine);

    // spark-cli packages are self-contained applications. Windows supplies the
    // caller's working directory when an .exe is double-clicked or started by
    // another process, so root package-relative scenes/saves at the executable.
    //
    // Only a RELATIVE command-line path forces us to keep the caller's working
    // directory, because that is the directory it is resolved against. Skipping
    // the anchor for absolute `-game`/`-manifest`/`-scene` arguments — the
    // documented and CI-used launch form — left a packaged runtime with zero
    // mounted Data/*.spk archives and scattered its Logs/Saves into whatever
    // directory the caller happened to be in.
    const auto isRelativeOptionPath = [lpCmdLine](std::wstring_view option)
    {
        const auto value = Spark::Platform::FindWindowsCommandLineUtf8Argument(lpCmdLine, option);
        return value && !value->empty() && std::filesystem::path(*value).is_relative();
    };
    const bool hasRelativeLaunchRoot = isRelativeOptionPath(L"-game") || isRelativeOptionPath(L"-manifest") ||
                                       (!g_scenePath.empty() && std::filesystem::path(g_scenePath).is_relative());
    if (!hasRelativeLaunchRoot)
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
    g_headlessMode = ParseHeadlessFlag(lpCmdLine);
    if (g_headlessMode)
        return RunHeadlessWindows(lpCmdLine);
#endif

    // A small CPU renderer keeps the launch signature independent of the
    // selected RHI and any video codec. Automated/headless starts are skipped
    // by policy, so smoke tests never inherit the 2.8-second delay.
    Spark::PlayStartupSplash(BuildStartupSplashContext());

    // Register window class and title
    ASSERT(MAX_LOADSTRING <= _countof(g_szClass) && MAX_LOADSTRING <= _countof(g_szTitle));
    wcscpy_s(g_szClass, MAX_LOADSTRING, L"SparkEngineWindowClass");
    wcscpy_s(g_szTitle, MAX_LOADSTRING, L"Spark Engine");

    ATOM cls = MyRegisterClass(hInstance);
    ASSERT_MSG(cls != 0, "MyRegisterClass failed");
    if (cls == 0)
    {
        if (ShouldShowWindowsFatalDialog())
            MessageBoxW(nullptr, L"RegisterClassExW failed", L"Fatal Error", MB_ICONERROR);
        else
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "RegisterClassExW failed during automated windowed startup");
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
    const bool lifecycleInitialized = InitializeWindowedSubsystems(hInstance, lpCmdLine);
    ApplyRuntimeWindowCaption();

    // A failed engine lifecycle has already rolled back its stages. Leave via the
    // ordinary windowed teardown (module preflight + reverse-order cleanup) and
    // report the failure instead of running a game on half-built systems.
    if (!lifecycleInitialized)
    {
        Spark::SimpleConsole::GetInstance().LogError(
            "Engine lifecycle failed to initialize; terminating with a failure status.");
        PostQuitMessage(1);
    }

    const size_t initializedModules =
        GetEngineRuntime().moduleManager ? GetEngineRuntime().moduleManager->GetInitializedModuleCount() : 0;

    // Keep the ordinary windowed teardown path authoritative even when an
    // explicitly required module failed to initialize. Posting WM_QUIT with a
    // nonzero status lets RunWindowedMainLoop perform its persistence preflight
    // and reverse-order cleanup before returning the failure to the caller.
    const bool requireGame = Spark::Platform::HasWindowsCommandLineOption(lpCmdLine, L"-require-game");
    const bool requiredGameMissing = requireGame && initializedModules == 0;
    if (requiredGameMissing)
    {
        Spark::SimpleConsole::GetInstance().LogError(
            "Required game module was not initialized; terminating with a failure status.");
        PostQuitMessage(2);
    }

    // Run the message pump + tick loop until WM_QUIT
    const int loopExitCode = RunWindowedMainLoop(hInstance);

    // Publish exactly one machine-readable record only after the ordinary
    // windowed teardown has destroyed the ModuleManager. The final snapshot
    // therefore proves successful callbacks across the complete manager
    // lifetime rather than merely proving that startup reached OnLoad.
    if (requireGame)
    {
        const ModuleManager::LifecycleEvidence evidence = ModuleManager::GetLastTeardownLifecycleEvidence();
        // Task 1 records the module's exact ModuleInfo name. The stable-v1
        // wire contract instead identifies the shipped DLL target so it stays
        // independent of display-name wording.
        if (const auto* record = evidence.FindModule("Spark Arena - Engine Showcase"))
        {
            WriteCommandOutput(std::format("SPARK_MODULE_LIFECYCLE module=SparkGameFPS create={} load={} update={} "
                                           "fixed={} render={} unload={} destroy={} faults={}\n",
                                           record->createModule, record->onLoad, record->onUpdate,
                                           record->onFixedUpdate, record->onRender, record->onUnload,
                                           record->destroyModule, record->faults));
        }
    }

    if (!lifecycleInitialized)
        return 1;
    return requiredGameMissing ? 2 : loopExitCode;
}

#endif // SPARK_PLATFORM_WINDOWS
