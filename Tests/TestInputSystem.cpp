// TestInputSystem.cpp - Tests for input system logic (action mapping, axis bindings)
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <algorithm>

namespace TestInput {

enum class KeyState { Up, Down };

class MockInputState {
public:
    void SetKey(int key, KeyState state) { m_keys[key] = state; }
    bool IsKeyDown(int key) const {
        auto it = m_keys.find(key);
        return (it != m_keys.end()) ? (it->second == KeyState::Down) : false;
    }

private:
    std::unordered_map<int, KeyState> m_keys;
};

struct ActionBinding {
    std::string name;
    int key;
};

struct AxisBinding {
    std::string name;
    int positiveKey;
    int negativeKey;
    float scale;
};

class MockActionMapper {
public:
    void BindAction(const std::string& name, int key) {
        m_actions.push_back({name, key});
    }

    void BindAxis(const std::string& name, int posKey, int negKey, float scale = 1.0f) {
        m_axes.push_back({name, posKey, negKey, scale});
    }

    bool IsActionActive(const std::string& name, const MockInputState& state) const {
        for (const auto& a : m_actions) {
            if (a.name == name && state.IsKeyDown(a.key)) return true;
        }
        return false;
    }

    float GetAxisValue(const std::string& name, const MockInputState& state) const {
        float result = 0.0f;
        for (const auto& a : m_axes) {
            if (a.name == name) {
                if (state.IsKeyDown(a.positiveKey)) result += a.scale;
                if (state.IsKeyDown(a.negativeKey)) result -= a.scale;
            }
        }
        return result;
    }

    void RemoveAction(const std::string& name) {
        m_actions.erase(
            std::remove_if(m_actions.begin(), m_actions.end(),
                            [&](const ActionBinding& a) { return a.name == name; }),
            m_actions.end());
    }

    size_t GetActionCount() const { return m_actions.size(); }
    size_t GetAxisCount() const { return m_axes.size(); }

private:
    std::vector<ActionBinding> m_actions;
    std::vector<AxisBinding> m_axes;
};

// Dead zone helper
float ApplyDeadZone(float value, float deadZone) {
    if (std::abs(value) < deadZone) return 0.0f;
    return value;
}

} // namespace TestInput

TEST(ActionMapping_BasicBind)
{
    TestInput::MockInputState state;
    TestInput::MockActionMapper mapper;
    mapper.BindAction("jump", 32);  // Space

    state.SetKey(32, TestInput::KeyState::Down);
    EXPECT_TRUE(mapper.IsActionActive("jump", state));
}

TEST(ActionMapping_InactiveWhenKeyUp)
{
    TestInput::MockInputState state;
    TestInput::MockActionMapper mapper;
    mapper.BindAction("jump", 32);

    state.SetKey(32, TestInput::KeyState::Up);
    EXPECT_FALSE(mapper.IsActionActive("jump", state));
}

TEST(ActionMapping_UnboundAction)
{
    TestInput::MockInputState state;
    TestInput::MockActionMapper mapper;
    EXPECT_FALSE(mapper.IsActionActive("nonexistent", state));
}

TEST(ActionMapping_RemoveBinding)
{
    TestInput::MockInputState state;
    TestInput::MockActionMapper mapper;
    mapper.BindAction("fire", 1);
    EXPECT_EQ(mapper.GetActionCount(), 1u);

    mapper.RemoveAction("fire");
    EXPECT_EQ(mapper.GetActionCount(), 0u);
}

TEST(AxisMapping_PositiveOnly)
{
    TestInput::MockInputState state;
    TestInput::MockActionMapper mapper;
    mapper.BindAxis("moveX", 68, 65);  // D/A

    state.SetKey(68, TestInput::KeyState::Down);  // D pressed
    EXPECT_NEAR(mapper.GetAxisValue("moveX", state), 1.0f, 0.001f);
}

TEST(AxisMapping_NegativeOnly)
{
    TestInput::MockInputState state;
    TestInput::MockActionMapper mapper;
    mapper.BindAxis("moveX", 68, 65);

    state.SetKey(65, TestInput::KeyState::Down);  // A pressed
    EXPECT_NEAR(mapper.GetAxisValue("moveX", state), -1.0f, 0.001f);
}

TEST(AxisMapping_BothKeysCancel)
{
    TestInput::MockInputState state;
    TestInput::MockActionMapper mapper;
    mapper.BindAxis("moveX", 68, 65);

    state.SetKey(68, TestInput::KeyState::Down);
    state.SetKey(65, TestInput::KeyState::Down);
    EXPECT_NEAR(mapper.GetAxisValue("moveX", state), 0.0f, 0.001f);
}

TEST(AxisMapping_NeitherKey)
{
    TestInput::MockInputState state;
    TestInput::MockActionMapper mapper;
    mapper.BindAxis("moveX", 68, 65);
    EXPECT_NEAR(mapper.GetAxisValue("moveX", state), 0.0f, 0.001f);
}

TEST(AxisMapping_CustomScale)
{
    TestInput::MockInputState state;
    TestInput::MockActionMapper mapper;
    mapper.BindAxis("moveX", 68, 65, 2.0f);

    state.SetKey(68, TestInput::KeyState::Down);
    EXPECT_NEAR(mapper.GetAxisValue("moveX", state), 2.0f, 0.001f);
}

TEST(DeadZone_BelowThreshold)
{
    EXPECT_NEAR(TestInput::ApplyDeadZone(0.05f, 0.15f), 0.0f, 0.001f);
    EXPECT_NEAR(TestInput::ApplyDeadZone(-0.05f, 0.15f), 0.0f, 0.001f);
}

TEST(DeadZone_AboveThreshold)
{
    EXPECT_NEAR(TestInput::ApplyDeadZone(0.5f, 0.15f), 0.5f, 0.001f);
    EXPECT_NEAR(TestInput::ApplyDeadZone(-0.5f, 0.15f), -0.5f, 0.001f);
}
