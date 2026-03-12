/**
 * @file SparkEngine.h
 * @brief Main engine header - SparkEngine is the executable runtime host
 * @author Spark Engine Team
 * @date 2025
 *
 * SparkEngine is the executable that hosts the runtime. It initializes all
 * engine systems (graphics, input, audio, etc.) and loads game modules
 * (DLLs/shared libraries) at runtime via the IGameModule interface.
 *
 * This architecture is similar to Unreal Engine: the engine is the exe,
 * game logic lives in a dynamically loaded module. The editor can trigger
 * recompilation of the game DLL and hot-reload it.
 *
 * Game code should access subsystems via EngineContext / IEngineContext.
 */

#pragma once
#include "resource.h"
#include "Platform.h"
#include <memory>

// Forward declarations for engine systems
class GraphicsEngine;
class InputManager;
class Timer;
class SparkEngineCamera;
class GameModuleLoader;
class IGameModule;

#ifdef SPARK_PLATFORM_WINDOWS
extern HINSTANCE g_hInst;
#endif

// Headless/dedicated server mode flag
#ifdef SPARK_HEADLESS_SUPPORT
extern bool g_headlessMode;
#endif

// Engine-owned subsystem instances are internal to SparkEngine.cpp.
// Game code should access subsystems via EngineContext::Get() exclusively.
