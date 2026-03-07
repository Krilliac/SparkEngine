/**
 * @file AdvancedConsoleCommands.h
 * @brief Console command declarations for unified GraphicsEngine
 */

#pragma once

// Forward declarations
class Game;
class GraphicsEngine;

namespace SparkConsole {
    /**
     * @brief Register all advanced console commands for the unified GraphicsEngine
     */
    void RegisterAdvancedCommands(Game* game, GraphicsEngine* graphics);
}
