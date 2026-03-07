/**
 * @file GraphicsConsoleCommands.h
 * @brief Registers graphics-related console commands
 *
 * Extracted from GraphicsEngine to separate console integration from
 * rendering responsibilities (Single Responsibility Principle).
 */

#pragma once

class GraphicsEngine;

namespace Spark::Graphics {

/**
 * @brief Register all graphics console commands with the engine console
 * @param engine The graphics engine instance to operate on
 *
 * Registers commands: gfx_vsync, gfx_wireframe, gfx_metrics, gfx_screenshot
 */
void RegisterGraphicsConsoleCommands(GraphicsEngine& engine);

} // namespace Spark::Graphics
