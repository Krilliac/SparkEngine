/**
 * @file SparkEngineLinuxInternal.h
 * @brief Internal declarations shared by the POSIX (Linux + macOS) entry-point files.
 *
 * Split from SparkEngineLinux.cpp to keep files under the ~500-line guideline.
 * Declares the cross-file globals owned by SparkEngine.cpp / SparkEngineLinux.cpp
 * and the startup/tick/shutdown helpers shared by the headless, SDL2 windowed,
 * and no-SDL2 fallback paths (SparkEngineLinux*.cpp). Not part of the public
 * engine API — include only from the SparkEngineLinux* entry-point files.
 */
#pragma once

#include "Platform.h"
#include "ExecScript.h"

#include <atomic>
#include <cstdint>

#ifndef SPARK_PLATFORM_WINDOWS

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
extern Spark::ExecScriptPlayer g_execScript; ///< -exec / -exec-audit / -test-seconds playback
extern void InitPhysics();
extern bool InitConsole();
extern void ShutdownPhysics();
extern bool CanShutdownEngine();
extern void ShutdownEngine();
extern bool ShutdownEngineAfterPreflight();
extern void SetupCrashHandler();

/// Set by SignalHandler (SparkEngineLinux.cpp) on SIGINT/SIGTERM; polled by every main loop.
extern std::atomic<bool> g_shutdownRequested;

/// @brief Common per-frame tick logic shared by SDL2 windowed and no-SDL2 fallback modes.
void TickFrame(float dt);

/// @brief Initialize engine core subsystems common to all Linux startup paths.
void InitLinuxCoreSubsystems(bool registerGameplay);

/// @brief Load game modules, initialize hot-reload watcher, and register console commands.
void InitLinuxModulesAndCommands(int argc, char* argv[], bool initAudio);

/**
 * @brief Load the reflected scene named by `-scene <path>` into the engine ECS world.
 *
 * Call after InitLinuxModulesAndCommands (or instead of it under -minimal-init).
 * On success prints one `SPARK_SCENE_LOADED entities=N renderables=M` record to
 * stdout. The caller must fail the launch when this returns false.
 *
 * @return true when no -scene was given or the scene loaded; false otherwise.
 */
bool LoadLinuxLaunchScene(int argc, char* argv[]);

/// Process exit status when `-scene` names a scene that cannot be loaded.
inline constexpr int kLinuxSceneLoadFailedExitCode = 4;

/// @brief True when @p flag appears verbatim in argv[1..argc).
bool HasLinuxCommandLineFlag(int argc, char* argv[], const char* flag);

/// @brief Common shutdown sequence for all Linux startup paths.
/// @return false when the engine lifecycle teardown was not clean (exit non-zero).
bool ShutdownLinuxAfterPreflight();

#ifdef SPARK_HEADLESS_SUPPORT
/// @brief Run the engine in headless/dedicated server mode (Linux).
int RunHeadlessLinux(int argc, char* argv[]);
#endif // SPARK_HEADLESS_SUPPORT

#ifdef SPARK_SDL2_AVAILABLE
/// @brief Run the SDL2 event pump and per-frame engine tick loop.
void RunSDL2MainLoop(bool pollSdlEvents);

/// @brief Run the engine in SDL2 windowed mode (Linux).
int RunSDL2Windowed(int argc, char* argv[]);
#else
/// @brief Run the engine without SDL2 (no-window fallback).
int RunNoSDL2Fallback(int argc, char* argv[]);
#endif // SPARK_SDL2_AVAILABLE

#endif // !SPARK_PLATFORM_WINDOWS
