/**
 * @file TestSparkGameFPSMirrorCompanionsReal.cpp
 * @brief Production-source companions for the stable-v1 single-player mirrors.
 *
 * TestInputManagerState.cpp and TestEngineContext.cpp both say in their own
 * headers that they are standalone reimplementations, so neither can detect a
 * regression in the shipped code they claim to cover. This file includes the
 * real headers and exercises the shipped classes:
 *
 *   InputManager   (SparkEngine/Source/Input/InputManager.cpp) - sensitivity and
 *                  dead-zone validation, key bindings, state clearing
 *   EngineContext  (SparkEngine/Source/Core/EngineContext.cpp) - the TypeId
 *                  service locator that TestEngineContext.cpp reimplements
 *
 * Both .cpp files are already part of SparkEngineLib, so no additional
 * production source has to be added to the SparkTests target.
 *
 * Deliberately NOT exercised here: InputManager::Initialize/Update/CaptureMouse
 * and the global EngineContext singleton. Those need a real window, and
 * CaptureMouse(true) hides the desktop cursor for the whole session.
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

TEST(InputManagerReal_ClearInputStatesLeavesNoKeyDown)
{
    InputManager input;
    input.Console_ClearInputStates();
    // Key codes here are ordinary VK values; no key can be down after a clear.
    EXPECT_FALSE(input.IsKeyDown(65));
    EXPECT_TRUE(input.IsKeyUp(65));
    EXPECT_FALSE(input.WasKeyPressed(65));
    EXPECT_FALSE(input.IsMouseButtonDown(0));
}

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
