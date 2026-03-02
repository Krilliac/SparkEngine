/**
 * @file SparkEngine.h
 * @brief Main engine header containing global declarations and forward declarations
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file serves as the central header for the Spark Engine, providing forward
 * declarations for core engine systems and global variable declarations that are
 * shared across the entire engine framework.
 */

#pragma once
#include "resource.h"
#include <memory>

// Forward declarations
class GraphicsEngine;
class Game;
class InputManager;
class Timer;

/**
 * @brief Global application instance handle
 * 
 * Windows application instance handle used throughout the engine
 * for Win32 API calls and window management.
 */
extern HINSTANCE g_hInst;

/**
 * @brief Global graphics engine instance
 * 
 * Manages DirectX 11 rendering pipeline, device creation, swap chain,
 * render targets, and all graphics-related operations.
 */
extern std::unique_ptr<GraphicsEngine> g_graphics;

/**
 * @brief Global game instance
 * 
 * Main game loop controller that manages scene updates, rendering,
 * game objects, and coordinates between all engine systems.
 */
extern std::unique_ptr<Game> g_game;

/**
 * @brief Global input manager instance
 * 
 * Handles keyboard and mouse input processing, key mapping,
 * and provides input state queries for the game systems.
 */
extern std::unique_ptr<InputManager> g_input;

/**
 * @brief Global timer instance
 * 
 * High-precision timing system for delta time calculation,
 * frame rate management, and game loop timing control.
 */
extern std::unique_ptr<Timer> g_timer;