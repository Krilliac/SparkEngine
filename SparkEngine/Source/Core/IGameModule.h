/**
 * @file IGameModule.h
 * @brief Abstract interface that game DLLs must implement
 *
 * The engine loads game modules (DLLs/shared libraries) at runtime.
 * Each game module exports a CreateGameModule() function that returns
 * an IGameModule implementation. This is the primary contract between
 * the engine runtime and game code.
 *
 * Similar to Unreal Engine's game module system - the engine is the
 * executable host, and game logic lives in a dynamically loaded library.
 */

#pragma once

#include <string>

// Forward declarations - engine types the game module receives
class GraphicsEngine;
class InputManager;
class Timer;

/**
 * @brief Abstract interface for game modules loaded by the engine
 *
 * Game projects implement this interface in a shared library (DLL on Windows,
 * .so on Linux). The engine loads the library and calls CreateGameModule()
 * to get an instance, then drives the game loop through this interface.
 */
class IGameModule
{
public:
    virtual ~IGameModule() = default;

    /** @brief Get the display name of this game module */
    virtual const char* GetGameName() const = 0;

    /** @brief Get the version string of this game module */
    virtual const char* GetGameVersion() const = 0;

    /**
     * @brief Initialize game systems
     * @param graphics The engine's graphics system
     * @param input The engine's input system
     * @return true on success, false on failure
     */
    virtual bool Initialize(GraphicsEngine* graphics, InputManager* input) = 0;

    /** @brief Shut down game systems and release resources */
    virtual void Shutdown() = 0;

    /**
     * @brief Update game state for one frame
     * @param deltaTime Seconds since last frame
     */
    virtual void Update(float deltaTime) = 0;

    /** @brief Render the current frame */
    virtual void Render() = 0;

    /** @brief Called when the window is resized */
    virtual void OnResize(int width, int height) { (void)width; (void)height; }

    /** @brief Pause game simulation */
    virtual void Pause() {}

    /** @brief Resume game simulation */
    virtual void Resume() {}

    /** @brief Check if the game is currently paused */
    virtual bool IsPaused() const { return false; }
};

/**
 * @brief Function signature for the game module factory exported from game DLLs
 *
 * Every game DLL must export a function with this signature named "CreateGameModule".
 * The engine calls this after loading the DLL to get the game module instance.
 */
using CreateGameModuleFn = IGameModule* (*)();

/**
 * @brief Function signature for destroying the game module
 *
 * Every game DLL must export a function with this signature named "DestroyGameModule".
 * The engine calls this before unloading the DLL.
 */
using DestroyGameModuleFn = void (*)(IGameModule*);

