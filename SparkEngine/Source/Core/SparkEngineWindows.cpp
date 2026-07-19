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
#include "Engine/Dialogue/DialogueSystem.h"
#include "Engine/Modding/ModSystem.h"
#include "Engine/UI/UISystem.h"
#include "Graphics/WeatherSystem.h"
#include "Utils/Assert.h"
#include "Utils/FreezeDetector.h"
#include "Utils/LocalFileCache.h"
#include "Utils/Logger.h"
#include "Utils/SparkConsole.h"
#include "Utils/Validate.h"
#include "Utils/WineDetection.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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

// Windows-specific globals (MAX_LOADSTRING lives in SparkEngineWindowsInternal.h)
HINSTANCE g_hInst;
WCHAR g_szTitle[MAX_LOADSTRING];
WCHAR g_szClass[MAX_LOADSTRING];
std::unique_ptr<Spark::LocalFileCache> g_fileCache;
std::unique_ptr<Spark::WeatherSystem> g_weatherSystem;
std::unique_ptr<Spark::UI::UISystem> g_uiSystem;
std::unique_ptr<Spark::DialogueSystem> g_dialogueSystem;
std::unique_ptr<Spark::ModSystem> g_modSystem;
std::string g_scenePath; ///< -scene <path>: reflected-scene JSON rendered when no game module loads

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

#endif // SPARK_PLATFORM_WINDOWS
