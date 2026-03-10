/**
 * @file PlatformInput.h
 * @brief Cross-platform input abstraction layer with pluggable backends
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a platform-agnostic input interface that abstracts over
 * platform-specific input APIs through a backend plugin architecture:
 *
 * - **Win32InputBackend** — Win32 raw input + XInput (current default)
 * - **SDL2InputBackend** — SDL2 event system (compiled when SPARK_SDL2_AVAILABLE is defined)
 * - Future: Linux evdev, libinput, etc.
 *
 * This layer sits between game code and the platform-specific input APIs,
 * providing:
 * - Unified key codes (KeyCode enum) that map identically across platforms
 * - Gamepad abstraction with axis/button queries
 * - Action mapping that binds named game actions to keys or gamepad inputs
 * - Axis mapping that combines positive/negative keys into analog-style values
 * - Input event callbacks for event-driven architectures
 *
 * ## Architecture
 *
 * ```
 * Game Code
 *    |
 *    v
 * PlatformInputManager (singleton, unified API)
 *    |
 *    v
 * IPlatformInputBackend (interface)
 *    |
 *    +-- Win32InputBackend (Windows + XInput)
 *    +-- SDL2InputBackend  (cross-platform, optional)
 * ```
 *
 * Typical usage:
 * @code
 *   auto& input = Spark::Input::PlatformInputManager::GetInstance();
 *   input.Initialize(hwnd);  // auto-detects best backend
 *
 *   input.BindAction("Jump", KeyCode::Space);
 *   input.BindAction("Jump", GamepadBtn::A);
 *   input.BindAxis("MoveForward", KeyCode::W, KeyCode::S);
 *
 *   // In game loop:
 *   input.Update();
 *   if (input.IsActionActive("Jump")) player.Jump();
 *   float forward = input.GetAxisValue("MoveForward");
 * @endcode
 *
 * @see InputManager, GamepadInput
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
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

    // ============================================================================
    // Platform Input Backend Interface
    // ============================================================================

    /**
 * @brief Abstract interface that all platform input backends must implement
 *
 * Each backend translates platform-specific input APIs (Win32, SDL2, etc.)
 * into the unified KeyCode/GamepadAxis/GamepadBtn types. Backends are
 * instantiated and managed by PlatformInputManager.
 */
    class IPlatformInputBackend
    {
      public:
        virtual ~IPlatformInputBackend() = default;

        /**
     * @brief Initialize the input backend with a native window handle
     * @param windowHandle Platform-specific window handle (HWND on Windows, SDL_Window* on SDL2)
     * @return true if initialization succeeded, false on failure
     */
        virtual bool Initialize(void* windowHandle) = 0;

        /** @brief Shut down the backend and release platform resources */
        virtual void Shutdown() = 0;

        /** @brief Poll for new input events (called once per frame by the manager) */
        virtual void PollEvents() = 0;

        /** @brief Check if a key is currently held down @param key The key to query @return true if held */
        virtual bool IsKeyDown(KeyCode key) const = 0;

        /** @brief Check if a key was pressed this frame @param key The key to query @return true if just pressed */
        virtual bool WasKeyPressed(KeyCode key) const = 0;

        /** @brief Check if a key was released this frame @param key The key to query @return true if just released */
        virtual bool WasKeyReleased(KeyCode key) const = 0;

        /** @brief Get the current mouse position @param x [out] X position @param y [out] Y position */
        virtual void GetMousePosition(int& x, int& y) const = 0;

        /** @brief Get mouse movement since last frame @param dx [out] X delta @param dy [out] Y delta */
        virtual void GetMouseDelta(int& dx, int& dy) const = 0;

        /** @brief Get the mouse scroll wheel delta @return Scroll delta (positive = up) */
        virtual float GetMouseScroll() const = 0;

        /** @brief Enable or disable mouse capture (locks cursor to window) @param capture true to capture */
        virtual void SetMouseCapture(bool capture) = 0;

        /** @brief Check if the mouse is currently captured @return true if captured */
        virtual bool IsMouseCaptured() const = 0;

        /** @brief Get the number of connected gamepads @return Count of connected gamepads */
        virtual int GetGamepadCount() const = 0;

        /**
     * @brief Get the state of a specific gamepad
     * @param index Gamepad slot index
     * @return Pointer to the gamepad state, or nullptr if index is invalid
     */
        virtual const GamepadState* GetGamepadState(int index) const = 0;

        /**
     * @brief Set vibration on a gamepad
     * @param gamepadIndex Gamepad slot index
     * @param leftMotor    Left motor intensity [0.0, 1.0]
     * @param rightMotor   Right motor intensity [0.0, 1.0]
     * @param duration     Duration in seconds (0 = indefinite)
     */
        virtual void SetVibration(int gamepadIndex, float leftMotor, float rightMotor, float duration) = 0;

        /** @brief Get the human-readable backend name @return Backend name string (e.g., "Win32+XInput") */
        virtual const char* GetBackendName() const = 0;
    };

    // ============================================================================
    // Win32 Input Backend
    // ============================================================================

    /**
 * @brief Input backend using Win32 raw messages and XInput for gamepads
 *
 * Processes WM_KEYDOWN/WM_KEYUP, WM_MOUSEMOVE, WM_MOUSEWHEEL, and
 * WM_LBUTTONDOWN/UP messages from the Win32 message pump, and polls
 * XInput for gamepad state each frame.
 *
 * @note HandleWindowMessage() must be called from the application's WndProc
 *       to feed input events to this backend.
 */
    class Win32InputBackend : public IPlatformInputBackend
    {
      public:
        bool Initialize(void* windowHandle) override;
        void Shutdown() override;
        void PollEvents() override;

        bool IsKeyDown(KeyCode key) const override;
        bool WasKeyPressed(KeyCode key) const override;
        bool WasKeyReleased(KeyCode key) const override;

        void GetMousePosition(int& x, int& y) const override;
        void GetMouseDelta(int& dx, int& dy) const override;
        float GetMouseScroll() const override;

        void SetMouseCapture(bool capture) override;
        bool IsMouseCaptured() const override;

        int GetGamepadCount() const override;
        const GamepadState* GetGamepadState(int index) const override;

        void SetVibration(int gamepadIndex, float leftMotor, float rightMotor, float duration) override;

        const char* GetBackendName() const override { return "Win32+XInput"; }

        /**
     * @brief Feed a Win32 window message into the input system
     *
     * Must be called from the application's WndProc for every input-related
     * message (WM_KEYDOWN, WM_KEYUP, WM_MOUSEMOVE, WM_MOUSEWHEEL, etc.).
     *
     * @param message Win32 message ID (e.g., WM_KEYDOWN)
     * @param wParam  Message wParam
     * @param lParam  Message lParam
     */
        void HandleWindowMessage(uint32_t message, uintptr_t wParam, intptr_t lParam);

      private:
        void* m_windowHandle = nullptr; ///< HWND of the game window
        bool m_mouseCaptured = false;   ///< Whether the mouse cursor is locked to the window

        std::array<bool, static_cast<size_t>(KeyCode::MaxKeyCode)> m_keyStates{};     ///< Current key states
        std::array<bool, static_cast<size_t>(KeyCode::MaxKeyCode)> m_prevKeyStates{}; ///< Previous frame key states

        int m_mouseX = 0, m_mouseY = 0;           ///< Current absolute mouse position
        int m_mouseDeltaX = 0, m_mouseDeltaY = 0; ///< Mouse movement since last frame
        float m_mouseScroll = 0.0f;               ///< Accumulated scroll wheel delta

        static constexpr int MAX_GAMEPADS = 4;
        std::array<GamepadState, MAX_GAMEPADS> m_gamepads; ///< Per-gamepad state arrays

        /**
     * @brief Convert a Win32 virtual key code to the unified KeyCode enum
     * @param vk Win32 virtual key code (VK_*)
     * @return Corresponding KeyCode, or KeyCode::Unknown if unmapped
     */
        KeyCode VirtualKeyToKeyCode(int vk) const;
    };

    // ============================================================================
    // SDL2 Input Backend (optional, compiled when SDL2 is available)
    // ============================================================================

#ifdef SPARK_SDL2_AVAILABLE
    /**
 * @brief Input backend using the SDL2 event system for cross-platform input
 *
 * Uses SDL_PollEvent() for keyboard/mouse input and SDL_GameController API
 * for gamepad support. This backend provides identical behavior across
 * Windows, Linux, and macOS.
 *
 * @note Only compiled when SPARK_SDL2_AVAILABLE is defined and SDL2 headers
 *       are available in the include path.
 */
    class SDL2InputBackend : public IPlatformInputBackend
    {
      public:
        bool Initialize(void* windowHandle) override;
        void Shutdown() override;
        void PollEvents() override;

        bool IsKeyDown(KeyCode key) const override;
        bool WasKeyPressed(KeyCode key) const override;
        bool WasKeyReleased(KeyCode key) const override;

        void GetMousePosition(int& x, int& y) const override;
        void GetMouseDelta(int& dx, int& dy) const override;
        float GetMouseScroll() const override;

        void SetMouseCapture(bool capture) override;
        bool IsMouseCaptured() const override;

        int GetGamepadCount() const override;
        const GamepadState* GetGamepadState(int index) const override;

        void SetVibration(int gamepadIndex, float leftMotor, float rightMotor, float duration) override;

        const char* GetBackendName() const override { return "SDL2"; }

      private:
        void* m_window = nullptr;     ///< SDL_Window pointer
        bool m_mouseCaptured = false; ///< Mouse capture state

        std::array<bool, static_cast<size_t>(KeyCode::MaxKeyCode)> m_keyStates{};
        std::array<bool, static_cast<size_t>(KeyCode::MaxKeyCode)> m_prevKeyStates{};

        int m_mouseX = 0, m_mouseY = 0;
        int m_mouseDeltaX = 0, m_mouseDeltaY = 0;
        float m_mouseScroll = 0.0f;

        static constexpr int MAX_GAMEPADS = 4;
        std::array<GamepadState, MAX_GAMEPADS> m_gamepads;
        std::array<void*, MAX_GAMEPADS> m_sdlControllers{}; ///< SDL_GameController handles

        /**
     * @brief Convert an SDL scancode to the unified KeyCode enum
     * @param sdlKey SDL scancode value
     * @return Corresponding KeyCode, or KeyCode::Unknown if unmapped
     */
        KeyCode SDLKeyToKeyCode(int sdlKey) const;
    };
#endif

    // ============================================================================
    // Unified Platform Input Manager
    // ============================================================================

    /**
 * @brief Singleton input manager providing a unified API over platform backends
 *
 * PlatformInputManager is the main entry point for all input queries in game
 * code. It delegates to the active IPlatformInputBackend and adds higher-level
 * features like action mapping, axis mapping, and event callbacks.
 *
 * The manager auto-detects the best available backend at initialization time,
 * or a specific backend can be requested by name.
 *
 * @note This is a singleton — access via GetInstance().
 * @see IPlatformInputBackend, Win32InputBackend, SDL2InputBackend
 */
    class PlatformInputManager
    {
      public:
        /**
     * @brief Get the singleton instance
     * @return Reference to the global PlatformInputManager
     */
        static PlatformInputManager& GetInstance();

        /**
     * @brief Initialize the input system with auto-detected or specified backend
     * @param windowHandle      Native window handle (HWND on Windows)
     * @param preferredBackend  Backend name ("Win32", "SDL2", or "auto" for auto-detection)
     * @return true if initialization succeeded, false on failure
     */
        bool Initialize(void* windowHandle, const std::string& preferredBackend = "auto");

        /** @brief Shut down the input system and release all resources */
        void Shutdown();

        /**
     * @brief Poll for input events and update all state (call once per frame)
     *
     * Polls the active backend, updates key/mouse/gamepad state, evaluates
     * action and axis bindings, and dispatches event callbacks.
     */
        void Update();

        // ===== Key / Mouse Queries =====

        /** @brief Check if a key is currently held @param key Key to query @return true if held */
        bool IsKeyDown(KeyCode key) const;

        /** @brief Check if a key was pressed this frame @param key Key to query @return true if just pressed */
        bool WasKeyPressed(KeyCode key) const;

        /** @brief Check if a key was released this frame @param key Key to query @return true if just released */
        bool WasKeyReleased(KeyCode key) const;

        /** @brief Get current mouse position in window coordinates @param x [out] X position @param y [out] Y position */
        void GetMousePosition(int& x, int& y) const;

        /** @brief Get mouse movement since last frame @param dx [out] X delta @param dy [out] Y delta */
        void GetMouseDelta(int& dx, int& dy) const;

        /** @brief Get mouse scroll wheel delta @return Scroll delta (positive = up) */
        float GetMouseScroll() const;

        /** @brief Lock or unlock the mouse cursor to the window @param capture true to lock */
        void SetMouseCapture(bool capture);

        /** @brief Check if the mouse is locked to the window @return true if captured */
        bool IsMouseCaptured() const;

        // ===== Gamepad Queries =====

        /** @brief Get the number of connected gamepads @return Count (0 to MAX_GAMEPADS) */
        int GetGamepadCount() const;

        /** @brief Check if a gamepad is connected @param index Gamepad slot (default: 0) @return true if connected */
        bool IsGamepadConnected(int index = 0) const;

        /**
     * @brief Get a gamepad axis value
     * @param axis  Which axis to query
     * @param index Gamepad slot (default: 0)
     * @return Axis value: [-1, 1] for sticks, [0, 1] for triggers
     */
        float GetGamepadAxis(GamepadAxis axis, int index = 0) const;

        /** @brief Check if a gamepad button is held @param button Button to query @param index Gamepad slot @return true if held */
        bool IsGamepadButtonDown(GamepadBtn button, int index = 0) const;

        /** @brief Check if a gamepad button was pressed this frame @return true if just pressed */
        bool WasGamepadButtonPressed(GamepadBtn button, int index = 0) const;

        /** @brief Check if a gamepad button was released this frame @return true if just released */
        bool WasGamepadButtonReleased(GamepadBtn button, int index = 0) const;

        /**
     * @brief Set gamepad vibration
     * @param index      Gamepad slot index
     * @param leftMotor  Left motor intensity [0, 1]
     * @param rightMotor Right motor intensity [0, 1]
     * @param duration   Duration in seconds (0 = indefinite)
     */
        void SetVibration(int index, float leftMotor, float rightMotor, float duration = 0.0f);

        // ===== Action Mapping =====

        /**
     * @brief Bind a named action to a keyboard key
     * @param name    Action name (case-sensitive)
     * @param key     Key to bind
     * @param trigger When the action should fire (default: Pressed)
     */
        void BindAction(const std::string& name, KeyCode key, ActionTrigger trigger = ActionTrigger::Pressed);

        /**
     * @brief Bind a named action to a gamepad button
     * @param name    Action name
     * @param button  Gamepad button to bind
     * @param trigger When the action should fire
     */
        void BindAction(const std::string& name, GamepadBtn button, ActionTrigger trigger = ActionTrigger::Pressed);

        /**
     * @brief Bind a named axis to a pair of keyboard keys
     * @param name        Axis name
     * @param positiveKey Key that produces +1
     * @param negativeKey Key that produces -1
     * @param scale       Multiplier applied to the output
     */
        void BindAxis(const std::string& name, KeyCode positiveKey, KeyCode negativeKey, float scale = 1.0f);

        /**
     * @brief Bind a named axis to a gamepad analog axis
     * @param name      Axis name
     * @param axis      Gamepad axis to bind
     * @param scale     Multiplier applied to the output
     * @param deadZone  Minimum axis value to register (default: 0.15)
     */
        void BindAxis(const std::string& name, GamepadAxis axis, float scale = 1.0f, float deadZone = 0.15f);

        /**
     * @brief Check if a named action is currently active
     * @param name Action name
     * @return true if the action's trigger condition is met
     */
        bool IsActionActive(const std::string& name) const;

        /**
     * @brief Register a named action with a keyboard key binding.
     *
     * Convenience alias for BindAction(). Preferred in game setup code for
     * readability when declaring the initial set of bindings.
     *
     * @param name    Action name (case-sensitive)
     * @param key     Key to bind
     * @param trigger When the action should fire (default: Pressed)
     */
        void RegisterAction(const std::string& name, KeyCode key, ActionTrigger trigger = ActionTrigger::Pressed);

        /**
     * @brief Register a named action with a gamepad button binding.
     * @param name    Action name
     * @param button  Gamepad button to bind
     * @param trigger When the action should fire (default: Pressed)
     */
        void RegisterAction(const std::string& name, GamepadBtn button, ActionTrigger trigger = ActionTrigger::Pressed);

        /**
     * @brief Check if a named action was pressed this frame (rising edge).
     *
     * Unlike IsActionActive() which respects the ActionTrigger mode, this
     * method always checks for the pressed-this-frame edge, regardless of
     * the trigger setting on the binding.
     *
     * @param name Action name
     * @return true if any binding for this action was pressed this frame
     */
        bool IsActionPressed(const std::string& name) const;

        /**
     * @brief Get the analog value of a named action (0.0 or 1.0 for digital inputs).
     *
     * Returns 1.0 when the action is active, 0.0 otherwise. For gamepad
     * trigger-based actions, returns the trigger axis value [0, 1].
     *
     * @param name Action name
     * @return Action value: 0.0 (inactive) or 1.0 (active)
     */
        float GetActionValue(const std::string& name) const;

        /**
     * @brief Register a named axis with a pair of keyboard keys.
     *
     * Convenience alias for BindAxis(). Preferred in game setup code.
     *
     * @param name        Axis name
     * @param positiveKey Key that produces +1
     * @param negativeKey Key that produces -1
     * @param scale       Multiplier applied to the output
     */
        void RegisterAxis(const std::string& name, KeyCode positiveKey, KeyCode negativeKey, float scale = 1.0f);

        /**
     * @brief Register a named axis with a gamepad analog axis.
     * @param name      Axis name
     * @param axis      Gamepad axis to bind
     * @param scale     Multiplier applied to the output
     * @param deadZone  Minimum axis value to register (default: 0.15)
     */
        void RegisterAxis(const std::string& name, GamepadAxis axis, float scale = 1.0f, float deadZone = 0.15f);

        /**
     * @brief Get the current value of a named axis
     * @param name Axis name
     * @return Axis value in [-1, 1] (or [-scale, +scale])
     */
        float GetAxisValue(const std::string& name) const;

        /** @brief Remove all action and axis bindings */
        void ClearBindings();

        /** @brief Remove a specific action binding @param name Action name to remove */
        void RemoveAction(const std::string& name);

        /**
     * @brief Rebind an existing action to a new keyboard key.
     *
     * Finds the first keyboard binding for the named action and changes
     * its key. If no keyboard binding exists, a new one is created.
     *
     * @param name   Action name to rebind
     * @param newKey New key to bind the action to
     * @return true if the binding was updated or created
     */
        bool RebindAction(const std::string& name, KeyCode newKey);

        /**
     * @brief Rebind an existing action to a new gamepad button.
     *
     * Finds the first gamepad binding for the named action and changes
     * its button. If no gamepad binding exists, a new one is created.
     *
     * @param name      Action name to rebind
     * @param newButton New gamepad button to bind the action to
     * @return true if the binding was updated or created
     */
        bool RebindAction(const std::string& name, GamepadBtn newButton);

        /**
     * @brief Rebind an existing axis to new keyboard keys.
     *
     * Finds the first keyboard binding for the named axis and changes
     * its positive/negative keys. If none exists, a new one is created.
     *
     * @param name           Axis name to rebind
     * @param newPositiveKey New key that produces +1
     * @param newNegativeKey New key that produces -1
     * @return true if the binding was updated or created
     */
        bool RebindAxis(const std::string& name, KeyCode newPositiveKey, KeyCode newNegativeKey);

        // ===== Event Callbacks =====

        /**
     * @brief Register a callback for input events
     * @param callback Function called for every input event each frame
     */
        void RegisterEventCallback(InputEventCallback callback);

        // ===== Utility =====

        /** @brief Get the active backend name @return Backend name string (e.g., "Win32+XInput") */
        const char* GetBackendName() const;

        /** @brief Get the human-readable name for a key code @param key Key to query @return Key name string */
        std::string GetKeyName(KeyCode key) const;

        /** @brief Look up a KeyCode by its human-readable name @param name Key name @return KeyCode, or Unknown if not found */
        KeyCode GetKeyFromName(const std::string& name) const;

        /**
     * @brief Forward a Win32 window message to the active backend
     *
     * Must be called from WndProc when using the Win32 backend.
     *
     * @param message Win32 message ID
     * @param wParam  Message wParam
     * @param lParam  Message lParam
     */
        void HandleWindowMessage(uint32_t message, uintptr_t wParam, intptr_t lParam);

        // ===== Console Integration =====

        /** @brief Get a formatted status string for console display @return Multi-line status string */
        std::string Console_GetStatus() const;

        /** @brief Get a formatted list of all bindings for console display @return Multi-line binding list */
        std::string Console_ListBindings() const;

        /**
     * @brief Bind an action to a key by name via console command
     * @param action  Action name
     * @param keyName Human-readable key name (e.g., "Space", "W", "LeftShift")
     */
        void Console_BindAction(const std::string& action, const std::string& keyName);

        /** @brief Unbind an action via console command @param action Action name to unbind */
        void Console_UnbindAction(const std::string& action);

        /** @brief Set the global gamepad dead zone via console @param deadZone Dead zone radius [0, 1] */
        void Console_SetDeadZone(float deadZone);

        /** @brief Set the global input sensitivity via console @param sensitivity Sensitivity multiplier */
        void Console_SetSensitivity(float sensitivity);

      private:
        PlatformInputManager() = default;

        std::unique_ptr<IPlatformInputBackend> m_backend; ///< Active platform input backend
        std::vector<ActionBinding> m_actionBindings;      ///< Registered action bindings
        std::vector<AxisBinding> m_axisBindings;          ///< Registered axis bindings
        std::vector<InputEventCallback> m_eventCallbacks; ///< Registered event listeners

        float m_globalDeadZone = 0.15f;   ///< Global dead zone applied to all gamepad axes
        float m_globalSensitivity = 1.0f; ///< Global sensitivity multiplier for all input
    };

} // namespace Spark::Input
