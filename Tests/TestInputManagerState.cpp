/**
 * @file TestInputManagerState.cpp
 * @brief Tests for InputManager: key state edge detection, mouse delta with
 *        dead zone, sensitivity scaling, mouse capture, and input metrics.
 *
 * Standalone reimplementation — mirrors Spark::InputManager
 * without Win32/HWND dependencies for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{

    struct MousePoint
    {
        float dx = 0.0f;
        float dy = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
    };

    struct InputMetrics
    {
        int keyPressCount = 0;
        int mousePressCount = 0;
        float totalMouseDistance = 0.0f;
        int activeKeys = 0;
        int activeMouseButtons = 0;
        bool mouseCaptured = false;
        float mouseSensitivity = 1.0f;
        float mouseDeadZone = 0.01f;
        bool mouseAcceleration = false;
        bool invertMouseY = false;
    };

    // --- InputManager reimplementation ---

    class InputManager
    {
      public:
        void Initialize()
        {
            m_currentKeys.clear();
            m_previousKeys.clear();
            m_currentMouseButtons.clear();
            m_previousMouseButtons.clear();
            m_mouseDelta = {0, 0, 0, 0};
            m_mousePosition = {0, 0, 0, 0};
            m_initialized = true;
        }

        void Update()
        {
            m_previousKeys = m_currentKeys;
            m_previousMouseButtons = m_currentMouseButtons;

            // Apply dead zone to mouse delta
            float dx = m_rawMouseDelta.dx;
            float dy = m_rawMouseDelta.dy;

            float magnitude = std::sqrt(dx * dx + dy * dy);
            if (magnitude < m_deadZone)
            {
                dx = 0.0f;
                dy = 0.0f;
            }

            // Apply sensitivity
            dx *= m_sensitivity;
            dy *= m_sensitivity;

            // Apply mouse acceleration
            if (m_mouseAcceleration && magnitude > 0.0f)
            {
                float accelFactor = 1.0f + magnitude * 0.01f;
                dx *= accelFactor;
                dy *= accelFactor;
            }

            // Apply invert Y
            if (m_invertMouseY)
                dy = -dy;

            m_mouseDelta = {dx, dy, 0, 0};
            m_metrics.totalMouseDistance += std::sqrt(dx * dx + dy * dy);

            // Reset raw delta for next frame
            m_rawMouseDelta = {0, 0, 0, 0};
        }

        // Simulate key events
        void SimulateKeyDown(int key)
        {
            if (!m_currentKeys.count(key))
                m_metrics.keyPressCount++;
            m_currentKeys.insert(key);
        }

        void SimulateKeyUp(int key) { m_currentKeys.erase(key); }

        void SimulateMouseButtonDown(int button)
        {
            if (!m_currentMouseButtons.count(button))
                m_metrics.mousePressCount++;
            m_currentMouseButtons.insert(button);
        }

        void SimulateMouseButtonUp(int button) { m_currentMouseButtons.erase(button); }

        void SimulateMouseMove(float dx, float dy)
        {
            m_rawMouseDelta.dx = dx;
            m_rawMouseDelta.dy = dy;
            m_mousePosition.x += dx;
            m_mousePosition.y += dy;
        }

        // Key state queries
        bool IsKeyDown(int key) const { return m_currentKeys.count(key) > 0; }
        bool IsKeyUp(int key) const { return m_currentKeys.count(key) == 0; }

        bool WasKeyPressed(int key) const { return m_currentKeys.count(key) > 0 && m_previousKeys.count(key) == 0; }

        bool WasKeyReleased(int key) const { return m_currentKeys.count(key) == 0 && m_previousKeys.count(key) > 0; }

        // Mouse button queries
        bool IsMouseButtonDown(int button) const { return m_currentMouseButtons.count(button) > 0; }

        bool WasMouseButtonPressed(int button) const
        {
            return m_currentMouseButtons.count(button) > 0 && m_previousMouseButtons.count(button) == 0;
        }

        bool WasMouseButtonReleased(int button) const
        {
            return m_currentMouseButtons.count(button) == 0 && m_previousMouseButtons.count(button) > 0;
        }

        MousePoint GetMouseDelta() const { return m_mouseDelta; }
        MousePoint GetMousePosition() const { return m_mousePosition; }

        void CaptureMouse(bool capture)
        {
            m_mouseCaptured = capture;
            m_metrics.mouseCaptured = capture;
        }

        bool IsMouseCaptured() const { return m_mouseCaptured; }

        // Settings
        void SetMouseSensitivity(float sensitivity)
        {
            m_sensitivity = sensitivity;
            m_metrics.mouseSensitivity = sensitivity;
        }

        void SetMouseDeadZone(float deadZone)
        {
            m_deadZone = deadZone;
            m_metrics.mouseDeadZone = deadZone;
        }

        void SetMouseAcceleration(bool enabled)
        {
            m_mouseAcceleration = enabled;
            m_metrics.mouseAcceleration = enabled;
        }

        void SetInvertMouseY(bool enabled)
        {
            m_invertMouseY = enabled;
            m_metrics.invertMouseY = enabled;
        }

        float GetMouseSensitivity() const { return m_sensitivity; }
        float GetMouseDeadZone() const { return m_deadZone; }

        InputMetrics GetMetrics() const
        {
            auto metrics = m_metrics;
            metrics.activeKeys = static_cast<int>(m_currentKeys.size());
            metrics.activeMouseButtons = static_cast<int>(m_currentMouseButtons.size());
            return metrics;
        }

        void ClearInputStates()
        {
            m_currentKeys.clear();
            m_previousKeys.clear();
            m_currentMouseButtons.clear();
            m_previousMouseButtons.clear();
            m_mouseDelta = {};
            m_rawMouseDelta = {};
        }

      private:
        bool m_initialized = false;
        std::unordered_set<int> m_currentKeys;
        std::unordered_set<int> m_previousKeys;
        std::unordered_set<int> m_currentMouseButtons;
        std::unordered_set<int> m_previousMouseButtons;
        MousePoint m_mouseDelta{};
        MousePoint m_rawMouseDelta{};
        MousePoint m_mousePosition{};
        bool m_mouseCaptured = false;
        float m_sensitivity = 1.0f;
        float m_deadZone = 0.01f;
        bool m_mouseAcceleration = false;
        bool m_invertMouseY = false;
        InputMetrics m_metrics{};
    };

    // Key constants
    constexpr int KEY_W = 0x57;
    constexpr int KEY_A = 0x41;
    constexpr int KEY_S = 0x53;
    constexpr int KEY_D = 0x44;
    constexpr int KEY_SPACE = 0x20;
    constexpr int MOUSE_LEFT = 0;
    constexpr int MOUSE_RIGHT = 1;
    constexpr int MOUSE_MIDDLE = 2;

} // anonymous namespace

// ============================================================================
// Key State Edge Detection Tests
// ============================================================================

TEST(InputManager_KeyDown)
{
    InputManager mgr;
    mgr.Initialize();

    EXPECT_FALSE(mgr.IsKeyDown(KEY_W));
    mgr.SimulateKeyDown(KEY_W);
    EXPECT_TRUE(mgr.IsKeyDown(KEY_W));
    EXPECT_FALSE(mgr.IsKeyUp(KEY_W));
}

TEST(InputManager_KeyUp)
{
    InputManager mgr;
    mgr.Initialize();

    mgr.SimulateKeyDown(KEY_W);
    mgr.SimulateKeyUp(KEY_W);
    EXPECT_FALSE(mgr.IsKeyDown(KEY_W));
    EXPECT_TRUE(mgr.IsKeyUp(KEY_W));
}

TEST(InputManager_WasKeyPressed)
{
    InputManager mgr;
    mgr.Initialize();

    mgr.Update(); // frame 0 — snapshots empty into previous
    mgr.SimulateKeyDown(KEY_W);
    // Now current={W}, previous={} → WasKeyPressed should be true
    EXPECT_TRUE(mgr.WasKeyPressed(KEY_W));

    mgr.Update(); // frame 1 — snapshots {W} into previous, current still {W}
    // Now current={W}, previous={W} → not "just pressed"
    EXPECT_FALSE(mgr.WasKeyPressed(KEY_W));
}

TEST(InputManager_WasKeyReleased)
{
    InputManager mgr;
    mgr.Initialize();

    mgr.SimulateKeyDown(KEY_W);
    mgr.Update(); // previous={}, current={W} → snapshots {W} into previous
    // Now previous={W}, current={W}
    mgr.SimulateKeyUp(KEY_W);
    // Now previous={W}, current={} → WasKeyReleased should be true
    EXPECT_TRUE(mgr.WasKeyReleased(KEY_W));

    mgr.Update(); // previous={}, current={}
    EXPECT_FALSE(mgr.WasKeyReleased(KEY_W));
}

TEST(InputManager_MultipleKeysSimultaneous)
{
    InputManager mgr;
    mgr.Initialize();

    mgr.SimulateKeyDown(KEY_W);
    mgr.SimulateKeyDown(KEY_A);
    mgr.SimulateKeyDown(KEY_SPACE);

    EXPECT_TRUE(mgr.IsKeyDown(KEY_W));
    EXPECT_TRUE(mgr.IsKeyDown(KEY_A));
    EXPECT_TRUE(mgr.IsKeyDown(KEY_SPACE));
    EXPECT_FALSE(mgr.IsKeyDown(KEY_D));
}

// ============================================================================
// Mouse Button Tests
// ============================================================================

TEST(InputManager_MouseButtonDown)
{
    InputManager mgr;
    mgr.Initialize();

    EXPECT_FALSE(mgr.IsMouseButtonDown(MOUSE_LEFT));
    mgr.SimulateMouseButtonDown(MOUSE_LEFT);
    EXPECT_TRUE(mgr.IsMouseButtonDown(MOUSE_LEFT));
}

TEST(InputManager_WasMouseButtonPressed)
{
    InputManager mgr;
    mgr.Initialize();

    mgr.Update(); // snapshot empty
    mgr.SimulateMouseButtonDown(MOUSE_LEFT);
    // current={LEFT}, previous={} → pressed this frame
    EXPECT_TRUE(mgr.WasMouseButtonPressed(MOUSE_LEFT));

    mgr.Update(); // snapshot {LEFT}
    EXPECT_FALSE(mgr.WasMouseButtonPressed(MOUSE_LEFT));
}

TEST(InputManager_WasMouseButtonReleased)
{
    InputManager mgr;
    mgr.Initialize();

    mgr.SimulateMouseButtonDown(MOUSE_RIGHT);
    mgr.Update(); // snapshot into previous: previous={RIGHT}
    mgr.SimulateMouseButtonUp(MOUSE_RIGHT);
    // current={}, previous={RIGHT} → released
    EXPECT_TRUE(mgr.WasMouseButtonReleased(MOUSE_RIGHT));
}

// ============================================================================
// Mouse Delta / Dead Zone Tests
// ============================================================================

TEST(InputManager_MouseDeltaBasic)
{
    InputManager mgr;
    mgr.Initialize();
    mgr.SetMouseDeadZone(0.0f); // no dead zone

    mgr.SimulateMouseMove(10.0f, -5.0f);
    mgr.Update();

    EXPECT_NEAR(mgr.GetMouseDelta().dx, 10.0f, 0.01f);
    EXPECT_NEAR(mgr.GetMouseDelta().dy, -5.0f, 0.01f);
}

TEST(InputManager_MouseDeadZoneFilters)
{
    InputManager mgr;
    mgr.Initialize();
    mgr.SetMouseDeadZone(5.0f);

    // Small movement — under dead zone
    mgr.SimulateMouseMove(1.0f, 1.0f); // magnitude ~1.41
    mgr.Update();

    EXPECT_NEAR(mgr.GetMouseDelta().dx, 0.0f, 0.01f);
    EXPECT_NEAR(mgr.GetMouseDelta().dy, 0.0f, 0.01f);
}

TEST(InputManager_MouseDeadZonePasses)
{
    InputManager mgr;
    mgr.Initialize();
    mgr.SetMouseDeadZone(1.0f);

    // Large movement — above dead zone
    mgr.SimulateMouseMove(10.0f, 10.0f);
    mgr.Update();

    EXPECT_TRUE(std::abs(mgr.GetMouseDelta().dx) > 0.1f);
    EXPECT_TRUE(std::abs(mgr.GetMouseDelta().dy) > 0.1f);
}

TEST(InputManager_MouseSensitivity)
{
    InputManager mgr;
    mgr.Initialize();
    mgr.SetMouseDeadZone(0.0f);
    mgr.SetMouseSensitivity(2.0f);

    mgr.SimulateMouseMove(5.0f, 3.0f);
    mgr.Update();

    EXPECT_NEAR(mgr.GetMouseDelta().dx, 10.0f, 0.01f);
    EXPECT_NEAR(mgr.GetMouseDelta().dy, 6.0f, 0.01f);
}

TEST(InputManager_InvertMouseY)
{
    InputManager mgr;
    mgr.Initialize();
    mgr.SetMouseDeadZone(0.0f);
    mgr.SetInvertMouseY(true);

    mgr.SimulateMouseMove(5.0f, 3.0f);
    mgr.Update();

    EXPECT_NEAR(mgr.GetMouseDelta().dx, 5.0f, 0.01f);
    EXPECT_NEAR(mgr.GetMouseDelta().dy, -3.0f, 0.01f);
}

TEST(InputManager_MouseDeltaResetsBetweenFrames)
{
    InputManager mgr;
    mgr.Initialize();
    mgr.SetMouseDeadZone(0.0f);

    mgr.SimulateMouseMove(10.0f, 5.0f);
    mgr.Update();

    // No movement this frame
    mgr.Update();
    EXPECT_NEAR(mgr.GetMouseDelta().dx, 0.0f, 0.01f);
    EXPECT_NEAR(mgr.GetMouseDelta().dy, 0.0f, 0.01f);
}

// ============================================================================
// Mouse Capture Tests
// ============================================================================

TEST(InputManager_MouseCapture)
{
    InputManager mgr;
    mgr.Initialize();

    EXPECT_FALSE(mgr.IsMouseCaptured());
    mgr.CaptureMouse(true);
    EXPECT_TRUE(mgr.IsMouseCaptured());
    mgr.CaptureMouse(false);
    EXPECT_FALSE(mgr.IsMouseCaptured());
}

// ============================================================================
// Input Metrics Tests
// ============================================================================

TEST(InputManager_MetricsKeyPressCount)
{
    InputManager mgr;
    mgr.Initialize();

    mgr.SimulateKeyDown(KEY_W);
    mgr.SimulateKeyDown(KEY_A);
    mgr.SimulateKeyDown(KEY_W); // duplicate — should not increment

    auto metrics = mgr.GetMetrics();
    EXPECT_EQ(metrics.keyPressCount, 2);
}

TEST(InputManager_MetricsMousePressCount)
{
    InputManager mgr;
    mgr.Initialize();

    mgr.SimulateMouseButtonDown(MOUSE_LEFT);
    mgr.SimulateMouseButtonDown(MOUSE_RIGHT);

    auto metrics = mgr.GetMetrics();
    EXPECT_EQ(metrics.mousePressCount, 2);
}

TEST(InputManager_MetricsActiveKeys)
{
    InputManager mgr;
    mgr.Initialize();

    mgr.SimulateKeyDown(KEY_W);
    mgr.SimulateKeyDown(KEY_A);
    mgr.SimulateKeyDown(KEY_D);

    auto metrics = mgr.GetMetrics();
    EXPECT_EQ(metrics.activeKeys, 3);
}

// ============================================================================
// Clear State Tests
// ============================================================================

TEST(InputManager_ClearInputStates)
{
    InputManager mgr;
    mgr.Initialize();

    mgr.SimulateKeyDown(KEY_W);
    mgr.SimulateMouseButtonDown(MOUSE_LEFT);

    mgr.ClearInputStates();

    EXPECT_FALSE(mgr.IsKeyDown(KEY_W));
    EXPECT_FALSE(mgr.IsMouseButtonDown(MOUSE_LEFT));
}

// ============================================================================
// Settings Tests
// ============================================================================

TEST(InputManager_DefaultSettings)
{
    InputManager mgr;
    mgr.Initialize();

    EXPECT_NEAR(mgr.GetMouseSensitivity(), 1.0f, 0.01f);
    EXPECT_NEAR(mgr.GetMouseDeadZone(), 0.01f, 0.001f);
}

TEST(InputManager_SetSettings)
{
    InputManager mgr;
    mgr.Initialize();

    mgr.SetMouseSensitivity(2.5f);
    mgr.SetMouseDeadZone(0.05f);

    EXPECT_NEAR(mgr.GetMouseSensitivity(), 2.5f, 0.01f);
    EXPECT_NEAR(mgr.GetMouseDeadZone(), 0.05f, 0.001f);
}
