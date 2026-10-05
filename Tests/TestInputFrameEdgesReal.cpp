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

#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace
{
    /// One host frame: the message pump has already delivered this frame's
    /// messages, now the tick runs the input step before the modules.
    void RunFrameInputStep(InputManager& input)
    {
        input.Update();
    }

    std::string ReadWin32InputHostSource()
    {
        const std::string path =
            std::string(SPARK_TEST_SOURCE_DIR) + "/SparkEngine/Source/Core/SparkEngineWindowsWin32.cpp";
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
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

TEST(InputFrameEdgesReal_GenericModifiersAliasEitherSideWithoutLosingSideState)
{
    struct ModifierKeys
    {
        int generic;
        int left;
        int right;
    };

    constexpr std::array<ModifierKeys, 3> modifiers = {
        {{VK_SHIFT, VK_LSHIFT, VK_RSHIFT}, {VK_CONTROL, VK_LCONTROL, VK_RCONTROL}, {VK_MENU, VK_LMENU, VK_RMENU}}};

    for (const auto& keys : modifiers)
    {
        InputManager input;
        RunFrameInputStep(input);

        input.HandleMessage(WM_KEYDOWN, keys.left, 0);
        EXPECT_TRUE(input.IsKeyDown(keys.generic));
        EXPECT_TRUE(input.IsKeyDown(keys.left));
        EXPECT_FALSE(input.IsKeyDown(keys.right));
        RunFrameInputStep(input);
        EXPECT_TRUE(input.IsFrameKeyDown(keys.generic));
        EXPECT_TRUE(input.WasKeyPressed(keys.generic));
        EXPECT_TRUE(input.WasKeyPressed(keys.left));

        input.HandleMessage(WM_KEYDOWN, keys.right, 0);
        RunFrameInputStep(input);
        EXPECT_TRUE(input.IsKeyDown(keys.generic));
        EXPECT_FALSE(input.WasKeyPressed(keys.generic));
        EXPECT_TRUE(input.WasKeyPressed(keys.right));

        input.HandleMessage(WM_KEYUP, keys.left, 0);
        EXPECT_TRUE(input.IsKeyDown(keys.generic));
        RunFrameInputStep(input);
        EXPECT_TRUE(input.IsFrameKeyDown(keys.generic));
        EXPECT_FALSE(input.WasKeyReleased(keys.generic));
        EXPECT_TRUE(input.WasKeyReleased(keys.left));
        EXPECT_TRUE(input.IsKeyDown(keys.right));

        input.HandleMessage(WM_KEYUP, keys.right, 0);
        EXPECT_FALSE(input.IsKeyDown(keys.generic));
        RunFrameInputStep(input);
        EXPECT_FALSE(input.IsFrameKeyDown(keys.generic));
        EXPECT_TRUE(input.WasKeyReleased(keys.generic));
        EXPECT_TRUE(input.WasKeyReleased(keys.right));
    }
}

TEST(InputFrameEdgesReal_Win32HostNormalizesModifierSidesAndSystemKeys)
{
    const std::string source = ReadWin32InputHostSource();
    ASSERT_FALSE(source.empty());

    EXPECT_TRUE(source.find("case WM_SYSKEYDOWN:") != std::string::npos);
    EXPECT_TRUE(source.find("case WM_SYSKEYUP:") != std::string::npos);
    EXPECT_TRUE(source.find("MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX)") != std::string::npos);
    EXPECT_TRUE(source.find("(static_cast<ULONG_PTR>(lParam) & 0x01000000u) != 0") != std::string::npos);
    EXPECT_TRUE(source.find("scanCode |= 0xE000u;") != std::string::npos);
    EXPECT_TRUE(source.find("mappedKey == VK_LSHIFT || mappedKey == VK_RSHIFT") != std::string::npos);
    EXPECT_TRUE(source.find("mappedKey == VK_LCONTROL || mappedKey == VK_RCONTROL") != std::string::npos);
    EXPECT_TRUE(source.find("mappedKey == VK_LMENU || mappedKey == VK_RMENU") != std::string::npos);
    EXPECT_TRUE(source.find("(msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) ? WM_KEYDOWN : WM_KEYUP") !=
                std::string::npos);
    EXPECT_TRUE(source.find("NormalizeWin32ModifierVirtualKey(wParam, lParam)") != std::string::npos);
}
