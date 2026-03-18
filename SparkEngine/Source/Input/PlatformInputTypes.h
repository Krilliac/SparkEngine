/**
 * @file PlatformInputTypes.h
 * @brief Type definitions, enums, and structs for the cross-platform input system
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains all platform-agnostic input type definitions used by PlatformInput.h
 * and its backends. Separated to allow lightweight inclusion of input types
 * without pulling in the full PlatformInputManager class hierarchy.
 *
 * @see PlatformInput.h
 */

#pragma once

#include <string>
#include <functional>
#include <cstdint>
#include <array>

namespace Spark::Input
{

    // ============================================================================
    // Platform-Agnostic Key Codes
    // ============================================================================

    /**
 * @brief Unified key code enumeration that maps identically across all platforms
 *
 * KeyCode provides a single set of identifiers for all keyboard keys, function
 * keys, modifier keys, numpad keys, and even mouse buttons. Platform backends
 * translate their native key codes (VK_* on Win32, SDL_SCANCODE_* on SDL2)
 * to this unified enum.
 *
 * Mouse buttons are included in the same enum to allow uniform binding in
 * the action mapping system.
 */
    enum class KeyCode : uint16_t
    {
        Unknown = 0, ///< Invalid / unrecognized key

        // Letters (using ASCII values for easy mapping)
        A = 'A',
        B = 'B',
        C = 'C',
        D = 'D',
        E = 'E',
        F = 'F',
        G = 'G',
        H = 'H',
        I = 'I',
        J = 'J',
        K = 'K',
        L = 'L',
        M = 'M',
        N = 'N',
        O = 'O',
        P = 'P',
        Q = 'Q',
        R = 'R',
        S = 'S',
        T = 'T',
        U = 'U',
        V = 'V',
        W = 'W',
        X = 'X',
        Y = 'Y',
        Z = 'Z',

        // Number row
        Num0 = '0',
        Num1 = '1',
        Num2 = '2',
        Num3 = '3',
        Num4 = '4',
        Num5 = '5',
        Num6 = '6',
        Num7 = '7',
        Num8 = '8',
        Num9 = '9',

        // Function keys
        F1 = 256,
        F2,
        F3,
        F4,
        F5,
        F6,
        F7,
        F8,
        F9,
        F10,
        F11,
        F12,

        // Navigation keys
        Escape = 280,
        Enter,
        Tab,
        Backspace,
        Insert,
        Delete,
        Right,
        Left,
        Down,
        Up,
        PageUp,
        PageDown,
        Home,
        End,

        // Modifier keys
        LeftShift = 300,
        RightShift,
        LeftCtrl,
        RightCtrl,
        LeftAlt,
        RightAlt,
        LeftSuper,
        RightSuper,

        // Punctuation and misc keys
        Space = 320,
        Comma,
        Period,
        Slash,
        Semicolon,
        Apostrophe,
        LeftBracket,
        RightBracket,
        Backslash,
        GraveAccent,
        Minus,
        Equal,

        // Numpad keys
        Numpad0 = 350,
        Numpad1,
        Numpad2,
        Numpad3,
        Numpad4,
        Numpad5,
        Numpad6,
        Numpad7,
        Numpad8,
        Numpad9,
        NumpadDecimal,
        NumpadDivide,
        NumpadMultiply,
        NumpadSubtract,
        NumpadAdd,
        NumpadEnter,

        // Mouse buttons (unified with keyboard for action binding convenience)
        MouseLeft = 400,
        MouseRight,
        MouseMiddle,
        MouseButton4,
        MouseButton5, ///< Extra mouse buttons (side buttons)

        MaxKeyCode = 512 ///< Sentinel for array sizing — not a valid key
    };

    // ============================================================================
    // Gamepad Abstraction
    // ============================================================================

    /**
 * @brief Platform-agnostic gamepad axis identifiers
 *
 * Represents the analog axes on a standard gamepad. Axis values are
 * normalized to [-1, 1] for sticks and [0, 1] for triggers.
 */
    enum class GamepadAxis : uint8_t
    {
        LeftStickX,   ///< Left thumbstick horizontal axis (-1 = left, +1 = right)
        LeftStickY,   ///< Left thumbstick vertical axis (-1 = down, +1 = up)
        RightStickX,  ///< Right thumbstick horizontal axis
        RightStickY,  ///< Right thumbstick vertical axis
        LeftTrigger,  ///< Left trigger (0 = released, 1 = fully pressed)
        RightTrigger, ///< Right trigger (0 = released, 1 = fully pressed)
        Count         ///< Sentinel for array sizing
    };

    /**
 * @brief Platform-agnostic gamepad button identifiers
 *
 * Standard button layout matching Xbox controller conventions.
 */
    enum class GamepadBtn : uint8_t
    {
        A,
        B,
        X,
        Y, ///< Face buttons
        LeftBumper,
        RightBumper, ///< Shoulder bumpers
        Back,
        Start,
        Guide, ///< Menu buttons
        LeftStick,
        RightStick, ///< Thumbstick clicks
        DPadUp,
        DPadDown,
        DPadLeft,
        DPadRight, ///< D-pad directional buttons
        Count      ///< Sentinel for array sizing
    };

    /**
 * @brief Per-gamepad state snapshot with connection info, axes, and button states
 *
 * Stores the current and previous frame's button states to enable edge
 * detection (just-pressed / just-released queries).
 */
    struct GamepadState
    {
        bool connected = false; ///< Whether this gamepad is currently connected
        std::string name;       ///< Human-readable gamepad name (e.g., "Xbox Wireless Controller")
        std::array<float, static_cast<size_t>(GamepadAxis::Count)> axes{};      ///< Current axis values
        std::array<bool, static_cast<size_t>(GamepadBtn::Count)> buttons{};     ///< Current button states
        std::array<bool, static_cast<size_t>(GamepadBtn::Count)> prevButtons{}; ///< Previous frame's button states
    };

    // ============================================================================
    // Input Action Mapping
    // ============================================================================

    /**
 * @brief When an action binding should trigger relative to the button press
 */
    enum class ActionTrigger
    {
        Pressed,  ///< Triggers on the frame the button is first pressed (rising edge)
        Released, ///< Triggers on the frame the button is released (falling edge)
        Held      ///< Triggers every frame while the button is held down
    };

    /**
 * @brief Binding record linking a named action to a keyboard key or gamepad button
 */
    struct ActionBinding
    {
        std::string actionName;                         ///< Name of the game action
        KeyCode key = KeyCode::Unknown;                 ///< Keyboard key (when useGamepad == false)
        GamepadBtn gamepadButton = GamepadBtn::A;       ///< Gamepad button (when useGamepad == true)
        bool useGamepad = false;                        ///< Whether this binding uses gamepad input
        ActionTrigger trigger = ActionTrigger::Pressed; ///< When the action should fire
    };

    /**
 * @brief Binding record for analog axis input from key pairs or gamepad axes
 *
 * Keyboard axes are simulated by combining a positive and negative key
 * (e.g., W/S for forward/backward) into a [-1, 1] value.
 */
    struct AxisBinding
    {
        std::string axisName;                              ///< Name of the game axis
        KeyCode positiveKey = KeyCode::Unknown;            ///< Key that produces +1 value
        KeyCode negativeKey = KeyCode::Unknown;            ///< Key that produces -1 value
        GamepadAxis gamepadAxis = GamepadAxis::LeftStickX; ///< Gamepad axis (when useGamepad == true)
        bool useGamepad = false;                           ///< Whether this binding uses gamepad input
        float scale = 1.0f;                                ///< Multiplier applied to the axis value
        float deadZone = 0.15f;                            ///< Minimum gamepad axis value to register
    };

    // ============================================================================
    // Input Events
    // ============================================================================

    /**
 * @brief Generic input event for callback-based input handling
 *
 * Encodes any input event (key press, mouse move, gamepad input, text input)
 * into a single struct that can be dispatched to registered callbacks.
 */
    struct InputEvent
    {
        /** @brief Type of input event */
        enum class Type
        {
            KeyDown,
            KeyUp, ///< Keyboard key press / release
            MouseMove,
            MouseScroll, ///< Mouse movement or scroll wheel
            GamepadButton,
            GamepadAxis, ///< Gamepad button or axis change
            TextInput    ///< Unicode text character input
        };

        Type type;                                         ///< The event type
        KeyCode key = KeyCode::Unknown;                    ///< Key involved (for KeyDown/KeyUp events)
        int mouseX = 0, mouseY = 0;                        ///< Absolute mouse position in window coordinates
        int mouseDeltaX = 0, mouseDeltaY = 0;              ///< Relative mouse movement since last frame
        float scrollDelta = 0.0f;                          ///< Mouse scroll wheel delta
        int gamepadIndex = 0;                              ///< Gamepad slot index for gamepad events
        GamepadBtn gamepadButton = GamepadBtn::A;          ///< Gamepad button (for GamepadButton events)
        GamepadAxis gamepadAxis = GamepadAxis::LeftStickX; ///< Gamepad axis (for GamepadAxis events)
        float axisValue = 0.0f;                            ///< Axis value for GamepadAxis events
        char32_t textChar = 0;                             ///< Unicode character for TextInput events
    };

    /** @brief Callback function type for input event notifications */
    using InputEventCallback = std::function<void(const InputEvent&)>;

} // namespace Spark::Input
