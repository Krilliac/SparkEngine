/**
 * @file TestGamepadInputProcessing.cpp
 * @brief Tests for GamepadInput: dead zone processing (circular vs axial),
 *        stick normalization, trigger thresholds, vibration timers, and action bindings.
 *
 * Standalone reimplementation — mirrors Spark::GamepadInput
 * without XInput/DirectXMath dependencies for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

    constexpr int MAX_CONTROLLERS = 4;

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

    enum class GamepadTrigger
    {
        Left,
        Right
    };

    enum class GamepadStick
    {
        Left,
        Right
    };

    enum class DeadZoneMode
    {
        Circular,
        Axial
    };

    struct Vec2
    {
        float x = 0.0f;
        float y = 0.0f;

        float Length() const { return std::sqrt(x * x + y * y); }
    };

    struct GamepadState
    {
        bool connected = false;
        bool buttons[static_cast<int>(GamepadButton::Count)] = {};
        bool prevButtons[static_cast<int>(GamepadButton::Count)] = {};
        Vec2 rawLeftStick{};
        Vec2 rawRightStick{};
        float rawLeftTrigger = 0.0f;
        float rawRightTrigger = 0.0f;
        float leftMotor = 0.0f;
        float rightMotor = 0.0f;
        float vibrationTimer = 0.0f;
    };

    class GamepadInput
    {
      public:
        void Update(float deltaTime)
        {
            for (int i = 0; i < MAX_CONTROLLERS; i++)
            {
                auto& state = m_states[i];
                if (!state.connected)
                    continue;

                // Copy current to previous
                for (int b = 0; b < static_cast<int>(GamepadButton::Count); b++)
                    state.prevButtons[b] = state.buttons[b];

                // Process sticks
                m_processedLeftStick[i] = ProcessStick(state.rawLeftStick, m_leftDeadZone);
                m_processedRightStick[i] = ProcessStick(state.rawRightStick, m_rightDeadZone);

                // Apply sensitivity
                m_processedLeftStick[i].x *= m_stickSensitivity;
                m_processedLeftStick[i].y *= m_stickSensitivity;
                m_processedRightStick[i].x *= m_stickSensitivity;
                m_processedRightStick[i].y *= m_stickSensitivity;

                // Process triggers
                m_processedLeftTrigger[i] = ProcessTrigger(state.rawLeftTrigger);
                m_processedRightTrigger[i] = ProcessTrigger(state.rawRightTrigger);

                // Update vibration timer
                if (state.vibrationTimer > 0.0f)
                {
                    state.vibrationTimer -= deltaTime;
                    if (state.vibrationTimer <= 0.0f)
                    {
                        state.leftMotor = 0.0f;
                        state.rightMotor = 0.0f;
                        state.vibrationTimer = 0.0f;
                    }
                }
            }
        }

        // Simulate gamepad state for testing
        void SetConnected(int index, bool connected)
        {
            if (index >= 0 && index < MAX_CONTROLLERS)
                m_states[index].connected = connected;
        }

        void SetRawStick(GamepadStick stick, float x, float y, int index = 0)
        {
            if (index < 0 || index >= MAX_CONTROLLERS)
                return;
            if (stick == GamepadStick::Left)
                m_states[index].rawLeftStick = {x, y};
            else
                m_states[index].rawRightStick = {x, y};
        }

        void SetRawTrigger(GamepadTrigger trigger, float value, int index = 0)
        {
            if (index < 0 || index >= MAX_CONTROLLERS)
                return;
            if (trigger == GamepadTrigger::Left)
                m_states[index].rawLeftTrigger = value;
            else
                m_states[index].rawRightTrigger = value;
        }

        void SetButton(GamepadButton button, bool pressed, int index = 0)
        {
            if (index >= 0 && index < MAX_CONTROLLERS)
                m_states[index].buttons[static_cast<int>(button)] = pressed;
        }

        // Queries
        bool IsConnected(int index = 0) const
        {
            return (index >= 0 && index < MAX_CONTROLLERS) ? m_states[index].connected : false;
        }

        int GetConnectedCount() const
        {
            int count = 0;
            for (int i = 0; i < MAX_CONTROLLERS; i++)
                if (m_states[i].connected)
                    count++;
            return count;
        }

        bool IsButtonDown(GamepadButton button, int index = 0) const
        {
            if (index < 0 || index >= MAX_CONTROLLERS)
                return false;
            return m_states[index].buttons[static_cast<int>(button)];
        }

        bool WasButtonPressed(GamepadButton button, int index = 0) const
        {
            if (index < 0 || index >= MAX_CONTROLLERS)
                return false;
            int b = static_cast<int>(button);
            return m_states[index].buttons[b] && !m_states[index].prevButtons[b];
        }

        bool WasButtonReleased(GamepadButton button, int index = 0) const
        {
            if (index < 0 || index >= MAX_CONTROLLERS)
                return false;
            int b = static_cast<int>(button);
            return !m_states[index].buttons[b] && m_states[index].prevButtons[b];
        }

        Vec2 GetLeftStick(int index = 0) const
        {
            return (index >= 0 && index < MAX_CONTROLLERS) ? m_processedLeftStick[index] : Vec2{};
        }

        Vec2 GetRightStick(int index = 0) const
        {
            return (index >= 0 && index < MAX_CONTROLLERS) ? m_processedRightStick[index] : Vec2{};
        }

        float GetLeftTrigger(int index = 0) const
        {
            return (index >= 0 && index < MAX_CONTROLLERS) ? m_processedLeftTrigger[index] : 0.0f;
        }

        float GetRightTrigger(int index = 0) const
        {
            return (index >= 0 && index < MAX_CONTROLLERS) ? m_processedRightTrigger[index] : 0.0f;
        }

        bool IsTriggerDown(GamepadTrigger trigger, float threshold = 0.5f, int index = 0) const
        {
            float val = (trigger == GamepadTrigger::Left) ? GetLeftTrigger(index) : GetRightTrigger(index);
            return val >= threshold;
        }

        void SetVibration(float leftMotor, float rightMotor, float duration = 0.0f, int index = 0)
        {
            if (index < 0 || index >= MAX_CONTROLLERS)
                return;
            m_states[index].leftMotor = std::clamp(leftMotor, 0.0f, 1.0f);
            m_states[index].rightMotor = std::clamp(rightMotor, 0.0f, 1.0f);
            m_states[index].vibrationTimer = duration;
        }

        void StopVibration(int index = 0)
        {
            if (index >= 0 && index < MAX_CONTROLLERS)
            {
                m_states[index].leftMotor = 0.0f;
                m_states[index].rightMotor = 0.0f;
                m_states[index].vibrationTimer = 0.0f;
            }
        }

        // Dead zone settings
        void SetDeadZone(GamepadStick stick, float deadZone)
        {
            if (stick == GamepadStick::Left)
                m_leftDeadZone = deadZone;
            else
                m_rightDeadZone = deadZone;
        }

        float GetDeadZone(GamepadStick stick) const
        {
            return (stick == GamepadStick::Left) ? m_leftDeadZone : m_rightDeadZone;
        }

        void SetDeadZoneMode(DeadZoneMode mode) { m_deadZoneMode = mode; }
        void SetTriggerThreshold(float threshold) { m_triggerThreshold = threshold; }
        void SetStickSensitivity(float sensitivity) { m_stickSensitivity = sensitivity; }

        // Action bindings
        void BindAction(const std::string& action, GamepadButton button) { m_actionBindings[action] = button; }

        bool IsActionDown(const std::string& action, int index = 0) const
        {
            auto it = m_actionBindings.find(action);
            if (it == m_actionBindings.end())
                return false;
            return IsButtonDown(it->second, index);
        }

        float GetVibrationLeft(int index = 0) const
        {
            return (index >= 0 && index < MAX_CONTROLLERS) ? m_states[index].leftMotor : 0.0f;
        }

        float GetVibrationRight(int index = 0) const
        {
            return (index >= 0 && index < MAX_CONTROLLERS) ? m_states[index].rightMotor : 0.0f;
        }

      private:
        Vec2 ProcessStick(const Vec2& raw, float deadZone) const
        {
            if (m_deadZoneMode == DeadZoneMode::Circular)
            {
                float magnitude = raw.Length();
                if (magnitude < deadZone)
                    return {0.0f, 0.0f};
                // Remap [deadZone, 1] to [0, 1]
                float normalized = (magnitude - deadZone) / (1.0f - deadZone);
                normalized = std::min(normalized, 1.0f);
                return {raw.x / magnitude * normalized, raw.y / magnitude * normalized};
            }
            else
            {
                // Axial — each axis independent
                float x = (std::abs(raw.x) < deadZone) ? 0.0f : raw.x;
                float y = (std::abs(raw.y) < deadZone) ? 0.0f : raw.y;
                return {x, y};
            }
        }

        float ProcessTrigger(float raw) const
        {
            if (raw < m_triggerThreshold)
                return 0.0f;
            return (raw - m_triggerThreshold) / (1.0f - m_triggerThreshold);
        }

        GamepadState m_states[MAX_CONTROLLERS]{};
        Vec2 m_processedLeftStick[MAX_CONTROLLERS]{};
        Vec2 m_processedRightStick[MAX_CONTROLLERS]{};
        float m_processedLeftTrigger[MAX_CONTROLLERS]{};
        float m_processedRightTrigger[MAX_CONTROLLERS]{};

        DeadZoneMode m_deadZoneMode = DeadZoneMode::Circular;
        float m_leftDeadZone = 0.24f;
        float m_rightDeadZone = 0.24f;
        float m_triggerThreshold = 0.12f;
        float m_stickSensitivity = 1.0f;

        std::unordered_map<std::string, GamepadButton> m_actionBindings;
    };

} // anonymous namespace

// ============================================================================
// Connection Tests
// ============================================================================

TEST(GamepadInput_NotConnectedByDefault)
{
    GamepadInput input;
    EXPECT_FALSE(input.IsConnected(0));
    EXPECT_EQ(input.GetConnectedCount(), 0);
}

TEST(GamepadInput_ConnectController)
{
    GamepadInput input;
    input.SetConnected(0, true);
    EXPECT_TRUE(input.IsConnected(0));
    EXPECT_EQ(input.GetConnectedCount(), 1);
}

TEST(GamepadInput_MultipleControllers)
{
    GamepadInput input;
    input.SetConnected(0, true);
    input.SetConnected(1, true);
    input.SetConnected(3, true);
    EXPECT_EQ(input.GetConnectedCount(), 3);
}

// ============================================================================
// Circular Dead Zone Tests
// ============================================================================

TEST(GamepadInput_CircularDeadZone_InsideZone)
{
    GamepadInput input;
    input.SetConnected(0, true);
    input.SetDeadZone(GamepadStick::Left, 0.25f);

    // Small stick movement — inside dead zone
    input.SetRawStick(GamepadStick::Left, 0.1f, 0.1f);
    input.Update(0.016f);

    auto stick = input.GetLeftStick();
    EXPECT_NEAR(stick.x, 0.0f, 0.01f);
    EXPECT_NEAR(stick.y, 0.0f, 0.01f);
}

TEST(GamepadInput_CircularDeadZone_OutsideZone)
{
    GamepadInput input;
    input.SetConnected(0, true);
    input.SetDeadZone(GamepadStick::Left, 0.25f);

    // Large stick movement — outside dead zone
    input.SetRawStick(GamepadStick::Left, 0.8f, 0.0f);
    input.Update(0.016f);

    auto stick = input.GetLeftStick();
    EXPECT_GT(stick.x, 0.0f);
}

TEST(GamepadInput_CircularDeadZone_FullDeflection)
{
    GamepadInput input;
    input.SetConnected(0, true);
    input.SetDeadZone(GamepadStick::Left, 0.25f);

    input.SetRawStick(GamepadStick::Left, 1.0f, 0.0f);
    input.Update(0.016f);

    auto stick = input.GetLeftStick();
    EXPECT_NEAR(stick.x, 1.0f, 0.01f);
}

TEST(GamepadInput_AxialDeadZone)
{
    GamepadInput input;
    input.SetConnected(0, true);
    input.SetDeadZoneMode(DeadZoneMode::Axial);
    input.SetDeadZone(GamepadStick::Left, 0.25f);

    // X above dead zone, Y below — only X passes
    input.SetRawStick(GamepadStick::Left, 0.5f, 0.1f);
    input.Update(0.016f);

    auto stick = input.GetLeftStick();
    EXPECT_GT(std::abs(stick.x), 0.0f);
    EXPECT_NEAR(stick.y, 0.0f, 0.01f);
}

// ============================================================================
// Stick Normalization Tests
// ============================================================================

TEST(GamepadInput_StickDirectionPreserved)
{
    GamepadInput input;
    input.SetConnected(0, true);
    input.SetDeadZone(GamepadStick::Left, 0.1f);

    // Diagonal input
    input.SetRawStick(GamepadStick::Left, 0.7f, 0.7f);
    input.Update(0.016f);

    auto stick = input.GetLeftStick();
    // Direction should be roughly 45 degrees
    EXPECT_NEAR(stick.x, stick.y, 0.05f);
}

TEST(GamepadInput_StickSensitivity)
{
    GamepadInput input;
    input.SetConnected(0, true);
    input.SetDeadZone(GamepadStick::Left, 0.0f);
    input.SetStickSensitivity(2.0f);

    input.SetRawStick(GamepadStick::Left, 0.5f, 0.0f);
    input.Update(0.016f);

    auto stick = input.GetLeftStick();
    EXPECT_NEAR(stick.x, 1.0f, 0.01f); // 0.5 * 2.0
}

// ============================================================================
// Trigger Threshold Tests
// ============================================================================

TEST(GamepadInput_TriggerBelowThreshold)
{
    GamepadInput input;
    input.SetConnected(0, true);
    input.SetTriggerThreshold(0.12f);

    input.SetRawTrigger(GamepadTrigger::Left, 0.05f);
    input.Update(0.016f);

    EXPECT_NEAR(input.GetLeftTrigger(), 0.0f, 0.01f);
    EXPECT_FALSE(input.IsTriggerDown(GamepadTrigger::Left));
}

TEST(GamepadInput_TriggerAboveThreshold)
{
    GamepadInput input;
    input.SetConnected(0, true);
    input.SetTriggerThreshold(0.12f);

    input.SetRawTrigger(GamepadTrigger::Left, 1.0f);
    input.Update(0.016f);

    EXPECT_GT(input.GetLeftTrigger(), 0.0f);
    EXPECT_TRUE(input.IsTriggerDown(GamepadTrigger::Left));
}

TEST(GamepadInput_TriggerFullDeflection)
{
    GamepadInput input;
    input.SetConnected(0, true);
    input.SetTriggerThreshold(0.12f);

    input.SetRawTrigger(GamepadTrigger::Right, 1.0f);
    input.Update(0.016f);

    EXPECT_NEAR(input.GetRightTrigger(), 1.0f, 0.01f);
}

// ============================================================================
// Button Tests
// ============================================================================

TEST(GamepadInput_ButtonDown)
{
    GamepadInput input;
    input.SetConnected(0, true);

    input.SetButton(GamepadButton::A, true);
    EXPECT_TRUE(input.IsButtonDown(GamepadButton::A));
    EXPECT_FALSE(input.IsButtonDown(GamepadButton::B));
}

TEST(GamepadInput_WasButtonPressed)
{
    GamepadInput input;
    input.SetConnected(0, true);

    input.Update(0.016f); // frame 0 — snapshots empty prev
    input.SetButton(GamepadButton::A, true);
    // current=A, prev=empty → pressed
    EXPECT_TRUE(input.WasButtonPressed(GamepadButton::A));

    input.Update(0.016f); // frame 1 — prev=A, current=A
    EXPECT_FALSE(input.WasButtonPressed(GamepadButton::A));
}

TEST(GamepadInput_WasButtonReleased)
{
    GamepadInput input;
    input.SetConnected(0, true);

    input.SetButton(GamepadButton::B, true);
    input.Update(0.016f); // prev=B
    input.SetButton(GamepadButton::B, false);
    // current=false, prev=true → released
    EXPECT_TRUE(input.WasButtonReleased(GamepadButton::B));
}

// ============================================================================
// Vibration Timer Tests
// ============================================================================

TEST(GamepadInput_VibrationWithDuration)
{
    GamepadInput input;
    input.SetConnected(0, true);

    input.SetVibration(0.5f, 0.8f, 1.0f);
    EXPECT_NEAR(input.GetVibrationLeft(), 0.5f, 0.01f);
    EXPECT_NEAR(input.GetVibrationRight(), 0.8f, 0.01f);

    // After 1.5 seconds — vibration should have stopped
    input.Update(1.5f);

    EXPECT_NEAR(input.GetVibrationLeft(), 0.0f, 0.01f);
    EXPECT_NEAR(input.GetVibrationRight(), 0.0f, 0.01f);
}

TEST(GamepadInput_StopVibration)
{
    GamepadInput input;
    input.SetConnected(0, true);

    input.SetVibration(1.0f, 1.0f, 10.0f);
    input.StopVibration();

    EXPECT_NEAR(input.GetVibrationLeft(), 0.0f, 0.01f);
    EXPECT_NEAR(input.GetVibrationRight(), 0.0f, 0.01f);
}

TEST(GamepadInput_VibrationClampedToOne)
{
    GamepadInput input;
    input.SetConnected(0, true);

    input.SetVibration(2.0f, -0.5f);
    EXPECT_NEAR(input.GetVibrationLeft(), 1.0f, 0.01f);
    EXPECT_NEAR(input.GetVibrationRight(), 0.0f, 0.01f);
}

// ============================================================================
// Action Binding Tests
// ============================================================================

TEST(GamepadInput_BindAndCheckAction)
{
    GamepadInput input;
    input.SetConnected(0, true);

    input.BindAction("Jump", GamepadButton::A);
    input.BindAction("Fire", GamepadButton::RightBumper);

    input.SetButton(GamepadButton::A, true);
    EXPECT_TRUE(input.IsActionDown("Jump"));
    EXPECT_FALSE(input.IsActionDown("Fire"));
}

TEST(GamepadInput_UnboundActionReturnsFalse)
{
    GamepadInput input;
    input.SetConnected(0, true);

    EXPECT_FALSE(input.IsActionDown("NonExistent"));
}

// ============================================================================
// Dead Zone Configuration Tests
// ============================================================================

TEST(GamepadInput_DefaultDeadZones)
{
    GamepadInput input;
    EXPECT_NEAR(input.GetDeadZone(GamepadStick::Left), 0.24f, 0.01f);
    EXPECT_NEAR(input.GetDeadZone(GamepadStick::Right), 0.24f, 0.01f);
}

TEST(GamepadInput_SetDeadZone)
{
    GamepadInput input;
    input.SetDeadZone(GamepadStick::Left, 0.15f);
    input.SetDeadZone(GamepadStick::Right, 0.3f);

    EXPECT_NEAR(input.GetDeadZone(GamepadStick::Left), 0.15f, 0.01f);
    EXPECT_NEAR(input.GetDeadZone(GamepadStick::Right), 0.3f, 0.01f);
}

// ============================================================================
// Out-of-Bounds Controller Index Tests
// ============================================================================

TEST(GamepadInput_InvalidControllerIndex)
{
    GamepadInput input;
    EXPECT_FALSE(input.IsConnected(-1));
    EXPECT_FALSE(input.IsConnected(MAX_CONTROLLERS));
    EXPECT_FALSE(input.IsButtonDown(GamepadButton::A, -1));
    EXPECT_NEAR(input.GetLeftTrigger(MAX_CONTROLLERS), 0.0f, 0.01f);
}
