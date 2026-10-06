/**
 * @file AdvancedConsoleCommands.h
 * @brief Console command declarations for unified GraphicsEngine
 *
 * The commands register through the host's public Spark::IConsole
 * (IEngineContext::GetConsole()); Game registers them once its engine context
 * is set and removes them in Game::Shutdown, before the FPS module unloads.
 * Game thread only.
 */

#pragma once

// Forward declarations
class Game;
class GraphicsEngine;

namespace Spark
{
    class IConsole;
}

namespace SparkConsole
{
    /**
     * @brief Register all advanced console commands for the unified GraphicsEngine
     * @param console The host console from IEngineContext::GetConsole()
     */
    void RegisterAdvancedCommands(Spark::IConsole& console, Game* game, GraphicsEngine* graphics);

    /** @brief Remove the commands RegisterAdvancedCommands registered on @p console, before the FPS DLL unloads. */
    void UnregisterAdvancedCommands(Spark::IConsole& console);
} // namespace SparkConsole
