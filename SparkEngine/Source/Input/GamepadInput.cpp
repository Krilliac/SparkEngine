#include "Core/Platform.h"
// GamepadInput.cpp
#include "GamepadInput.h"
#include "../Utils/Validate.h"
#include <sstream>
#include <cmath>
#include <algorithm>

using namespace DirectX;
GamepadInput::GamepadInput() = default;
GamepadInput::~GamepadInput()
{
    StopAllVibrations();
}

void GamepadInput::Update(float deltaTime)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Input);
    m_connectionPollTimer += deltaTime;

    for (int i = 0; i < MAX_CONTROLLERS; ++i)
    {
        auto& state = m_states[i];

        // Save previous state
        state.prevXinputState = state.xinputState;

        // Only poll disconnected controllers periodically to save CPU
        if (!state.connected && m_connectionPollTimer < CONNECTION_POLL_INTERVAL)
            continue;

        DWORD result = XInputGetState(i, &state.xinputState);
        bool wasConnected = state.connected;
        state.connected = (result == ERROR_SUCCESS);

        // Log connection state changes (not per-frame)
        if (state.connected && !wasConnected)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Input, "Gamepad %d connected", i);
        }
        else if (!state.connected && wasConnected)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Input, "Gamepad %d disconnected", i);
        }

        if (!state.connected)
            continue;

        // Process thumbsticks with dead zone
        state.leftStick =
            ApplyDeadZone(state.xinputState.Gamepad.sThumbLX, state.xinputState.Gamepad.sThumbLY, m_leftStickDeadZone);

        state.rightStick =
            ApplyDeadZone(state.xinputState.Gamepad.sThumbRX, state.xinputState.Gamepad.sThumbRY, m_rightStickDeadZone);

        // Apply sensitivity
        state.leftStick.x *= m_stickSensitivity;
        state.leftStick.y *= m_stickSensitivity;
        state.rightStick.x *= m_stickSensitivity;
        state.rightStick.y *= m_stickSensitivity;

        // Process triggers
        state.leftTrigger = NormalizeTrigger(state.xinputState.Gamepad.bLeftTrigger);
        state.rightTrigger = NormalizeTrigger(state.xinputState.Gamepad.bRightTrigger);

        // Update vibration timer
        if (state.vibrationTimer > 0.0f)
        {
            state.vibrationTimer -= deltaTime;
            if (state.vibrationTimer <= 0.0f)
            {
                StopVibration(i);
            }
        }
    }

    if (m_connectionPollTimer >= CONNECTION_POLL_INTERVAL)
        m_connectionPollTimer = 0.0f;
}

// ============================================================================
// Connection
// ============================================================================

bool GamepadInput::IsConnected(int controllerIndex) const
{
    if (controllerIndex < 0 || controllerIndex >= MAX_CONTROLLERS)
        return false;
    return m_states[controllerIndex].connected;
}

int GamepadInput::GetConnectedCount() const
{
    int count = 0;
    for (const auto& state : m_states)
        if (state.connected)
            count++;
    return count;
}

// ============================================================================
// Buttons
// ============================================================================

bool GamepadInput::IsButtonDown(GamepadButton button, int controllerIndex) const
{
    if (!IsConnected(controllerIndex))
        return false;
    return (m_states[controllerIndex].xinputState.Gamepad.wButtons & GetXInputButton(button)) != 0;
}

bool GamepadInput::WasButtonPressed(GamepadButton button, int controllerIndex) const
{
    if (!IsConnected(controllerIndex))
        return false;
    WORD btn = GetXInputButton(button);
    bool current = (m_states[controllerIndex].xinputState.Gamepad.wButtons & btn) != 0;
    bool previous = (m_states[controllerIndex].prevXinputState.Gamepad.wButtons & btn) != 0;
    return current && !previous;
}

bool GamepadInput::WasButtonReleased(GamepadButton button, int controllerIndex) const
{
    if (!IsConnected(controllerIndex))
        return false;
    WORD btn = GetXInputButton(button);
    bool current = (m_states[controllerIndex].xinputState.Gamepad.wButtons & btn) != 0;
    bool previous = (m_states[controllerIndex].prevXinputState.Gamepad.wButtons & btn) != 0;
    return !current && previous;
}

// ============================================================================
// Thumbsticks
// ============================================================================

XMFLOAT2 GamepadInput::GetLeftStick(int controllerIndex) const
{
    if (!IsConnected(controllerIndex))
        return {0, 0};
    return m_states[controllerIndex].leftStick;
}

XMFLOAT2 GamepadInput::GetRightStick(int controllerIndex) const
{
    if (!IsConnected(controllerIndex))
        return {0, 0};
    return m_states[controllerIndex].rightStick;
}

float GamepadInput::GetLeftStickX(int controllerIndex) const
{
    return GetLeftStick(controllerIndex).x;
}
float GamepadInput::GetLeftStickY(int controllerIndex) const
{
    return GetLeftStick(controllerIndex).y;
}
float GamepadInput::GetRightStickX(int controllerIndex) const
{
    return GetRightStick(controllerIndex).x;
}
float GamepadInput::GetRightStickY(int controllerIndex) const
{
    return GetRightStick(controllerIndex).y;
}

// ============================================================================
// Triggers
// ============================================================================

float GamepadInput::GetLeftTrigger(int controllerIndex) const
{
    if (!IsConnected(controllerIndex))
        return 0.0f;
    return m_states[controllerIndex].leftTrigger;
}

float GamepadInput::GetRightTrigger(int controllerIndex) const
{
    if (!IsConnected(controllerIndex))
        return 0.0f;
    return m_states[controllerIndex].rightTrigger;
}

bool GamepadInput::IsTriggerDown(GamepadTrigger trigger, float threshold, int controllerIndex) const
{
    float value =
        (trigger == GamepadTrigger::Left) ? GetLeftTrigger(controllerIndex) : GetRightTrigger(controllerIndex);
    return value >= threshold;
}

// ============================================================================
// Vibration
// ============================================================================

void GamepadInput::SetVibration(float leftMotor, float rightMotor, float duration, int controllerIndex)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Input);
    SPARK_VALIDATE_RANGE(Spark::LogCategory::Input, controllerIndex, 0, MAX_CONTROLLERS - 1);
    if (controllerIndex < 0 || controllerIndex >= MAX_CONTROLLERS)
        return;

    auto& state = m_states[controllerIndex];
    state.leftMotor = std::clamp(leftMotor, 0.0f, 1.0f);
    state.rightMotor = std::clamp(rightMotor, 0.0f, 1.0f);
    state.vibrationTimer = duration;

    XINPUT_VIBRATION vibration;
    vibration.wLeftMotorSpeed = static_cast<WORD>(state.leftMotor * 65535.0f);
    vibration.wRightMotorSpeed = static_cast<WORD>(state.rightMotor * 65535.0f);
    XInputSetState(controllerIndex, &vibration);
}

void GamepadInput::StopVibration(int controllerIndex)
{
    SPARK_WARN_IF(Spark::LogCategory::Input, controllerIndex < 0 || controllerIndex >= MAX_CONTROLLERS,
                  "StopVibration called with out-of-range controller index");
    if (controllerIndex < 0 || controllerIndex >= MAX_CONTROLLERS)
        return;

    auto& state = m_states[controllerIndex];
    state.leftMotor = 0.0f;
    state.rightMotor = 0.0f;
    state.vibrationTimer = 0.0f;

    XINPUT_VIBRATION vibration = {};
    XInputSetState(controllerIndex, &vibration);
}

void GamepadInput::StopAllVibrations()
{
    SPARK_LOG_INFO(Spark::LogCategory::Input, "Stopping all gamepad vibrations");
    for (int i = 0; i < MAX_CONTROLLERS; ++i)
        StopVibration(i);
}

// ============================================================================
// Configuration
// ============================================================================

void GamepadInput::SetDeadZone(GamepadStick stick, float deadZone)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Input);
    SPARK_WARN_IF(Spark::LogCategory::Input, deadZone < 0.0f || deadZone > 1.0f,
                  "Dead zone value out of [0,1] range, will be clamped");
    deadZone = std::clamp(deadZone, 0.0f, 1.0f);
    if (stick == GamepadStick::Left)
        m_leftStickDeadZone = deadZone;
    else
        m_rightStickDeadZone = deadZone;
}

float GamepadInput::GetDeadZone(GamepadStick stick) const
{
    return (stick == GamepadStick::Left) ? m_leftStickDeadZone : m_rightStickDeadZone;
}

// ============================================================================
// Action Mapping
// ============================================================================

void GamepadInput::BindAction(const std::string& action, GamepadButton button)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Input);
    SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Input, action);
    ActionBinding binding;
    binding.type = ActionBinding::Type::Button;
    binding.button = button;
    binding.trigger = GamepadTrigger::Left;
    binding.triggerThreshold = 0.5f;
    m_actionBindings[action] = binding;
}

void GamepadInput::BindAction(const std::string& action, GamepadTrigger trigger, float threshold)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Input);
    SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Input, action);
    ActionBinding binding;
    binding.type = ActionBinding::Type::Trigger;
    binding.button = GamepadButton::A;
    binding.trigger = trigger;
    binding.triggerThreshold = threshold;
    m_actionBindings[action] = binding;
}

bool GamepadInput::IsActionDown(const std::string& action, int controllerIndex) const
{
    auto it = m_actionBindings.find(action);
    if (it == m_actionBindings.end())
        return false;

    const auto& binding = it->second;
    if (binding.type == ActionBinding::Type::Button)
        return IsButtonDown(binding.button, controllerIndex);
    else
        return IsTriggerDown(binding.trigger, binding.triggerThreshold, controllerIndex);
}

bool GamepadInput::WasActionPressed(const std::string& action, int controllerIndex) const
{
    auto it = m_actionBindings.find(action);
    if (it == m_actionBindings.end())
        return false;

    const auto& binding = it->second;
    if (binding.type == ActionBinding::Type::Button)
        return WasButtonPressed(binding.button, controllerIndex);
    return false; // Trigger "pressed" detection would need previous state comparison
}

// ============================================================================
// Internal Helpers
// ============================================================================

WORD GamepadInput::GetXInputButton(GamepadButton button) const
{
    switch (button)
    {
    case GamepadButton::A:
        return XINPUT_GAMEPAD_A;
    case GamepadButton::B:
        return XINPUT_GAMEPAD_B;
    case GamepadButton::X:
        return XINPUT_GAMEPAD_X;
    case GamepadButton::Y:
        return XINPUT_GAMEPAD_Y;
    case GamepadButton::LeftBumper:
        return XINPUT_GAMEPAD_LEFT_SHOULDER;
    case GamepadButton::RightBumper:
        return XINPUT_GAMEPAD_RIGHT_SHOULDER;
    case GamepadButton::Back:
        return XINPUT_GAMEPAD_BACK;
    case GamepadButton::Start:
        return XINPUT_GAMEPAD_START;
    case GamepadButton::LeftStick:
        return XINPUT_GAMEPAD_LEFT_THUMB;
    case GamepadButton::RightStick:
        return XINPUT_GAMEPAD_RIGHT_THUMB;
    case GamepadButton::DPadUp:
        return XINPUT_GAMEPAD_DPAD_UP;
    case GamepadButton::DPadDown:
        return XINPUT_GAMEPAD_DPAD_DOWN;
    case GamepadButton::DPadLeft:
        return XINPUT_GAMEPAD_DPAD_LEFT;
    case GamepadButton::DPadRight:
        return XINPUT_GAMEPAD_DPAD_RIGHT;
    default:
        return 0;
    }
}

XMFLOAT2 GamepadInput::ApplyDeadZone(short rawX, short rawY, float deadZone) const
{
    float x = rawX / 32767.0f;
    float y = rawY / 32767.0f;

    if (m_deadZoneMode == DeadZoneMode::Circular)
    {
        float magnitude = sqrtf(x * x + y * y);
        if (magnitude <= deadZone)
            return {0, 0};

        // Remap from [deadZone..1] to [0..1]
        float range = 1.0f - deadZone;
        float normalized = (range > 0.0f) ? std::clamp((magnitude - deadZone) / range, 0.0f, 1.0f) : 0.0f;
        float scale = normalized / magnitude;
        return {x * scale, y * scale};
    }
    else // Axial
    {
        if (fabsf(x) < deadZone)
            x = 0.0f;
        else
            x = (x - (x > 0 ? deadZone : -deadZone)) / (1.0f - deadZone);

        if (fabsf(y) < deadZone)
            y = 0.0f;
        else
            y = (y - (y > 0 ? deadZone : -deadZone)) / (1.0f - deadZone);

        return {std::clamp(x, -1.0f, 1.0f), std::clamp(y, -1.0f, 1.0f)};
    }
}

float GamepadInput::NormalizeTrigger(BYTE raw) const
{
    float value = raw / 255.0f;
    if (value < m_triggerThreshold)
        return 0.0f;
    return (value - m_triggerThreshold) / (1.0f - m_triggerThreshold);
}

// ============================================================================
// Console Integration
// ============================================================================

std::string GamepadInput::Console_GetStatus() const
{
    std::ostringstream ss;
    ss << "=== Gamepad Status ===\n";
    for (int i = 0; i < MAX_CONTROLLERS; ++i)
    {
        const auto& state = m_states[i];
        ss << "Controller " << i << ": " << (state.connected ? "Connected" : "Disconnected") << "\n";
        if (state.connected)
        {
            ss << "  Left Stick: (" << state.leftStick.x << ", " << state.leftStick.y << ")\n"
               << "  Right Stick: (" << state.rightStick.x << ", " << state.rightStick.y << ")\n"
               << "  Left Trigger: " << state.leftTrigger << "\n"
               << "  Right Trigger: " << state.rightTrigger << "\n"
               << "  Vibration: L=" << state.leftMotor << " R=" << state.rightMotor << "\n";
        }
    }
    return ss.str();
}

std::string GamepadInput::Console_ListBindings() const
{
    std::ostringstream ss;
    ss << "=== Gamepad Action Bindings ===\n";
    for (const auto& pair : m_actionBindings)
    {
        ss << "  " << pair.first << " -> ";
        if (pair.second.type == ActionBinding::Type::Button)
            ss << "Button " << static_cast<int>(pair.second.button);
        else
            ss << "Trigger " << (pair.second.trigger == GamepadTrigger::Left ? "Left" : "Right");
        ss << "\n";
    }
    return ss.str();
}
