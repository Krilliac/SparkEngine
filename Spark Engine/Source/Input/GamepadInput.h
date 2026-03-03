/**
 * @file GamepadInput.h
 * @brief Xbox controller / gamepad input support via XInput
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides gamepad input handling including thumbstick dead zones,
 * trigger analog input, vibration/rumble feedback, and action mapping.
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <Xinput.h>
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <unordered_map>
#include <functional>
#include <array>

#pragma comment(lib, "xinput.lib")

using DirectX::XMFLOAT2;

/**
 * @brief Gamepad button identifiers
 */
enum class GamepadButton
{
    A,
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
    DPadRight,
    Count
};

/**
 * @brief Gamepad trigger identifiers
 */
enum class GamepadTrigger
{
    Left,
    Right
};

/**
 * @brief Gamepad thumbstick identifiers
 */
enum class GamepadStick
{
    Left,
    Right
};

/**
 * @brief Dead zone mode for thumbsticks
 */
enum class DeadZoneMode
{
    Circular,   ///< Radial dead zone (recommended)
    Axial       ///< Per-axis dead zone
};

/**
 * @brief Per-controller state
 */
struct GamepadState
{
    bool connected = false;
    XINPUT_STATE xinputState = {};
    XINPUT_STATE prevXinputState = {};

    // Processed values
    XMFLOAT2 leftStick = {0, 0};
    XMFLOAT2 rightStick = {0, 0};
    float leftTrigger = 0.0f;
    float rightTrigger = 0.0f;

    // Vibration
    float leftMotor = 0.0f;
    float rightMotor = 0.0f;
    float vibrationTimer = 0.0f;
};

/**
 * @brief Gamepad input manager supporting up to 4 controllers
 */
class GamepadInput
{
public:
    static constexpr int MAX_CONTROLLERS = 4;

    GamepadInput();
    ~GamepadInput();

    /**
     * @brief Update all controller states
     */
    void Update(float deltaTime);

    // ============================================================================
    // Connection
    // ============================================================================

    bool IsConnected(int controllerIndex = 0) const;
    int GetConnectedCount() const;

    // ============================================================================
    // Buttons
    // ============================================================================

    bool IsButtonDown(GamepadButton button, int controllerIndex = 0) const;
    bool WasButtonPressed(GamepadButton button, int controllerIndex = 0) const;
    bool WasButtonReleased(GamepadButton button, int controllerIndex = 0) const;

    // ============================================================================
    // Thumbsticks (dead zone applied)
    // ============================================================================

    XMFLOAT2 GetLeftStick(int controllerIndex = 0) const;
    XMFLOAT2 GetRightStick(int controllerIndex = 0) const;

    float GetLeftStickX(int controllerIndex = 0) const;
    float GetLeftStickY(int controllerIndex = 0) const;
    float GetRightStickX(int controllerIndex = 0) const;
    float GetRightStickY(int controllerIndex = 0) const;

    // ============================================================================
    // Triggers (0.0 to 1.0)
    // ============================================================================

    float GetLeftTrigger(int controllerIndex = 0) const;
    float GetRightTrigger(int controllerIndex = 0) const;
    bool IsTriggerDown(GamepadTrigger trigger, float threshold = 0.5f, int controllerIndex = 0) const;

    // ============================================================================
    // Vibration / Rumble
    // ============================================================================

    /**
     * @brief Set vibration for a controller
     * @param leftMotor Left motor intensity (0.0 - 1.0) - low frequency rumble
     * @param rightMotor Right motor intensity (0.0 - 1.0) - high frequency rumble
     * @param duration Duration in seconds (0 = indefinite)
     */
    void SetVibration(float leftMotor, float rightMotor, float duration = 0.0f, int controllerIndex = 0);

    /**
     * @brief Stop vibration on a controller
     */
    void StopVibration(int controllerIndex = 0);

    /**
     * @brief Stop all vibrations on all controllers
     */
    void StopAllVibrations();

    // ============================================================================
    // Configuration
    // ============================================================================

    void SetDeadZone(GamepadStick stick, float deadZone);
    float GetDeadZone(GamepadStick stick) const;
    void SetDeadZoneMode(DeadZoneMode mode) { m_deadZoneMode = mode; }

    void SetTriggerThreshold(float threshold) { m_triggerThreshold = threshold; }
    void SetStickSensitivity(float sensitivity) { m_stickSensitivity = sensitivity; }

    // ============================================================================
    // Action Mapping (maps gamepad buttons to named actions)
    // ============================================================================

    void BindAction(const std::string& action, GamepadButton button);
    void BindAction(const std::string& action, GamepadTrigger trigger, float threshold = 0.5f);
    bool IsActionDown(const std::string& action, int controllerIndex = 0) const;
    bool WasActionPressed(const std::string& action, int controllerIndex = 0) const;

    // ============================================================================
    // Console Integration
    // ============================================================================

    std::string Console_GetStatus() const;
    std::string Console_ListBindings() const;

private:
    WORD GetXInputButton(GamepadButton button) const;
    XMFLOAT2 ApplyDeadZone(short rawX, short rawY, float deadZone) const;
    float NormalizeTrigger(BYTE raw) const;

    std::array<GamepadState, MAX_CONTROLLERS> m_states;

    // Dead zone settings
    float m_leftStickDeadZone = 0.24f;   // ~7849 / 32767
    float m_rightStickDeadZone = 0.26f;  // ~8689 / 32767
    float m_triggerThreshold = 0.12f;     // ~30 / 255
    DeadZoneMode m_deadZoneMode = DeadZoneMode::Circular;
    float m_stickSensitivity = 1.0f;

    // Action bindings
    struct ActionBinding {
        enum class Type { Button, Trigger };
        Type type;
        GamepadButton button;
        GamepadTrigger trigger;
        float triggerThreshold;
    };
    std::unordered_map<std::string, ActionBinding> m_actionBindings;

    // Connection polling (don't check every frame for disconnected controllers)
    float m_connectionPollTimer = 0.0f;
    static constexpr float CONNECTION_POLL_INTERVAL = 2.0f;
};
