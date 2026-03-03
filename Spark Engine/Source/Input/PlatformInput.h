/**
 * @file PlatformInput.h
 * @brief Cross-platform input abstraction layer
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a platform-agnostic input interface that abstracts over:
 * - Win32 raw input (current default)
 * - SDL2 (when available)
 * - Linux evdev (future)
 *
 * This layer sits between the game and the platform-specific InputManager,
 * providing unified key codes, gamepad support, and action mapping that
 * works across platforms.
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <array>

namespace Spark::Input {

// ============================================================================
// Platform-Agnostic Key Codes
// ============================================================================

enum class KeyCode : uint16_t {
    Unknown = 0,

    // Letters
    A = 'A', B = 'B', C = 'C', D = 'D', E = 'E', F = 'F',
    G = 'G', H = 'H', I = 'I', J = 'J', K = 'K', L = 'L',
    M = 'M', N = 'N', O = 'O', P = 'P', Q = 'Q', R = 'R',
    S = 'S', T = 'T', U = 'U', V = 'V', W = 'W', X = 'X',
    Y = 'Y', Z = 'Z',

    // Numbers
    Num0 = '0', Num1 = '1', Num2 = '2', Num3 = '3', Num4 = '4',
    Num5 = '5', Num6 = '6', Num7 = '7', Num8 = '8', Num9 = '9',

    // Function keys
    F1 = 256, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,

    // Navigation
    Escape = 280, Enter, Tab, Backspace, Insert, Delete,
    Right, Left, Down, Up, PageUp, PageDown, Home, End,

    // Modifiers
    LeftShift = 300, RightShift, LeftCtrl, RightCtrl,
    LeftAlt, RightAlt, LeftSuper, RightSuper,

    // Misc
    Space = 320, Comma, Period, Slash, Semicolon,
    Apostrophe, LeftBracket, RightBracket, Backslash,
    GraveAccent, Minus, Equal,

    // Numpad
    Numpad0 = 350, Numpad1, Numpad2, Numpad3, Numpad4,
    Numpad5, Numpad6, Numpad7, Numpad8, Numpad9,
    NumpadDecimal, NumpadDivide, NumpadMultiply,
    NumpadSubtract, NumpadAdd, NumpadEnter,

    // Mouse buttons (unified with keyboard)
    MouseLeft = 400, MouseRight, MouseMiddle,
    MouseButton4, MouseButton5,

    MaxKeyCode = 512
};

// ============================================================================
// Gamepad Abstraction
// ============================================================================

enum class GamepadAxis : uint8_t {
    LeftStickX,
    LeftStickY,
    RightStickX,
    RightStickY,
    LeftTrigger,
    RightTrigger,
    Count
};

enum class GamepadBtn : uint8_t {
    A, B, X, Y,
    LeftBumper, RightBumper,
    Back, Start, Guide,
    LeftStick, RightStick,
    DPadUp, DPadDown, DPadLeft, DPadRight,
    Count
};

struct GamepadState {
    bool connected = false;
    std::string name;
    std::array<float, static_cast<size_t>(GamepadAxis::Count)> axes{};
    std::array<bool, static_cast<size_t>(GamepadBtn::Count)> buttons{};
    std::array<bool, static_cast<size_t>(GamepadBtn::Count)> prevButtons{};
};

// ============================================================================
// Input Action Mapping
// ============================================================================

enum class ActionTrigger {
    Pressed,   ///< Just pressed this frame
    Released,  ///< Just released this frame
    Held       ///< Currently held down
};

struct ActionBinding {
    std::string actionName;
    KeyCode key = KeyCode::Unknown;
    GamepadBtn gamepadButton = GamepadBtn::A;
    bool useGamepad = false;
    ActionTrigger trigger = ActionTrigger::Pressed;
};

struct AxisBinding {
    std::string axisName;
    KeyCode positiveKey = KeyCode::Unknown;
    KeyCode negativeKey = KeyCode::Unknown;
    GamepadAxis gamepadAxis = GamepadAxis::LeftStickX;
    bool useGamepad = false;
    float scale = 1.0f;
    float deadZone = 0.15f;
};

// ============================================================================
// Input Events
// ============================================================================

struct InputEvent {
    enum class Type {
        KeyDown, KeyUp,
        MouseMove, MouseScroll,
        GamepadButton, GamepadAxis,
        TextInput
    };

    Type type;
    KeyCode key = KeyCode::Unknown;
    int mouseX = 0, mouseY = 0;
    int mouseDeltaX = 0, mouseDeltaY = 0;
    float scrollDelta = 0.0f;
    int gamepadIndex = 0;
    GamepadBtn gamepadButton = GamepadBtn::A;
    GamepadAxis gamepadAxis = GamepadAxis::LeftStickX;
    float axisValue = 0.0f;
    char32_t textChar = 0;
};

using InputEventCallback = std::function<void(const InputEvent&)>;

// ============================================================================
// Platform Input Backend Interface
// ============================================================================

class IPlatformInputBackend {
public:
    virtual ~IPlatformInputBackend() = default;

    virtual bool Initialize(void* windowHandle) = 0;
    virtual void Shutdown() = 0;
    virtual void PollEvents() = 0;

    virtual bool IsKeyDown(KeyCode key) const = 0;
    virtual bool WasKeyPressed(KeyCode key) const = 0;
    virtual bool WasKeyReleased(KeyCode key) const = 0;

    virtual void GetMousePosition(int& x, int& y) const = 0;
    virtual void GetMouseDelta(int& dx, int& dy) const = 0;
    virtual float GetMouseScroll() const = 0;

    virtual void SetMouseCapture(bool capture) = 0;
    virtual bool IsMouseCaptured() const = 0;

    virtual int GetGamepadCount() const = 0;
    virtual const GamepadState* GetGamepadState(int index) const = 0;

    virtual void SetVibration(int gamepadIndex, float leftMotor, float rightMotor, float duration) = 0;

    virtual const char* GetBackendName() const = 0;
};

// ============================================================================
// Win32 Input Backend
// ============================================================================

class Win32InputBackend : public IPlatformInputBackend {
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

    /// Call from WndProc to feed input events
    void HandleWindowMessage(uint32_t message, uintptr_t wParam, intptr_t lParam);

private:
    void* m_windowHandle = nullptr;
    bool m_mouseCaptured = false;

    std::array<bool, static_cast<size_t>(KeyCode::MaxKeyCode)> m_keyStates{};
    std::array<bool, static_cast<size_t>(KeyCode::MaxKeyCode)> m_prevKeyStates{};

    int m_mouseX = 0, m_mouseY = 0;
    int m_mouseDeltaX = 0, m_mouseDeltaY = 0;
    float m_mouseScroll = 0.0f;

    static constexpr int MAX_GAMEPADS = 4;
    std::array<GamepadState, MAX_GAMEPADS> m_gamepads;

    KeyCode VirtualKeyToKeyCode(int vk) const;
};

// ============================================================================
// SDL2 Input Backend (optional, compiled when SDL2 is available)
// ============================================================================

#ifdef SPARK_SDL2_AVAILABLE
class SDL2InputBackend : public IPlatformInputBackend {
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
    void* m_window = nullptr;
    bool m_mouseCaptured = false;

    std::array<bool, static_cast<size_t>(KeyCode::MaxKeyCode)> m_keyStates{};
    std::array<bool, static_cast<size_t>(KeyCode::MaxKeyCode)> m_prevKeyStates{};

    int m_mouseX = 0, m_mouseY = 0;
    int m_mouseDeltaX = 0, m_mouseDeltaY = 0;
    float m_mouseScroll = 0.0f;

    static constexpr int MAX_GAMEPADS = 4;
    std::array<GamepadState, MAX_GAMEPADS> m_gamepads;
    std::array<void*, MAX_GAMEPADS> m_sdlControllers{};

    KeyCode SDLKeyToKeyCode(int sdlKey) const;
};
#endif

// ============================================================================
// Unified Platform Input Manager
// ============================================================================

class PlatformInputManager {
public:
    static PlatformInputManager& GetInstance();

    /// Initialize with auto-detected backend (or specify)
    bool Initialize(void* windowHandle, const std::string& preferredBackend = "auto");
    void Shutdown();

    /// Call each frame to update input state
    void Update();

    // ===== Key / Mouse Queries =====

    bool IsKeyDown(KeyCode key) const;
    bool WasKeyPressed(KeyCode key) const;
    bool WasKeyReleased(KeyCode key) const;

    void GetMousePosition(int& x, int& y) const;
    void GetMouseDelta(int& dx, int& dy) const;
    float GetMouseScroll() const;

    void SetMouseCapture(bool capture);
    bool IsMouseCaptured() const;

    // ===== Gamepad Queries =====

    int GetGamepadCount() const;
    bool IsGamepadConnected(int index = 0) const;
    float GetGamepadAxis(GamepadAxis axis, int index = 0) const;
    bool IsGamepadButtonDown(GamepadBtn button, int index = 0) const;
    bool WasGamepadButtonPressed(GamepadBtn button, int index = 0) const;
    bool WasGamepadButtonReleased(GamepadBtn button, int index = 0) const;
    void SetVibration(int index, float leftMotor, float rightMotor, float duration = 0.0f);

    // ===== Action Mapping =====

    void BindAction(const std::string& name, KeyCode key, ActionTrigger trigger = ActionTrigger::Pressed);
    void BindAction(const std::string& name, GamepadBtn button, ActionTrigger trigger = ActionTrigger::Pressed);
    void BindAxis(const std::string& name, KeyCode positiveKey, KeyCode negativeKey, float scale = 1.0f);
    void BindAxis(const std::string& name, GamepadAxis axis, float scale = 1.0f, float deadZone = 0.15f);

    bool IsActionActive(const std::string& name) const;
    float GetAxisValue(const std::string& name) const;

    void ClearBindings();
    void RemoveAction(const std::string& name);

    // ===== Event Callbacks =====

    void RegisterEventCallback(InputEventCallback callback);

    // ===== Utility =====

    const char* GetBackendName() const;
    std::string GetKeyName(KeyCode key) const;
    KeyCode GetKeyFromName(const std::string& name) const;

    /// Forward Win32 messages (when using Win32 backend)
    void HandleWindowMessage(uint32_t message, uintptr_t wParam, intptr_t lParam);

    // ===== Console Integration =====

    std::string Console_GetStatus() const;
    std::string Console_ListBindings() const;
    void Console_BindAction(const std::string& action, const std::string& keyName);
    void Console_UnbindAction(const std::string& action);
    void Console_SetDeadZone(float deadZone);
    void Console_SetSensitivity(float sensitivity);

private:
    PlatformInputManager() = default;

    std::unique_ptr<IPlatformInputBackend> m_backend;
    std::vector<ActionBinding> m_actionBindings;
    std::vector<AxisBinding> m_axisBindings;
    std::vector<InputEventCallback> m_eventCallbacks;

    float m_globalDeadZone = 0.15f;
    float m_globalSensitivity = 1.0f;
};

} // namespace Spark::Input
