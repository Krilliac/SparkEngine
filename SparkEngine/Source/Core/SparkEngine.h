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
 * ## Deprecation Notice
 * The global extern variables (g_graphics, g_input, etc.) are deprecated.
 * Use EngineContext / IEngineContext instead to access engine subsystems.
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

// ============================================================================
// DEPRECATED: Global subsystem pointers
// Use EngineContext (IEngineContext) instead. These will be removed in a
// future release.
// ============================================================================

/** @deprecated Use EngineContext::GetGraphics() instead */
[[deprecated("Use EngineContext::GetGraphics() instead of g_graphics")]]
inline GraphicsEngine* SparkGetGlobalGraphics();

/** @deprecated Use EngineContext::GetInput() instead */
[[deprecated("Use EngineContext::GetInput() instead of g_input")]]
inline InputManager* SparkGetGlobalInput();

/** @deprecated Use EngineContext::GetTimer() instead */
[[deprecated("Use EngineContext::GetTimer() instead of g_timer")]]
inline Timer* SparkGetGlobalTimer();

// Globals are still defined in SparkEngine.cpp for backward compatibility
// but new code should use EngineContext.
extern std::unique_ptr<GraphicsEngine> g_graphics;
extern std::unique_ptr<InputManager> g_input;
extern std::unique_ptr<Timer> g_timer;
extern std::unique_ptr<GameModuleLoader> g_moduleLoader;
