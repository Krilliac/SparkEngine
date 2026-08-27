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
#include "Graphics/WeatherSystem.h"
#include "ModuleManager.h"
#include "Utils/Assert.h"
#include "Utils/FreezeDetector.h"
#include "Utils/LocalFileCache.h"
#include "Utils/Logger.h"
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
#include <fstream>
#include <memory>
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
    // lpCmdLine omits the executable. Prefix a dummy argv[0] so the system
    // tokenizer applies normal quoting/backslash rules to every user argument.
    const std::wstring fullCommandLine = L"SparkEngine.exe " + std::wstring(cmdLine ? cmdLine : L"");
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(fullCommandLine.c_str(), &argc);
    if (!argv)
        return {};

    std::wstring wpath;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::wstring_view(argv[i]) == L"-scene")
        {
            wpath = argv[i + 1];
            break;
        }
    }
    LocalFree(argv);
    if (wpath.empty() || wpath.size() > static_cast<size_t>(INT_MAX))
        return {};

    const int inputLength = static_cast<int>(wpath.size());
    const int utf8Length =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wpath.data(), inputLength, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0)
        return {};
    std::string utf8(static_cast<size_t>(utf8Length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wpath.data(), inputLength, utf8.data(), utf8Length, nullptr,
                            nullptr) != utf8Length)
    {
        return {};
    }
    return utf8;
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
    context.automatedTest = g_testFrameLimit > 0 || g_testSecondsLimit > 0.0;
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
    std::wstring cmd(cmdLine);
    return cmd.find(L"-headless") != std::wstring::npos || cmd.find(L"-dedicated") != std::wstring::npos;
}
#endif // SPARK_HEADLESS_SUPPORT

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
double g_testSecondsLimit = 0.0; ///< -test-seconds N: exit after N wall seconds

/// Wall-clock since the first due-check of the main loop (lazy start so boot
/// time is excluded from both t-entries and -test-seconds).
double ExecElapsedSeconds()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

static void LoadExecScriptFromCmdLine(LPWSTR cmdLine)
{
    const auto pathArgument = Spark::Platform::FindWindowsCommandLineUtf8Argument(cmdLine, L"-exec");
    if (!pathArgument || pathArgument->empty())
        return;
    const std::string& path = *pathArgument;

    std::ifstream file(std::filesystem::u8path(path));
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

void RunDueScriptedCommands(int frameCount)
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
    if (!hasRedirectedOutput && AttachConsole(ATTACH_PARENT_PROCESS))
    {
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        // Don't rebind stdin: under Wine in a headless sandbox there's no
        // interactive input, and CONIN$ can block during open.
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

    // spark-cli packages are self-contained applications. Windows supplies the
    // caller's working directory when an .exe is double-clicked or started by
    // another process, so root package-relative scenes/saves at the executable
    // unless the caller explicitly supplied a content/module location.
    const bool hasExplicitLaunchRoot = Spark::Platform::HasWindowsCommandLineOption(lpCmdLine, L"-game") ||
                                       Spark::Platform::HasWindowsCommandLineOption(lpCmdLine, L"-manifest") ||
                                       !g_scenePath.empty();
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
    ApplyRuntimeWindowCaption();

    // Run the message pump + tick loop until WM_QUIT
    return RunWindowedMainLoop(hInstance);
}

#endif // SPARK_PLATFORM_WINDOWS
