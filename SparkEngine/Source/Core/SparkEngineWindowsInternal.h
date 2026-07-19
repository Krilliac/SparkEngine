/**
 * @file SparkEngineWindowsInternal.h
 * @brief Internal declarations shared by the Windows entry-point files.
 *
 * Split from SparkEngineWindows.cpp to keep files under the ~500-line guideline.
 * Declares the cross-file globals owned by SparkEngine.cpp / SparkEngineWindows.cpp
 * and the startup/loop/module helpers shared by the windowed, headless, and
 * project-selector paths (SparkEngineWindows*.cpp). Not part of the public
 * engine API — include only from the SparkEngineWindows* entry-point files.
 */
#pragma once

#include "Platform.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS

class ModuleManager;

namespace Spark
{
    class LocalFileCache;
    class WeatherSystem;
    class DialogueSystem;
    class ModSystem;
    namespace UI
    {
        class UISystem;
    }
} // namespace Spark

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

// Windows-specific globals (defined in SparkEngineWindows.cpp; g_hInst is
// declared in SparkEngine.h)
constexpr int MAX_LOADSTRING = 100;
extern WCHAR g_szTitle[MAX_LOADSTRING];
extern WCHAR g_szClass[MAX_LOADSTRING];
extern std::unique_ptr<Spark::LocalFileCache> g_fileCache;
extern std::unique_ptr<Spark::WeatherSystem> g_weatherSystem;
extern std::unique_ptr<Spark::UI::UISystem> g_uiSystem;
extern std::unique_ptr<Spark::DialogueSystem> g_dialogueSystem;
extern std::unique_ptr<Spark::ModSystem> g_modSystem;
extern std::string g_scenePath;   ///< -scene <path>: reflected-scene JSON rendered when no game module loads
extern double g_testSecondsLimit; ///< -test-seconds N: exit after N wall seconds

/// Bare-launch project selector candidates (defined in SparkEngineWindowsModules.cpp).
extern std::vector<std::string> g_projectSelectorCandidates;

/// @brief Wall-clock since the first due-check of the main loop (lazy start).
double ExecElapsedSeconds();

/// @brief Run all -exec scripted commands due at this frame / wall-clock time.
void RunDueScriptedCommands(int frameCount);

/// @brief Find the module manifest or fall back to the project selector.
bool LoadGameModules(ModuleManager& manager, LPWSTR cmdLine);

#ifdef SPARK_HAS_IMGUI
/// @brief ImGui panel listing discovered game modules (bare-launch flow).
void DrawProjectSelectorPanel();
#endif // SPARK_HAS_IMGUI

/// @brief Consume a project-selector pick: load + init the chosen module.
void ConsumeProjectSelectorChoice();

/// @brief Initialize windowed-mode subsystems, load modules, register commands.
void InitializeWindowedSubsystems(HINSTANCE hInstance, LPWSTR lpCmdLine);

/// @brief Run the Win32 message pump and per-frame engine tick loop.
int RunWindowedMainLoop(HINSTANCE hInstance);

#ifdef SPARK_HEADLESS_SUPPORT
/// @brief Run the engine in headless/dedicated server mode (Windows).
int RunHeadlessWindows(LPWSTR lpCmdLine);
#endif // SPARK_HEADLESS_SUPPORT

// Win32 boilerplate (defined in SparkEngineWindowsWin32.cpp)
ATOM MyRegisterClass(HINSTANCE);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

#endif // SPARK_PLATFORM_WINDOWS
