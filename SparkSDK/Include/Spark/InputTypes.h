/**
 * @file InputTypes.h
 * @brief Input type definitions for game modules
 *
 * Provides key codes, mouse button identifiers, gamepad enums, and
 * input state structures that game modules use to query input from
 * the InputManager obtained via IEngineContext::GetInput().
 *
 * ## Usage
 * @code
 *   auto* input = context->GetInput();
 *   if (input->IsKeyPressed(Spark::Key::Space))
 *       player.Jump();
 *   if (input->IsMouseButtonDown(Spark::MouseButton::Left))
 *       player.Fire();
 * @endcode
 */

#pragma once

#include <cstdint>

namespace Spark
{

    /**
     * @brief Mouse button identifiers
     */
    enum class MouseButton : uint8_t
    {
        Left = 0,
        Right = 1,
        Middle = 2
    };

    /**
     * @brief 2D integer point for mouse position and movement deltas
     *
     * Supports C++17 structured bindings: auto [x, y] = mousePos;
     */
    struct MousePoint
    {
        int x = 0;
        int y = 0;
    };

    /**
     * @brief Gamepad button identifiers (Xbox layout)
     */
    enum class GamepadButton : uint8_t
    {
        A = 0,
        B,
        X,
        Y,
        LeftBumper,
        RightBumper,
        Back,
        Start,
        LeftStick,
        RightStick,
        DPadUp,
        DPadDown,
        DPadLeft,
        DPadRight
    };

    /**
     * @brief Gamepad axis identifiers
     */
    enum class GamepadAxis : uint8_t
    {
        LeftStickX = 0,
        LeftStickY,
        RightStickX,
        RightStickY,
        LeftTrigger,
        RightTrigger
    };

    /**
     * @brief Input action binding result
     *
     * Returned by the input manager when querying bound actions.
     */
    struct InputAction
    {
        bool pressed = false;  ///< True on the frame the action was first triggered
        bool held = false;     ///< True while the action is active
        bool released = false; ///< True on the frame the action was released
        float value = 0.0f;    ///< Analog value [0.0, 1.0] for axes/triggers
    };

} // namespace Spark
