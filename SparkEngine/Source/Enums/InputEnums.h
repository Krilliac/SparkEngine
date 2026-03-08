/**
 * @file InputEnums.h
 * @brief Input system enumerations for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file contains all enumerations related to input handling,
 * including key codes, mouse buttons, and input states.
 */

#pragma once

namespace SparkEngine
{

    /**
 * @brief Input action types
 */
    enum class InputAction
    {
        PRESSED = 0,  ///< Input was just pressed
        RELEASED = 1, ///< Input was just released
        HELD = 2,     ///< Input is being held down
        REPEATED = 3  ///< Input is repeating (key repeat)
    };

    /**
 * @brief Mouse button types
 */
    enum class MouseButton
    {
        LEFT = 0,     ///< Left mouse button
        RIGHT = 1,    ///< Right mouse button
        MIDDLE = 2,   ///< Middle mouse button (wheel)
        BUTTON_4 = 3, ///< Mouse button 4
        BUTTON_5 = 4  ///< Mouse button 5
    };

    /**
 * @brief Gamepad buttons
 */
    enum class GamepadButton
    {
        A = 0,             ///< A button (Xbox) / X button (PlayStation)
        B = 1,             ///< B button (Xbox) / Circle button (PlayStation)
        X = 2,             ///< X button (Xbox) / Square button (PlayStation)
        Y = 3,             ///< Y button (Xbox) / Triangle button (PlayStation)
        LEFT_BUMPER = 4,   ///< Left bumper/shoulder button
        RIGHT_BUMPER = 5,  ///< Right bumper/shoulder button
        LEFT_TRIGGER = 6,  ///< Left trigger (digital)
        RIGHT_TRIGGER = 7, ///< Right trigger (digital)
        SELECT = 8,        ///< Select/Back button
        START = 9,         ///< Start/Options button
        LEFT_STICK = 10,   ///< Left stick click
        RIGHT_STICK = 11,  ///< Right stick click
        DPAD_UP = 12,      ///< D-pad up
        DPAD_DOWN = 13,    ///< D-pad down
        DPAD_LEFT = 14,    ///< D-pad left
        DPAD_RIGHT = 15    ///< D-pad right
    };

    /**
 * @brief Gamepad axes
 */
    enum class GamepadAxis
    {
        LEFT_STICK_X = 0,  ///< Left stick X axis
        LEFT_STICK_Y = 1,  ///< Left stick Y axis
        RIGHT_STICK_X = 2, ///< Right stick X axis
        RIGHT_STICK_Y = 3, ///< Right stick Y axis
        LEFT_TRIGGER = 4,  ///< Left trigger (analog)
        RIGHT_TRIGGER = 5  ///< Right trigger (analog)
    };

    /**
 * @brief Key modifier flags
 */
    enum class KeyModifier
    {
        NONE = 0,  ///< No modifiers
        CTRL = 1,  ///< Control key
        SHIFT = 2, ///< Shift key
        ALT = 4,   ///< Alt key
        SUPER = 8  ///< Super/Windows key
    };

    /**
 * @brief Input binding types
 */
    enum class BindingType
    {
        BUTTON = 0,  ///< Button/key binding
        AXIS = 1,    ///< Axis binding
        VECTOR2 = 2, ///< 2D vector binding
        VECTOR3 = 3  ///< 3D vector binding
    };

    /**
 * @brief Input context types
 */
    enum class InputContext
    {
        GAME = 0,   ///< Game input context
        UI = 1,     ///< UI input context
        EDITOR = 2, ///< Editor input context
        DEBUG = 3,  ///< Debug input context
        CONSOLE = 4 ///< Console input context
    };

    /**
 * @brief Cursor modes
 */
    enum class CursorMode
    {
        NORMAL = 0,  ///< Normal cursor
        HIDDEN = 1,  ///< Hidden cursor
        LOCKED = 2,  ///< Locked cursor (FPS mode)
        CONFINED = 3 ///< Confined cursor (stays in window)
    };

    /**
 * @brief Input device states
 */
    enum class DeviceState
    {
        DISCONNECTED = 0, ///< Device is disconnected
        CONNECTED = 1,    ///< Device is connected
        ERROR = 2         ///< Device has error
    };

    /**
 * @brief Touch gesture types
 */
    enum class GestureType
    {
        TAP = 0,        ///< Single tap
        DOUBLE_TAP = 1, ///< Double tap
        LONG_PRESS = 2, ///< Long press
        SWIPE = 3,      ///< Swipe gesture
        PINCH = 4,      ///< Pinch gesture
        ROTATE = 5      ///< Rotation gesture
    };

} // namespace SparkEngine
