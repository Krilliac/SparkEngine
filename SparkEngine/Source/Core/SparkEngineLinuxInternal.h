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
extern void InitPhysics();
extern void InitConsole();
extern void ShutdownPhysics();
extern bool CanShutdownEngine();
extern void ShutdownEngine();
extern void ShutdownEngineAfterPreflight();
extern void SetupCrashHandler();

/// Set by SignalHandler (SparkEngineLinux.cpp) on SIGINT/SIGTERM; polled by every main loop.
extern std::atomic<bool> g_shutdownRequested;

/// @brief Common per-frame tick logic shared by SDL2 windowed and no-SDL2 fallback modes.
void TickFrame(float dt);

/// @brief Initialize engine core subsystems common to all Linux startup paths.
void InitLinuxCoreSubsystems(bool registerGameplay);

/// @brief Load game modules, initialize hot-reload watcher, and register console commands.
void InitLinuxModulesAndCommands(int argc, char* argv[], bool initAudio);

/// @brief Common shutdown sequence for all Linux startup paths.
void ShutdownLinuxAfterPreflight();

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
