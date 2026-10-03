/**
 * @file TestInputFrameEdgesReal.cpp
 * @brief Regression tests for InputManager edge detection under the real host
 *        frame ordering.
 *
 * Every host (Win32 PeekMessage loop, Linux SDL2 event pump) delivers a frame's
 * input messages FIRST and then runs the tick, which calls InputManager::Update()
 * before ModuleManager::UpdateAll(). Update() used to snapshot the previous state
 * from the live state at that point, so a key pressed this frame was already in
 * the "previous" snapshot when modules ran: every WasKeyPressed / WasKeyReleased /
 * WasMouseButtonPressed query from a module (including AngelScript getKeyDown)
 * returned false in a real host.
 *
 * These tests drive the production InputManager in exactly that order:
 * HandleMessage(...) then Update() then the queries a module would make. No window
 * is created: a windowless InputManager still advances key/button edges, and the
 * left mouse button is avoided because it captures (and hides) the desktop cursor.
 */

#include "TestFramework.h"

#include "Input/InputManager.h"

#include <chrono>
#include <thread>

namespace
{
    /// One host frame: the message pump has already delivered this frame's
    /// messages, now the tick runs the input step before the modules.
    void RunFrameInputStep(InputManager& input)
    {
        input.Update();
    }
} // namespace

TEST(InputFrameEdgesReal_KeyDownDeliveredBeforeTickIsPressedForExactlyOneFrame)
{
    InputManager input;
    RunFrameInputStep(input); // an idle frame before the key is touched

    // Frame 1: the pump delivers WM_KEYDOWN, then the tick runs.
    input.HandleMessage(WM_KEYDOWN, VK_SPACE, 0);
    RunFrameInputStep(input);
    EXPECT_TRUE(input.IsKeyDown(VK_SPACE));
    EXPECT_TRUE(input.WasKeyPressed(VK_SPACE));
    EXPECT_TRUE(input.WasKeyPressed(VK_SPACE)); // stable across queries within a frame
    EXPECT_FALSE(input.WasKeyReleased(VK_SPACE));

    // Frame 2: key still held, no new messages -> no second press edge.
    RunFrameInputStep(input);
    EXPECT_TRUE(input.IsKeyDown(VK_SPACE));
    EXPECT_FALSE(input.WasKeyPressed(VK_SPACE));

    // Frame 3: the pump delivers WM_KEYUP.
    input.HandleMessage(WM_KEYUP, VK_SPACE, 0);
    RunFrameInputStep(input);
    EXPECT_FALSE(input.IsKeyDown(VK_SPACE));
    EXPECT_TRUE(input.WasKeyReleased(VK_SPACE));
    EXPECT_FALSE(input.WasKeyPressed(VK_SPACE));

    // Frame 4: the release edge lasts one frame too.
    RunFrameInputStep(input);
    EXPECT_FALSE(input.WasKeyReleased(VK_SPACE));
}

TEST(InputFrameEdgesReal_PressOnTheVeryFirstFrameIsAnEdge)
{
    InputManager input;
    input.HandleMessage(WM_KEYDOWN, 'W', 0);
    RunFrameInputStep(input);
    EXPECT_TRUE(input.WasKeyPressed('W'));
    RunFrameInputStep(input);
    EXPECT_FALSE(input.WasKeyPressed('W'));
}

TEST(InputFrameEdgesReal_MouseButtonDeliveredBeforeTickIsPressedForExactlyOneFrame)
{
    InputManager input;
    RunFrameInputStep(input);

    input.HandleMessage(WM_RBUTTONDOWN, 0, 0);
    RunFrameInputStep(input);
    EXPECT_TRUE(input.IsMouseButtonDown(1));
    EXPECT_TRUE(input.WasMouseButtonPressed(1));

    RunFrameInputStep(input);
    EXPECT_FALSE(input.WasMouseButtonPressed(1));

    input.HandleMessage(WM_RBUTTONUP, 0, 0);
    RunFrameInputStep(input);
    EXPECT_TRUE(input.WasMouseButtonReleased(1));

    RunFrameInputStep(input);
    EXPECT_FALSE(input.WasMouseButtonReleased(1));
}

TEST(InputFrameEdgesReal_ChangeAfterTheInputStepIsAnEdgeOnTheNextFrameOnly)
{
    // Console commands and scripted commands run after the modules. A press they
    // inject must neither vanish nor appear retroactively in the current frame.
    InputManager input;
    RunFrameInputStep(input);
    input.Console_SimulateKeyPress("E", 600000);
    EXPECT_TRUE(input.IsKeyDown('E'));
    EXPECT_FALSE(input.WasKeyPressed('E')); // this frame's edges are already latched

    RunFrameInputStep(input);
    EXPECT_TRUE(input.WasKeyPressed('E'));
    RunFrameInputStep(input);
    EXPECT_FALSE(input.WasKeyPressed('E'));

    input.Console_ClearInputStates();
    EXPECT_FALSE(input.IsKeyDown('E'));
    EXPECT_FALSE(input.WasKeyPressed('E'));
    EXPECT_EQ(input.GetPendingTimedKeyReleaseCount(), 0u);
}

TEST(InputFrameEdgesReal_TimedReleaseIsAReleaseEdgeOnTheFrameThatAppliesIt)
{
    InputManager input;
    input.Console_SimulateKeyPress("SPACE", 250);
    RunFrameInputStep(input);
    // The deadline is 250 ms away; if this first step was that slow, the press
    // was released before any frame observed it and the rest cannot be checked.
    if (!input.IsKeyDown(VK_SPACE))
        SKIP_TEST("First input step ran after the 250 ms release deadline");
    EXPECT_TRUE(input.WasKeyPressed(VK_SPACE));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    RunFrameInputStep(input);
    EXPECT_FALSE(input.IsKeyDown(VK_SPACE));
    EXPECT_TRUE(input.WasKeyReleased(VK_SPACE));
    EXPECT_EQ(input.GetPendingTimedKeyReleaseCount(), 0u);

    RunFrameInputStep(input);
    EXPECT_FALSE(input.WasKeyReleased(VK_SPACE));
}

TEST(InputFrameEdgesReal_ObservationIsLatchedAndSequenceAdvancesOnlyOnUpdate)
{
    InputManager input;
    const auto initial = input.GetInputFrameSequence();
    input.HandleMessage(WM_KEYDOWN, VK_F2, 0);
    EXPECT_FALSE(input.IsFrameKeyDown(VK_F2));
    EXPECT_EQ(input.GetInputFrameSequence(), initial);
    input.Update();
    EXPECT_EQ(input.GetInputFrameSequence(), initial + 1);
    EXPECT_TRUE(input.IsFrameKeyDown(VK_F2));
    input.HandleMessage(WM_KEYUP, VK_F2, 0);
    EXPECT_TRUE(input.IsFrameKeyDown(VK_F2));
    EXPECT_EQ(input.GetInputFrameSequence(), initial + 1);
    input.Update();
    EXPECT_FALSE(input.IsFrameKeyDown(VK_F2));
    EXPECT_TRUE(input.WasKeyReleased(VK_F2));
    EXPECT_EQ(input.GetInputFrameSequence(), initial + 2);
}
