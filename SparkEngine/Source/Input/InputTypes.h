/**
 * @file InputTypes.h
 * @brief Shared input type definitions
 * @author Spark Engine Team
 * @date 2026
 */

#pragma once

/**
 * @brief 2D integer point for mouse position and movement deltas
 *
 * Supports C++17 structured bindings: auto [x, y] = input.GetMousePosition();
 */
struct MousePoint
{
    int x = 0;
    int y = 0;
};
