/**
 * @file TestSparkGameFPSMirrorCompanionsReal.cpp
 * @brief Production-source companions for the stable-v1 single-player mirrors.
 *
 * TestEngineContext.cpp says in its own header that it is a standalone
 * reimplementation, so it cannot detect a regression in the shipped code it
 * claims to cover. This file includes the real headers and exercises the
 * shipped classes:
 *
 *   InputManager   (SparkEngine/Source/Input/InputManager.cpp) - sensitivity and
 *                  dead-zone validation, key bindings, state clearing, and the
 *                  windowless message path: HandleMessage + Update drive key and
 *                  button state, press/active metrics and per-frame mouse deltas
 *   EngineContext  (SparkEngine/Source/Core/EngineContext.cpp) - the TypeId
 *                  service locator that TestEngineContext.cpp reimplements
 *
 * Both .cpp files are already part of SparkEngineLib, so no additional
 * production source has to be added to the SparkTests target. RDY-010 retired
 * the TestInputManagerState.cpp mirror; its edge-detection cases live in
 * TestInputFrameEdgesReal.cpp.
 *
 * Deliberately NOT exercised here: InputManager::Initialize/CaptureMouse,
 * WM_LBUTTONDOWN (which captures the mouse) and the global EngineContext
 * singleton. Capture needs a real window, and CaptureMouse(true) hides the
 * desktop cursor for the whole session.
 */

#include "TestFramework.h"

#include "Core/EngineContext.h"
#include "Input/InputManager.h"

#include <string>

// ============================================================================
// InputManager — real class, no window required
// ============================================================================

TEST(InputManagerReal_FreshInstanceHasShippedDefaults)
{
    InputManager input;
    const InputManager::InputMetrics metrics = input.Console_GetMetrics();
    EXPECT_NEAR(metrics.mouseSensitivity, 1.0f, 0.0001f);
    EXPECT_NEAR(metrics.mouseDeadZone, 0.0f, 0.0001f);
    EXPECT_FALSE(metrics.mouseAcceleration);
    EXPECT_FALSE(metrics.invertMouseY);
    EXPECT_FALSE(metrics.mouseCaptured);
    EXPECT_FALSE(input.IsMouseCaptured());
    EXPECT_EQ(metrics.totalKeyBindings, static_cast<size_t>(0));
}

TEST(InputManagerReal_SensitivityAcceptsTheDocumentedRange)
{
    InputManager input;
    input.Console_SetMouseSensitivity(2.5f);
    EXPECT_NEAR(input.Console_GetMetrics().mouseSensitivity, 2.5f, 0.0001f);

    input.Console_SetMouseSensitivity(0.1f);
    EXPECT_NEAR(input.Console_GetMetrics().mouseSensitivity, 0.1f, 0.0001f);

    input.Console_SetMouseSensitivity(10.0f);
    EXPECT_NEAR(input.Console_GetMetrics().mouseSensitivity, 10.0f, 0.0001f);
}

TEST(InputManagerReal_SensitivityRejectsOutOfRangeInsteadOfClamping)
{
    InputManager input;
    input.Console_SetMouseSensitivity(3.0f);

    input.Console_SetMouseSensitivity(0.0f);
    // Out of range is rejected, so the previous value must survive intact.
    EXPECT_NEAR(input.Console_GetMetrics().mouseSensitivity, 3.0f, 0.0001f);

    input.Console_SetMouseSensitivity(11.0f);
    EXPECT_NEAR(input.Console_GetMetrics().mouseSensitivity, 3.0f, 0.0001f);

    input.Console_SetMouseSensitivity(-1.0f);
    EXPECT_NEAR(input.Console_GetMetrics().mouseSensitivity, 3.0f, 0.0001f);
}

TEST(InputManagerReal_DeadZoneAcceptsZeroAndRejectsNegative)
{
    InputManager input;
    input.Console_SetMouseDeadZone(2.0f);
    EXPECT_NEAR(input.Console_GetMetrics().mouseDeadZone, 2.0f, 0.0001f);

    // 0.0 is inside the documented range and must be accepted.
    input.Console_SetMouseDeadZone(0.0f);
    EXPECT_NEAR(input.Console_GetMetrics().mouseDeadZone, 0.0f, 0.0001f);

    input.Console_SetMouseDeadZone(-0.5f);
    EXPECT_NEAR(input.Console_GetMetrics().mouseDeadZone, 0.0f, 0.0001f);

    input.Console_SetMouseDeadZone(10.5f);
    EXPECT_NEAR(input.Console_GetMetrics().mouseDeadZone, 0.0f, 0.0001f);
}

TEST(InputManagerReal_BindKeyRejectsAnUnknownKeyName)
{
    InputManager input;
    EXPECT_FALSE(input.Console_BindKey("fire", "NotAKeyName"));
    EXPECT_EQ(input.Console_GetMetrics().totalKeyBindings, static_cast<size_t>(0));
}

TEST(InputManagerReal_BindKeyRegistersAnAction)
{
    InputManager input;
    ASSERT_TRUE(input.Console_BindKey("jump", "SPACE"));
    EXPECT_EQ(input.Console_GetMetrics().totalKeyBindings, static_cast<size_t>(1));
    // Nothing is pressed, so the bound action must be inactive.
    EXPECT_FALSE(input.Console_IsActionActive("jump"));
    // An action that was never bound is inactive rather than an error.
    EXPECT_FALSE(input.Console_IsActionActive("never_bound"));
}

TEST(InputManagerReal_UnbindKeyRemovesTheAction)
{
    InputManager input;
    ASSERT_TRUE(input.Console_BindKey("crouch", "C"));
    EXPECT_EQ(input.Console_GetMetrics().totalKeyBindings, static_cast<size_t>(1));

    input.Console_UnbindKey("crouch");
    EXPECT_EQ(input.Console_GetMetrics().totalKeyBindings, static_cast<size_t>(0));
    EXPECT_FALSE(input.Console_IsActionActive("crouch"));
}

TEST(InputManagerReal_ResetToDefaultsRestoresSettingsAndDropsBindings)
{
    InputManager input;
    input.Console_SetMouseSensitivity(4.0f);
    input.Console_SetMouseDeadZone(3.0f);
    input.Console_SetInvertMouseY(true);
    ASSERT_TRUE(input.Console_BindKey("reload", "R"));

    input.Console_ResetToDefaults();

    const InputManager::InputMetrics metrics = input.Console_GetMetrics();
    EXPECT_NEAR(metrics.mouseSensitivity, 1.0f, 0.0001f);
    EXPECT_NEAR(metrics.mouseDeadZone, 0.0f, 0.0001f);
    EXPECT_FALSE(metrics.invertMouseY);
    EXPECT_EQ(metrics.totalKeyBindings, static_cast<size_t>(0));
}

TEST(InputManagerReal_SettingsRoundTripThroughApply)
{
    InputManager source;
    source.Console_SetMouseSensitivity(2.0f);
    source.Console_SetMouseDeadZone(1.5f);
    source.Console_SetInvertMouseY(true);
    const InputManager::InputSettings settings = source.Console_GetSettings();

    InputManager target;
    target.Console_ApplySettings(settings);
    const InputManager::InputMetrics metrics = target.Console_GetMetrics();
    EXPECT_NEAR(metrics.mouseSensitivity, 2.0f, 0.0001f);
    EXPECT_NEAR(metrics.mouseDeadZone, 1.5f, 0.0001f);
    EXPECT_TRUE(metrics.invertMouseY);
}

namespace
{
    /// WM_MOUSEMOVE packs client x/y into the low/high words of lParam.
    LPARAM MouseMoveLParam(int x, int y)
    {
        return static_cast<LPARAM>((static_cast<unsigned>(y) << 16) | (static_cast<unsigned>(x) & 0xFFFFu));
    }

    /// Deliver a mouse move, then run the frame's input step that turns it into a delta.
    MousePoint MoveMouseAndTick(InputManager& input, int x, int y)
    {
        input.HandleMessage(WM_MOUSEMOVE, 0, MouseMoveLParam(x, y));
        input.Update();
        return input.GetMouseDelta();
    }
} // namespace

TEST(InputManagerReal_ClearInputStatesLeavesNoKeyDown)
{
    InputManager input;
    input.HandleMessage(WM_KEYDOWN, 'A', 0);
    input.HandleMessage(WM_RBUTTONDOWN, 0, 0);
    input.Update();
    ASSERT_TRUE(input.IsKeyDown('A'));
    ASSERT_TRUE(input.IsMouseButtonDown(1));

    input.Console_ClearInputStates();
    EXPECT_FALSE(input.IsKeyDown('A'));
    EXPECT_TRUE(input.IsKeyUp('A'));
    EXPECT_FALSE(input.WasKeyPressed('A'));
    EXPECT_FALSE(input.IsMouseButtonDown(1));
    EXPECT_FALSE(input.WasMouseButtonPressed(1));
    EXPECT_EQ(input.Console_GetMetrics().activeKeys, static_cast<size_t>(0));
}

TEST(InputManagerReal_KeyMessagesTrackSimultaneousKeysAndMetrics)
{
    InputManager input;
    input.HandleMessage(WM_KEYDOWN, 'W', 0);
    input.HandleMessage(WM_KEYDOWN, 'A', 0);
    input.HandleMessage(WM_KEYDOWN, VK_SPACE, 0);

    EXPECT_TRUE(input.IsKeyDown('W'));
    EXPECT_TRUE(input.IsKeyDown('A'));
    EXPECT_TRUE(input.IsKeyDown(VK_SPACE));
    EXPECT_FALSE(input.IsKeyDown('D'));
    EXPECT_TRUE(input.IsKeyUp('D'));
    InputManager::InputMetrics metrics = input.Console_GetMetrics();
    EXPECT_EQ(metrics.activeKeys, static_cast<size_t>(3));
    EXPECT_EQ(metrics.keyPressCount, static_cast<size_t>(3));

    input.HandleMessage(WM_KEYUP, 'A', 0);
    EXPECT_TRUE(input.IsKeyUp('A'));
    EXPECT_TRUE(input.IsKeyDown('W'));
    metrics = input.Console_GetMetrics();
    EXPECT_EQ(metrics.activeKeys, static_cast<size_t>(2));
    EXPECT_EQ(metrics.keyPressCount, static_cast<size_t>(3)); // a release is not a press
}

TEST(InputManagerReal_MouseButtonMessagesTrackRightAndMiddle)
{
    InputManager input;
    input.HandleMessage(WM_RBUTTONDOWN, 0, 0);
    input.HandleMessage(WM_MBUTTONDOWN, 0, 0);
    EXPECT_FALSE(input.IsMouseButtonDown(0));
    EXPECT_TRUE(input.IsMouseButtonDown(1));
    EXPECT_TRUE(input.IsMouseButtonDown(2));
    EXPECT_EQ(input.Console_GetMetrics().activeMouseButtons, static_cast<size_t>(2));

    input.HandleMessage(WM_RBUTTONUP, 0, 0);
    input.HandleMessage(WM_MBUTTONUP, 0, 0);
    EXPECT_FALSE(input.IsMouseButtonDown(1));
    EXPECT_FALSE(input.IsMouseButtonDown(2));
    EXPECT_EQ(input.Console_GetMetrics().activeMouseButtons, static_cast<size_t>(0));
}

TEST(InputManagerReal_MouseDeltaIsMeasuredPerFrame)
{
    InputManager input;
    MoveMouseAndTick(input, 100, 50); // seeds the position from the construction origin
    EXPECT_EQ(input.GetMousePosition().x, 100);
    EXPECT_EQ(input.GetMousePosition().y, 50);

    const MousePoint moved = MoveMouseAndTick(input, 110, 45);
    EXPECT_EQ(moved.x, 10);
    EXPECT_EQ(moved.y, -5);

    // No movement delivered this frame: the delta must not repeat the last one.
    input.Update();
    EXPECT_EQ(input.GetMouseDelta().x, 0);
    EXPECT_EQ(input.GetMouseDelta().y, 0);
}

#ifdef _WIN32
// The Windows Update() runs the uncaptured delta through ProcessMouseDelta and
// the Windows HandleMessage counts button presses. The POSIX Update() reports
// the raw delta and its HandleMessage does not count button presses.
TEST(InputManagerReal_MouseDeltaAppliesSensitivityInvertAndDeadZone)
{
    InputManager input;
    MoveMouseAndTick(input, 100, 100);

    input.Console_SetMouseSensitivity(2.0f);
    MousePoint delta = MoveMouseAndTick(input, 105, 103);
    EXPECT_EQ(delta.x, 10);
    EXPECT_EQ(delta.y, 6);

    input.Console_SetMouseSensitivity(1.0f);
    input.Console_SetInvertMouseY(true);
    delta = MoveMouseAndTick(input, 110, 106);
    EXPECT_EQ(delta.x, 5);
    EXPECT_EQ(delta.y, -3);

    input.Console_SetMouseDeadZone(5.0f);
    delta = MoveMouseAndTick(input, 111, 107); // |(1, 1)| is inside the dead zone
    EXPECT_EQ(delta.x, 0);
    EXPECT_EQ(delta.y, 0);
    delta = MoveMouseAndTick(input, 121, 117); // |(10, 10)| is outside it
    EXPECT_EQ(delta.x, 10);
    EXPECT_EQ(delta.y, -10);
}

TEST(InputManagerReal_MousePressCountCountsButtonDowns)
{
    InputManager input;
    input.HandleMessage(WM_RBUTTONDOWN, 0, 0);
    input.HandleMessage(WM_RBUTTONUP, 0, 0);
    input.HandleMessage(WM_MBUTTONDOWN, 0, 0);
    EXPECT_EQ(input.Console_GetMetrics().mousePressCount, static_cast<size_t>(2));
}
#endif

// ============================================================================
// EngineContext — the real TypeId service locator
// ============================================================================

namespace
{
    // Types local to this file: registering them can never collide with a real
    // engine subsystem, and they are not lifecycle-managed, so the registry
    // accepts them while the context is idle.
    struct CompanionServiceA
    {
        int value = 7;
    };

    struct CompanionServiceB
    {
        int value = 11;
    };
} // namespace

TEST(EngineContextReal_TypeIdIsStablePerTypeAndDistinctAcrossTypes)
{
    EXPECT_TRUE(GetTypeId<CompanionServiceA>() == GetTypeId<CompanionServiceA>());
    EXPECT_TRUE(GetTypeId<CompanionServiceA>() != GetTypeId<CompanionServiceB>());
    EXPECT_TRUE(GetTypeId<CompanionServiceA>() != nullptr);
}

TEST(EngineContextReal_UnregisteredLookupReturnsNull)
{
    EngineContext context;
    EXPECT_TRUE(context.GetSystem<CompanionServiceA>() == nullptr);
}

TEST(EngineContextReal_RegisteredSystemIsReturnedByType)
{
    EngineContext context;
    CompanionServiceA serviceA;
    CompanionServiceB serviceB;

    ASSERT_TRUE(context.RegisterSystem<CompanionServiceA>(&serviceA));
    ASSERT_TRUE(context.RegisterSystem<CompanionServiceB>(&serviceB));

    CompanionServiceA* const fetchedA = context.GetSystem<CompanionServiceA>();
    CompanionServiceB* const fetchedB = context.GetSystem<CompanionServiceB>();
    ASSERT_TRUE(fetchedA != nullptr);
    ASSERT_TRUE(fetchedB != nullptr);

    // The locator must be type-keyed, not order-keyed: each type resolves to
    // the pointer registered for it, never to the other one.
    EXPECT_TRUE(fetchedA == &serviceA);
    EXPECT_TRUE(fetchedB == &serviceB);
    EXPECT_EQ(fetchedA->value, 7);
    EXPECT_EQ(fetchedB->value, 11);
}

TEST(EngineContextReal_RegistriesAreIndependentPerContext)
{
    EngineContext first;
    EngineContext second;
    CompanionServiceA serviceA;

    ASSERT_TRUE(first.RegisterSystem<CompanionServiceA>(&serviceA));
    EXPECT_TRUE(first.GetSystem<CompanionServiceA>() == &serviceA);
    EXPECT_TRUE(second.GetSystem<CompanionServiceA>() == nullptr);
}
