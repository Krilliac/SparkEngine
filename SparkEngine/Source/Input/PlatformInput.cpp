#include "../Core/Platform.h"
/**
 * @file PlatformInput.cpp
 * @brief Cross-platform shared input logic (PlatformInputManager + key name mapping)
 *
 * Platform-specific backend implementations live in separate files:
 *   - PlatformInputWin32.cpp  (Win32 + XInput)
 *   - PlatformInputSDL2.cpp   (SDL2, used on Linux and optionally elsewhere)
 */

#include "PlatformInput.h"
#include "../Utils/Validate.h"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Spark::Input
{

    // ============================================================================
    // Key Name Mapping
    // ============================================================================

    static const std::unordered_map<KeyCode, std::string>& GetKeyNames()
    {
        static std::unordered_map<KeyCode, std::string> names = {
            {KeyCode::A, "A"},
            {KeyCode::B, "B"},
            {KeyCode::C, "C"},
            {KeyCode::D, "D"},
            {KeyCode::E, "E"},
            {KeyCode::F, "F"},
            {KeyCode::G, "G"},
            {KeyCode::H, "H"},
            {KeyCode::I, "I"},
            {KeyCode::J, "J"},
            {KeyCode::K, "K"},
            {KeyCode::L, "L"},
            {KeyCode::M, "M"},
            {KeyCode::N, "N"},
            {KeyCode::O, "O"},
            {KeyCode::P, "P"},
            {KeyCode::Q, "Q"},
            {KeyCode::R, "R"},
            {KeyCode::S, "S"},
            {KeyCode::T, "T"},
            {KeyCode::U, "U"},
            {KeyCode::V, "V"},
            {KeyCode::W, "W"},
            {KeyCode::X, "X"},
            {KeyCode::Y, "Y"},
            {KeyCode::Z, "Z"},
            {KeyCode::Num0, "0"},
            {KeyCode::Num1, "1"},
            {KeyCode::Num2, "2"},
            {KeyCode::Num3, "3"},
            {KeyCode::Num4, "4"},
            {KeyCode::Num5, "5"},
            {KeyCode::Num6, "6"},
            {KeyCode::Num7, "7"},
            {KeyCode::Num8, "8"},
            {KeyCode::Num9, "9"},
            {KeyCode::F1, "F1"},
            {KeyCode::F2, "F2"},
            {KeyCode::F3, "F3"},
            {KeyCode::F4, "F4"},
            {KeyCode::F5, "F5"},
            {KeyCode::F6, "F6"},
            {KeyCode::F7, "F7"},
            {KeyCode::F8, "F8"},
            {KeyCode::F9, "F9"},
            {KeyCode::F10, "F10"},
            {KeyCode::F11, "F11"},
            {KeyCode::F12, "F12"},
            {KeyCode::Escape, "Escape"},
            {KeyCode::Enter, "Enter"},
            {KeyCode::Tab, "Tab"},
            {KeyCode::Backspace, "Backspace"},
            {KeyCode::Space, "Space"},
            {KeyCode::LeftShift, "LeftShift"},
            {KeyCode::RightShift, "RightShift"},
            {KeyCode::LeftCtrl, "LeftCtrl"},
            {KeyCode::RightCtrl, "RightCtrl"},
            {KeyCode::LeftAlt, "LeftAlt"},
            {KeyCode::RightAlt, "RightAlt"},
            {KeyCode::Up, "Up"},
            {KeyCode::Down, "Down"},
            {KeyCode::Left, "Left"},
            {KeyCode::Right, "Right"},
            {KeyCode::Insert, "Insert"},
            {KeyCode::Delete, "Delete"},
            {KeyCode::Home, "Home"},
            {KeyCode::End, "End"},
            {KeyCode::PageUp, "PageUp"},
            {KeyCode::PageDown, "PageDown"},
            {KeyCode::MouseLeft, "MouseLeft"},
            {KeyCode::MouseRight, "MouseRight"},
            {KeyCode::MouseMiddle, "MouseMiddle"},
        };
        return names;
    }

    // ============================================================================
    // PlatformInputManager
    // ============================================================================

    PlatformInputManager& PlatformInputManager::GetInstance()
    {
        static PlatformInputManager instance;
        return instance;
    }

    bool PlatformInputManager::Initialize(void* windowHandle, const std::string& preferredBackend)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Input);
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Input, windowHandle, false);
#ifdef SPARK_SDL2_AVAILABLE
        if (preferredBackend == "sdl2" || preferredBackend == "SDL2")
        {
            m_backend = std::make_unique<SDL2InputBackend>();
            return m_backend->Initialize(windowHandle);
        }
#endif

#ifdef _WIN32
        if (preferredBackend == "auto" || preferredBackend == "win32")
        {
            m_backend = std::make_unique<Win32InputBackend>();
            return m_backend->Initialize(windowHandle);
        }
#endif

        // Fallback: try platform default
#ifdef _WIN32
        m_backend = std::make_unique<Win32InputBackend>();
#elif defined(SPARK_SDL2_AVAILABLE)
        m_backend = std::make_unique<SDL2InputBackend>();
#endif

        return m_backend ? m_backend->Initialize(windowHandle) : false;
    }

    void PlatformInputManager::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Input, "PlatformInputManager shutting down");
        if (m_backend)
            m_backend->Shutdown();
        m_backend.reset();
    }

    void PlatformInputManager::Update()
    {
        SPARK_WARN_IF(Spark::LogCategory::Input, !m_backend, "PlatformInputManager::Update called with no backend");
        if (!m_backend)
            return;
        m_backend->PollEvents();

        // Dispatch input events to registered callbacks
        if (!m_eventCallbacks.empty())
        {
            // Key events: scan all key codes for state changes
            for (uint16_t k = 0; k < static_cast<uint16_t>(KeyCode::MaxKeyCode); ++k)
            {
                auto kc = static_cast<KeyCode>(k);
                if (m_backend->WasKeyPressed(kc))
                {
                    InputEvent evt{};
                    evt.type = InputEvent::Type::KeyDown;
                    evt.key = kc;
                    for (const auto& cb : m_eventCallbacks)
                    {
                        cb(evt);
                    }
                }
                else if (m_backend->WasKeyReleased(kc))
                {
                    InputEvent evt{};
                    evt.type = InputEvent::Type::KeyUp;
                    evt.key = kc;
                    for (const auto& cb : m_eventCallbacks)
                    {
                        cb(evt);
                    }
                }
            }

            // Mouse movement events
            auto mouseDelta = m_backend->GetMouseDelta();
            if (mouseDelta.x != 0 || mouseDelta.y != 0)
            {
                InputEvent evt{};
                evt.type = InputEvent::Type::MouseMove;
                auto mousePos = m_backend->GetMousePosition();
                evt.mouseX = mousePos.x;
                evt.mouseY = mousePos.y;
                evt.mouseDeltaX = mouseDelta.x;
                evt.mouseDeltaY = mouseDelta.y;
                for (const auto& cb : m_eventCallbacks)
                {
                    cb(evt);
                }
            }

            // Mouse scroll events
            float scroll = m_backend->GetMouseScroll();
            if (scroll != 0.0f)
            {
                InputEvent evt{};
                evt.type = InputEvent::Type::MouseScroll;
                evt.scrollDelta = scroll;
                for (const auto& cb : m_eventCallbacks)
                {
                    cb(evt);
                }
            }

            // Gamepad button events
            for (int gi = 0; gi < m_backend->GetGamepadCount(); ++gi)
            {
                const GamepadState* gp = m_backend->GetGamepadState(gi);
                if (!gp || !gp->connected)
                    continue;

                for (uint8_t bi = 0; bi < static_cast<uint8_t>(GamepadBtn::Count); ++bi)
                {
                    bool curr = gp->buttons[bi];
                    bool prev = gp->prevButtons[bi];
                    if (curr != prev)
                    {
                        InputEvent evt{};
                        evt.type = InputEvent::Type::GamepadButton;
                        evt.gamepadIndex = gi;
                        evt.gamepadButton = static_cast<GamepadBtn>(bi);
                        // Encode press/release: for GamepadButton events, axisValue
                        // carries 1.0 for press, 0.0 for release
                        evt.axisValue = curr ? 1.0f : 0.0f;
                        for (const auto& cb : m_eventCallbacks)
                        {
                            cb(evt);
                        }
                    }
                }

                // Gamepad axis change events (only fire if axis moved past dead zone)
                for (uint8_t ai = 0; ai < static_cast<uint8_t>(GamepadAxis::Count); ++ai)
                {
                    float val = gp->axes[ai];
                    if (std::abs(val) > m_globalDeadZone)
                    {
                        InputEvent evt{};
                        evt.type = InputEvent::Type::GamepadAxis;
                        evt.gamepadIndex = gi;
                        evt.gamepadAxis = static_cast<GamepadAxis>(ai);
                        evt.axisValue = val;
                        for (const auto& cb : m_eventCallbacks)
                        {
                            cb(evt);
                        }
                    }
                }
            }
        }
    }

    bool PlatformInputManager::IsKeyDown(KeyCode key) const
    {
        return m_backend ? m_backend->IsKeyDown(key) : false;
    }

    bool PlatformInputManager::WasKeyPressed(KeyCode key) const
    {
        return m_backend ? m_backend->WasKeyPressed(key) : false;
    }

    bool PlatformInputManager::WasKeyReleased(KeyCode key) const
    {
        return m_backend ? m_backend->WasKeyReleased(key) : false;
    }

    MousePoint PlatformInputManager::GetMousePosition() const
    {
        if (m_backend)
            return m_backend->GetMousePosition();
        return {0, 0};
    }

    MousePoint PlatformInputManager::GetMouseDelta() const
    {
        if (m_backend)
        {
            auto delta = m_backend->GetMouseDelta();
            delta.x = static_cast<int>(delta.x * m_globalSensitivity);
            delta.y = static_cast<int>(delta.y * m_globalSensitivity);
            return delta;
        }
        return {0, 0};
    }

    float PlatformInputManager::GetMouseScroll() const
    {
        return m_backend ? m_backend->GetMouseScroll() : 0.0f;
    }

    void PlatformInputManager::SetMouseCapture(bool capture)
    {
        if (m_backend)
            m_backend->SetMouseCapture(capture);
    }

    bool PlatformInputManager::IsMouseCaptured() const
    {
        return m_backend ? m_backend->IsMouseCaptured() : false;
    }

    int PlatformInputManager::GetGamepadCount() const
    {
        return m_backend ? m_backend->GetGamepadCount() : 0;
    }

    bool PlatformInputManager::IsGamepadConnected(int index) const
    {
        if (!m_backend)
            return false;
        auto* state = m_backend->GetGamepadState(index);
        return state && state->connected;
    }

    float PlatformInputManager::GetGamepadAxis(GamepadAxis axis, int index) const
    {
        if (!m_backend)
            return 0.0f;
        auto* state = m_backend->GetGamepadState(index);
        if (!state || !state->connected)
            return 0.0f;
        float val = state->axes[static_cast<size_t>(axis)];
        // Apply dead zone
        if (std::abs(val) < m_globalDeadZone)
            return 0.0f;
        return val;
    }

    bool PlatformInputManager::IsGamepadButtonDown(GamepadBtn button, int index) const
    {
        if (!m_backend)
            return false;
        auto* state = m_backend->GetGamepadState(index);
        if (!state || !state->connected)
            return false;
        return state->buttons[static_cast<size_t>(button)];
    }

    bool PlatformInputManager::WasGamepadButtonPressed(GamepadBtn button, int index) const
    {
        if (!m_backend)
            return false;
        auto* state = m_backend->GetGamepadState(index);
        if (!state || !state->connected)
            return false;
        return state->buttons[static_cast<size_t>(button)] && !state->prevButtons[static_cast<size_t>(button)];
    }

    bool PlatformInputManager::WasGamepadButtonReleased(GamepadBtn button, int index) const
    {
        if (!m_backend)
            return false;
        auto* state = m_backend->GetGamepadState(index);
        if (!state || !state->connected)
            return false;
        return !state->buttons[static_cast<size_t>(button)] && state->prevButtons[static_cast<size_t>(button)];
    }

    void PlatformInputManager::SetVibration(int index, float leftMotor, float rightMotor, float duration)
    {
        if (m_backend)
            m_backend->SetVibration(index, leftMotor, rightMotor, duration);
    }

    // ===== Action Mapping =====

    void PlatformInputManager::BindAction(const std::string& name, KeyCode key, ActionTrigger trigger)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Input);
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Input, name);
        ActionBinding binding;
        binding.actionName = name;
        binding.key = key;
        binding.useGamepad = false;
        binding.trigger = trigger;
        m_actionBindings.push_back(binding);
    }

    void PlatformInputManager::BindAction(const std::string& name, GamepadBtn button, ActionTrigger trigger)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Input);
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Input, name);
        ActionBinding binding;
        binding.actionName = name;
        binding.gamepadButton = button;
        binding.useGamepad = true;
        binding.trigger = trigger;
        m_actionBindings.push_back(binding);
    }

    void PlatformInputManager::BindAxis(const std::string& name, KeyCode positiveKey, KeyCode negativeKey, float scale)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Input);
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Input, name);
        AxisBinding binding;
        binding.axisName = name;
        binding.positiveKey = positiveKey;
        binding.negativeKey = negativeKey;
        binding.useGamepad = false;
        binding.scale = scale;
        m_axisBindings.push_back(binding);
    }

    void PlatformInputManager::BindAxis(const std::string& name, GamepadAxis axis, float scale, float deadZone)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Input);
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Input, name);
        AxisBinding binding;
        binding.axisName = name;
        binding.gamepadAxis = axis;
        binding.useGamepad = true;
        binding.scale = scale;
        binding.deadZone = deadZone;
        m_axisBindings.push_back(binding);
    }

    bool PlatformInputManager::IsActionActive(const std::string& name) const
    {
        for (const auto& binding : m_actionBindings)
        {
            if (binding.actionName != name)
                continue;

            if (!binding.useGamepad)
            {
                switch (binding.trigger)
                {
                case ActionTrigger::Pressed:
                    if (WasKeyPressed(binding.key))
                        return true;
                    break;
                case ActionTrigger::Released:
                    if (WasKeyReleased(binding.key))
                        return true;
                    break;
                case ActionTrigger::Held:
                    if (IsKeyDown(binding.key))
                        return true;
                    break;
                }
            }
            else
            {
                switch (binding.trigger)
                {
                case ActionTrigger::Pressed:
                    if (WasGamepadButtonPressed(binding.gamepadButton))
                        return true;
                    break;
                case ActionTrigger::Released:
                    if (WasGamepadButtonReleased(binding.gamepadButton))
                        return true;
                    break;
                case ActionTrigger::Held:
                    if (IsGamepadButtonDown(binding.gamepadButton))
                        return true;
                    break;
                }
            }
        }
        return false;
    }

    float PlatformInputManager::GetAxisValue(const std::string& name) const
    {
        float result = 0.0f;

        for (const auto& binding : m_axisBindings)
        {
            if (binding.axisName != name)
                continue;

            if (!binding.useGamepad)
            {
                float val = 0.0f;
                if (IsKeyDown(binding.positiveKey))
                    val += 1.0f;
                if (IsKeyDown(binding.negativeKey))
                    val -= 1.0f;
                result += val * binding.scale;
            }
            else
            {
                float val = GetGamepadAxis(binding.gamepadAxis);
                if (std::abs(val) < binding.deadZone)
                    val = 0.0f;
                result += val * binding.scale;
            }
        }

        return result;
    }

    void PlatformInputManager::ClearBindings()
    {
        m_actionBindings.clear();
        m_axisBindings.clear();
    }

    void PlatformInputManager::RemoveAction(const std::string& name)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Input);
        SPARK_WARN_IF(Spark::LogCategory::Input, name.empty(), "RemoveAction called with empty name");
        m_actionBindings.erase(std::remove_if(m_actionBindings.begin(), m_actionBindings.end(),
                                              [&](const ActionBinding& b) { return b.actionName == name; }),
                               m_actionBindings.end());
        m_axisBindings.erase(std::remove_if(m_axisBindings.begin(), m_axisBindings.end(),
                                            [&](const AxisBinding& b) { return b.axisName == name; }),
                             m_axisBindings.end());
    }

    // ===== Registration Aliases =====

    void PlatformInputManager::RegisterAction(const std::string& name, KeyCode key, ActionTrigger trigger)
    {
        BindAction(name, key, trigger);
    }

    void PlatformInputManager::RegisterAction(const std::string& name, GamepadBtn button, ActionTrigger trigger)
    {
        BindAction(name, button, trigger);
    }

    bool PlatformInputManager::IsActionPressed(const std::string& name) const
    {
        for (const auto& binding : m_actionBindings)
        {
            if (binding.actionName != name)
                continue;

            if (!binding.useGamepad)
            {
                if (WasKeyPressed(binding.key))
                    return true;
            }
            else
            {
                if (WasGamepadButtonPressed(binding.gamepadButton))
                    return true;
            }
        }
        return false;
    }

    float PlatformInputManager::GetActionValue(const std::string& name) const
    {
        for (const auto& binding : m_actionBindings)
        {
            if (binding.actionName != name)
                continue;

            if (!binding.useGamepad)
            {
                if (IsKeyDown(binding.key))
                    return 1.0f;
            }
            else
            {
                if (IsGamepadButtonDown(binding.gamepadButton))
                    return 1.0f;
            }
        }
        return 0.0f;
    }

    void PlatformInputManager::RegisterAxis(const std::string& name, KeyCode positiveKey, KeyCode negativeKey,
                                            float scale)
    {
        BindAxis(name, positiveKey, negativeKey, scale);
    }

    void PlatformInputManager::RegisterAxis(const std::string& name, GamepadAxis axis, float scale, float deadZone)
    {
        BindAxis(name, axis, scale, deadZone);
    }

    // ===== Input Rebinding =====

    bool PlatformInputManager::RebindAction(const std::string& name, KeyCode newKey)
    {
        for (auto& binding : m_actionBindings)
        {
            if (binding.actionName == name && !binding.useGamepad)
            {
                binding.key = newKey;
                return true;
            }
        }

        // No existing keyboard binding found; create a new one
        BindAction(name, newKey);
        return true;
    }

    bool PlatformInputManager::RebindAction(const std::string& name, GamepadBtn newButton)
    {
        for (auto& binding : m_actionBindings)
        {
            if (binding.actionName == name && binding.useGamepad)
            {
                binding.gamepadButton = newButton;
                return true;
            }
        }

        // No existing gamepad binding found; create a new one
        BindAction(name, newButton);
        return true;
    }

    bool PlatformInputManager::RebindAxis(const std::string& name, KeyCode newPositiveKey, KeyCode newNegativeKey)
    {
        for (auto& binding : m_axisBindings)
        {
            if (binding.axisName == name && !binding.useGamepad)
            {
                binding.positiveKey = newPositiveKey;
                binding.negativeKey = newNegativeKey;
                return true;
            }
        }

        // No existing keyboard axis binding found; create a new one
        BindAxis(name, newPositiveKey, newNegativeKey);
        return true;
    }

    // ===== Event Callbacks =====

    void PlatformInputManager::RegisterEventCallback(InputEventCallback callback)
    {
        SPARK_WARN_IF(Spark::LogCategory::Input, !callback, "RegisterEventCallback called with null callback");
        m_eventCallbacks.push_back(std::move(callback));
    }

    const char* PlatformInputManager::GetBackendName() const
    {
        return m_backend ? m_backend->GetBackendName() : "None";
    }

    std::string PlatformInputManager::GetKeyName(KeyCode key) const
    {
        const auto& names = GetKeyNames();
        auto it = names.find(key);
        return (it != names.end()) ? it->second : "Unknown";
    }

    KeyCode PlatformInputManager::GetKeyFromName(const std::string& name) const
    {
        const auto& names = GetKeyNames();
        for (const auto& [code, keyName] : names)
        {
            if (keyName == name)
                return code;
        }
        return KeyCode::Unknown;
    }

    void PlatformInputManager::HandleWindowMessage(uint32_t message, uintptr_t wParam, intptr_t lParam)
    {
#ifdef _WIN32
        auto* win32Backend = dynamic_cast<Win32InputBackend*>(m_backend.get());
        if (win32Backend)
        {
            win32Backend->HandleWindowMessage(message, wParam, lParam);
        }
#endif
    }

    // ===== Console Integration =====

    std::string PlatformInputManager::Console_GetStatus() const
    {
        std::ostringstream ss;
        ss << "=== Platform Input ===\n";
        ss << "Backend: " << GetBackendName() << "\n";
        ss << "Mouse Captured: " << (IsMouseCaptured() ? "Yes" : "No") << "\n";
        ss << "Gamepads Connected: " << GetGamepadCount() << "\n";
        ss << "Action Bindings: " << m_actionBindings.size() << "\n";
        ss << "Axis Bindings: " << m_axisBindings.size() << "\n";
        ss << "Sensitivity: " << m_globalSensitivity << "\n";
        ss << "Dead Zone: " << m_globalDeadZone << "\n";
        return ss.str();
    }

    std::string PlatformInputManager::Console_ListBindings() const
    {
        std::ostringstream ss;
        ss << "=== Action Bindings ===\n";
        for (const auto& b : m_actionBindings)
        {
            ss << "  " << b.actionName << " -> ";
            if (!b.useGamepad)
            {
                ss << GetKeyName(b.key);
            }
            else
            {
                ss << "Gamepad:" << static_cast<int>(b.gamepadButton);
            }
            ss << "\n";
        }
        ss << "\n=== Axis Bindings ===\n";
        for (const auto& b : m_axisBindings)
        {
            ss << "  " << b.axisName << " -> ";
            if (!b.useGamepad)
            {
                ss << GetKeyName(b.positiveKey) << "/" << GetKeyName(b.negativeKey);
            }
            else
            {
                ss << "Gamepad Axis:" << static_cast<int>(b.gamepadAxis);
            }
            ss << " (scale:" << b.scale << ")\n";
        }
        return ss.str();
    }

    void PlatformInputManager::Console_BindAction(const std::string& action, const std::string& keyName)
    {
        KeyCode key = GetKeyFromName(keyName);
        if (key != KeyCode::Unknown)
        {
            BindAction(action, key);
        }
    }

    void PlatformInputManager::Console_UnbindAction(const std::string& action)
    {
        RemoveAction(action);
    }

    void PlatformInputManager::Console_SetDeadZone(float deadZone)
    {
        m_globalDeadZone = (std::max)(0.0f, (std::min)(deadZone, 1.0f));
    }

    void PlatformInputManager::Console_SetSensitivity(float sensitivity)
    {
        m_globalSensitivity = (std::max)(0.1f, (std::min)(sensitivity, 10.0f));
    }

} // namespace Spark::Input
