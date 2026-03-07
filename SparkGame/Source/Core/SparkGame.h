/**
 * @file SparkGame.h
 * @brief Main game header with global declarations for the game executable
 * @author Spark Engine Team
 * @date 2025
 *
 * This file contains the global variable declarations for the game executable.
 * Engine systems are provided by the SparkEngine static library.
 */

#pragma once
#include "Core/resource.h"
#include "Core/Platform.h"
#include <memory>

// Forward declarations
class GraphicsEngine;
class Game;
class InputManager;
class Timer;

#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @brief Global application instance handle (Windows only)
 */
extern HINSTANCE g_hInst;
#endif

/**
 * @brief Global graphics engine instance
 */
extern std::unique_ptr<GraphicsEngine> g_graphics;

/**
 * @brief Global game instance
 */
extern std::unique_ptr<Game> g_game;

/**
 * @brief Global input manager instance
 */
extern std::unique_ptr<InputManager> g_input;

/**
 * @brief Global timer instance
 */
extern std::unique_ptr<Timer> g_timer;
