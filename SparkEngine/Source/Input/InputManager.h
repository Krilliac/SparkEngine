/**
 * @file InputManager.h
 * @brief Comprehensive input handling system with console integration
 * @author Spark Engine Team
 * @date 2025
 * 
 * The InputManager class provides a complete input handling system that processes
 * keyboard and mouse input events, tracks button states, calculates mouse movement
 * deltas, provides convenient query methods for input state, and includes
 * comprehensive console integration for real-time input debugging and configuration.
 */

#pragma once

#include "Utils/Assert.h"
#include "../Core/framework.h"
#include "InputTypes.h"
#include <atomic>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

/**
 * @brief Input management system with comprehensive console integration
 * 
 * The InputManager class handles all user input processing for the Spark Engine.
 * It processes Windows messages for keyboard and mouse events, maintains state
 * tracking for input queries, provides mouse capture functionality for
 * first-person controls, and includes full console integration for debugging.
 * 
 * Features include:
 * - Keyboard state tracking with press/release detection
 * - Mouse button state tracking with press/release detection
 * - Mouse position and delta movement tracking
 * - Mouse capture/release for camera controls
 * - Frame-based state management for edge detection
 * - Windows message handling integration
 * - Real-time console integration for debugging
 * - Input sensitivity and dead zone configuration
 * - Key binding and remapping system
 * - Input event logging and analysis
 * 
 * @note Input states are updated once per frame via Update()
 * @warning Initialize() must be called with a valid window handle before use
 */
class InputManager
{
  private:
    std::unordered_map<int, bool> m_keyStates;     ///< Current frame keyboard states
    std::unordered_map<int, bool> m_prevKeyStates; ///< Previous frame keyboard states

    bool m_mouseButtons[3];     ///< Current frame mouse button states [Left, Right, Middle]
    bool m_prevMouseButtons[3]; ///< Previous frame mouse button states

    int m_mouseX, m_mouseY;           ///< Current mouse position
    int m_prevMouseX, m_prevMouseY;   ///< Previous frame mouse position
    int m_mouseDeltaX, m_mouseDeltaY; ///< Mouse movement delta since last frame

    HWND m_hwnd;                    ///< Window handle for mouse capture
    bool m_mouseCaptured;           ///< Whether mouse is currently captured
    bool m_captureHadFocus = false; ///< Captured-mouse focus tracking (re-seed on focus regain)

    // Console integration state
    float m_mouseSensitivity; ///< Mouse sensitivity multiplier
    float m_mouseDeadZone;    ///< Mouse dead zone for small movements
    bool m_mouseAcceleration; ///< Enable/disable mouse acceleration
    bool m_invertMouseY;      ///< Invert Y-axis for mouse movement
    bool m_rawMouseInput;     ///< Use raw mouse input instead of system cursor
    bool m_inputLogging;      ///< Enable input event logging

    // Key binding system
    std::unordered_map<std::string, int> m_keyBindings;     ///< Action name to key mappings
    std::unordered_map<int, std::string> m_reverseBindings; ///< Key to action name mappings

    // Input metrics and monitoring
    std::atomic<size_t> m_keyPressCount{0};                ///< Total key press count this session
    std::atomic<size_t> m_mousePressCount{0};              ///< Total mouse press count this session
    std::atomic<float> m_totalMouseDistance{0.0f};         ///< Total mouse movement distance
    std::vector<std::pair<int, bool>> m_recentInputEvents; ///< Recent input events for debugging

    mutable std::mutex m_inputMutex;                ///< Thread safety for input access
    std::function<void()> m_stateCallback;          ///< Callback for state changes
    std::vector<std::thread> m_pendingTimedThreads; ///< Timed key release threads to join on destruction

  public:
    /**
     * @brief Default constructor
     * 
     * Initializes input manager with default values. Call Initialize()
     * before processing any input.
     */
    InputManager();

    /**
     * @brief Destructor
     * 
     * Automatically releases mouse capture if it was active.
     */
    ~InputManager();

    /**
     * @brief Initialize the input manager with window handle
     * 
     * Sets up the input manager with the specified window handle for
     * mouse capture functionality and input processing.
     * 
     * @param hwnd Handle to the window that will receive input
     */
    void Initialize(HWND hwnd);

    /**
     * @brief Update input states for the current frame
     * 
     * Processes accumulated input changes and updates state tracking.
     * Should be called once per frame before querying input states.
     * Stores current states as previous states for next frame.
     */
    void Update();

    /**
     * @brief Process Windows input messages
     * 
     * Handles WM_KEYDOWN, WM_KEYUP, WM_LBUTTONDOWN, WM_LBUTTONUP,
     * WM_RBUTTONDOWN, WM_RBUTTONUP, WM_MBUTTONDOWN, WM_MBUTTONUP,
     * and WM_MOUSEMOVE messages to update input states.
     * 
     * @param message Windows message type
     * @param wParam Message-specific parameter (key code, etc.)
     * @param lParam Message-specific parameter (mouse coordinates, etc.)
     */
    void HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    /**
     * @brief Check if a key is currently pressed
     * 
     * Returns true if the specified key is currently being held down.
     * 
     * @param key Virtual key code (e.g., VK_SPACE, 'W', VK_ESCAPE)
     * @return true if key is currently pressed, false otherwise
     */
    bool IsKeyDown(int key) const;

    /**
     * @brief Check if a key is currently released
     * 
     * Returns true if the specified key is currently not being pressed.
     * 
     * @param key Virtual key code (e.g., VK_SPACE, 'W', VK_ESCAPE)
     * @return true if key is currently not pressed, false otherwise
     */
    bool IsKeyUp(int key) const;

    /**
     * @brief Check if a key was just pressed this frame
     * 
     * Returns true if the key was released last frame and pressed this frame.
     * Useful for detecting single key press events.
     * 
     * @param key Virtual key code (e.g., VK_SPACE, 'W', VK_ESCAPE)
     * @return true if key was just pressed, false otherwise
     */
    bool WasKeyPressed(int key) const;

    /**
     * @brief Check if a key was just released this frame
     * 
     * Returns true if the key was pressed last frame and released this frame.
     * Useful for detecting single key release events.
     * 
     * @param key Virtual key code (e.g., VK_SPACE, 'W', VK_ESCAPE)
     * @return true if key was just released, false otherwise
     */
    bool WasKeyReleased(int key) const;

    /**
     * @brief Check if a mouse button is currently pressed
     * 
     * Returns true if the specified mouse button is currently being held down.
     * 
     * @param button Mouse button index (0=Left, 1=Right, 2=Middle)
     * @return true if mouse button is currently pressed, false otherwise
     */
    bool IsMouseButtonDown(int button) const;

    /**
     * @brief Check if a mouse button was just pressed this frame
     * 
     * Returns true if the button was released last frame and pressed this frame.
     * 
     * @param button Mouse button index (0=Left, 1=Right, 2=Middle)
     * @return true if mouse button was just pressed, false otherwise
     */
    bool WasMouseButtonPressed(int button) const;

    /**
     * @brief Check if a mouse button was just released this frame
     * 
     * Returns true if the button was pressed last frame and released this frame.
     * 
     * @param button Mouse button index (0=Left, 1=Right, 2=Middle)
     * @return true if mouse button was just released, false otherwise
     */
    bool WasMouseButtonReleased(int button) const;

    /**
     * @brief Get mouse movement delta since last frame
     *
     * Returns the pixel delta movement of the mouse cursor since the last frame.
     * Applies sensitivity, dead zone, and acceleration settings.
     *
     * @return MousePoint with dx/dy values; check non-zero for valid movement
     */
    MousePoint GetMouseDelta() const;

    /**
     * @brief Get current mouse cursor position
     *
     * Returns the current pixel coordinates of the mouse cursor relative
     * to the client area of the window.
     *
     * @return MousePoint with x/y position in client coordinates
     */
    MousePoint GetMousePosition() const;

    /**
     * @brief Capture or release the mouse cursor
     * 
     * When captured, the mouse cursor is hidden and confined to the window,
     * and movement generates delta values for camera controls. When released,
     * the cursor becomes visible and can move freely.
     * 
     * @param capture true to capture mouse, false to release
     */
    void CaptureMouse(bool capture);

    /**
     * @brief Check if the mouse is currently captured
     * @return true if mouse is captured, false if free
     */
    bool IsMouseCaptured() const { return m_mouseCaptured; }

    // =========================================================================
    // CONSOLE INTEGRATION METHODS - Input System Control
    // ========================================================================

    /**
     * @brief Input metrics structure for console integration
     */
    struct InputMetrics
    {
        size_t keyPressCount;      ///< Total key presses this session
        size_t mousePressCount;    ///< Total mouse presses this session
        float totalMouseDistance;  ///< Total mouse movement distance
        size_t activeKeys;         ///< Number of currently pressed keys
        size_t activeMouseButtons; ///< Number of currently pressed mouse buttons
        bool mouseCaptured;        ///< Whether mouse is captured
        float mouseSensitivity;    ///< Current mouse sensitivity
        float mouseDeadZone;       ///< Current mouse dead zone
        bool mouseAcceleration;    ///< Mouse acceleration state
        bool invertMouseY;         ///< Y-axis inversion state
        bool rawMouseInput;        ///< Raw mouse input state
        bool inputLogging;         ///< Input logging state
        size_t totalKeyBindings;   ///< Number of active key bindings
    };

    /**
     * @brief Input settings structure for console control
     */
    struct InputSettings
    {
        float mouseSensitivity;                           ///< Mouse sensitivity multiplier (0.1-10.0)
        float mouseDeadZone;                              ///< Mouse dead zone threshold (0.0-10.0)
        bool mouseAcceleration;                           ///< Enable mouse acceleration
        bool invertMouseY;                                ///< Invert Y-axis for mouse movement
        bool rawMouseInput;                               ///< Use raw mouse input
        bool inputLogging;                                ///< Enable input event logging
        std::unordered_map<std::string, int> keyBindings; ///< Action to key mappings
    };

    /**
     * @brief Set mouse sensitivity via console
     * @param sensitivity Mouse sensitivity multiplier (0.1-10.0)
     */
    void Console_SetMouseSensitivity(float sensitivity);

    /**
     * @brief Set mouse dead zone via console
     * @param deadZone Dead zone threshold (0.0-10.0)
     */
    void Console_SetMouseDeadZone(float deadZone);

    /**
     * @brief Enable/disable mouse acceleration via console
     * @param enabled true to enable acceleration, false to disable
     */
    void Console_SetMouseAcceleration(bool enabled);

    /**
     * @brief Enable/disable Y-axis inversion via console
     * @param enabled true to invert Y-axis, false for normal
     */
    void Console_SetInvertMouseY(bool enabled);

    /**
     * @brief Enable/disable raw mouse input via console
     * @param enabled true for raw input, false for system cursor
     */
    void Console_SetRawMouseInput(bool enabled);

    /**
     * @brief Enable/disable input logging via console
     * @param enabled true to enable logging, false to disable
     */
    void Console_SetInputLogging(bool enabled);

    /**
     * @brief Bind a key to an action via console
     * @param action Action name to bind
     * @param keyName Key name or code to bind to action
     * @return true if binding was successful
     */
    bool Console_BindKey(const std::string& action, const std::string& keyName);

    /**
     * @brief Unbind a key action via console
     * @param action Action name to unbind
     */
    void Console_UnbindKey(const std::string& action);

    /**
     * @brief Get list of current key bindings via console
     * @return String containing all current key bindings
     */
    std::string Console_ListKeyBindings() const;

    /**
     * @brief Simulate a key press via console
     * @param keyName Key name or code to simulate
     * @param duration Duration to hold key in milliseconds (0 = single press)
     */
    void Console_SimulateKeyPress(const std::string& keyName, int duration = 0);

    /**
     * @brief Clear all input states via console
     */
    void Console_ClearInputStates();

    /**
     * @brief Get recent input events via console
     * @param count Number of recent events to retrieve
     * @return String containing recent input event information
     */
    std::string Console_GetRecentEvents(int count = 10) const;

    /**
     * @brief Check if a specific action is currently active via console
     * @param action Action name to check
     * @return true if action is currently active
     */
    bool Console_IsActionActive(const std::string& action) const;

    /**
     * @brief Get comprehensive input metrics (console integration)
     * @return InputMetrics structure with current input data
     */
    InputMetrics Console_GetMetrics() const;

    /**
     * @brief Get current input settings (console integration)
     * @return InputSettings structure with current settings
     */
    InputSettings Console_GetSettings() const;

    /**
     * @brief Apply input settings from console
     * @param settings InputSettings structure with new settings
     */
    void Console_ApplySettings(const InputSettings& settings);

    /**
     * @brief Reset input settings to defaults via console
     */
    void Console_ResetToDefaults();

    /**
     * @brief Register console state change callback
     * @param callback Function to call when input state changes
     */
    void Console_RegisterStateCallback(std::function<void()> callback);

    /**
     * @brief Force input system refresh via console
     */
    void Console_RefreshInput();

  private:
    /**
     * @brief Update the state of a specific key
     * 
     * Internal method for updating keyboard state from Windows messages.
     * 
     * @param key Virtual key code to update
     * @param isDown true if key is pressed, false if released
     */
    /// @brief Consolidated handler for mouse button down/up messages.
    void HandleMouseButtonMessage(int button, bool isDown);

    void UpdateKeyState(int key, bool isDown);

    /**
     * @brief Update the state of a specific mouse button
     * 
     * Internal method for updating mouse button state from Windows messages.
     * 
     * @param button Mouse button index (0=Left, 1=Right, 2=Middle)
     * @param isDown true if button is pressed, false if released
     */
    void UpdateMouseButton(int button, bool isDown);

    /**
     * @brief Update mouse position and calculate delta
     * 
     * Internal method for updating mouse position from Windows messages
     * and calculating movement delta with sensitivity and dead zone applied.
     * 
     * @param x New horizontal mouse position
     * @param y New vertical mouse position
     */
    void UpdateMousePosition(int x, int y);

    /**
     * @brief Apply mouse processing settings
     * 
     * Applies sensitivity, dead zone, acceleration, and Y-inversion to mouse delta.
     * 
     * @param deltaX Horizontal mouse delta to process
     * @param deltaY Vertical mouse delta to process
     */
    void ProcessMouseDelta(int& deltaX, int& deltaY) const;

    /**
     * @brief Convert key name to virtual key code
     * @param keyName Key name to convert
     * @return Virtual key code, or 0 if invalid
     */
    int KeyNameToVirtualKey(const std::string& keyName) const;

    /**
     * @brief Convert virtual key code to key name
     * @param virtualKey Virtual key code to convert
     * @return Key name string
     */
    std::string VirtualKeyToKeyName(int virtualKey) const;

    /**
     * @brief Add input event to recent events log
     * @param key Key code
     * @param isPressed true for press, false for release
     */
    void LogInputEvent(int key, bool isPressed);

    /**
     * @brief Notify console of state changes
     */
    void NotifyStateChange();

    /**
     * @brief Thread-safe metrics access helper
     * @return Current input metrics with thread safety
     */
    InputMetrics GetMetricsThreadSafe() const;
};
