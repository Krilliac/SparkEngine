/**
 * @file Test_engine-misc_MobileGestures.cpp
 * @brief Regression tests for the mobile gesture pipeline (MobilePlatform.cpp).
 *
 * Previously RecognizeGestures() built a Gesture but never pushed it, and Update
 * cleared m_pendingGestures immediately, so GetGestures() was always empty and
 * OnGesture callbacks never fired. These tests exercise the wired path: a clean
 * touch-end produces a Tap gesture, delivered both via GetGestures() and the
 * registered callback; a cancelled touch produces nothing.
 */

#include "../TestFramework.h"
#include "Engine/Mobile/MobilePlatform.h"

namespace
{
    Spark::Mobile::TouchEvent MakeTouch(uint32_t id, Spark::Mobile::TouchEventType type, float x, float y)
    {
        Spark::Mobile::TouchEvent e;
        e.touchId = id;
        e.type = type;
        e.x = x;
        e.y = y;
        return e;
    }
} // namespace

TEST(MobilePlatform_TapRecognizedAndCallbackFires)
{
    Spark::Mobile::MobilePlatform platform;
    platform.Initialize();

    bool callbackFired = false;
    Spark::Mobile::Gesture received;
    platform.OnGesture(Spark::Mobile::GestureType::Tap,
                       [&](const Spark::Mobile::Gesture& g)
                       {
                           callbackFired = true;
                           received = g;
                       });

    platform.ProcessTouchEvent(MakeTouch(1, Spark::Mobile::TouchEventType::Began, 0.3f, 0.4f));
    platform.ProcessTouchEvent(MakeTouch(1, Spark::Mobile::TouchEventType::Ended, 0.3f, 0.4f));

    platform.Update(0.016f);

    auto gestures = platform.GetGestures();
    ASSERT_EQ(gestures.size(), static_cast<size_t>(1));
    EXPECT_TRUE(gestures[0].type == Spark::Mobile::GestureType::Tap);
    EXPECT_NEAR(gestures[0].x, 0.3f, 0.001f);
    EXPECT_NEAR(gestures[0].y, 0.4f, 0.001f);

    EXPECT_TRUE(callbackFired);
    EXPECT_TRUE(received.type == Spark::Mobile::GestureType::Tap);
}

TEST(MobilePlatform_CancelledTouchProducesNoGesture)
{
    Spark::Mobile::MobilePlatform platform;
    platform.Initialize();

    platform.ProcessTouchEvent(MakeTouch(2, Spark::Mobile::TouchEventType::Began, 0.1f, 0.1f));
    platform.ProcessTouchEvent(MakeTouch(2, Spark::Mobile::TouchEventType::Cancelled, 0.1f, 0.1f));

    platform.Update(0.016f);

    EXPECT_EQ(platform.GetGestures().size(), static_cast<size_t>(0));
}

TEST(MobilePlatform_GesturesClearedNextFrame)
{
    Spark::Mobile::MobilePlatform platform;
    platform.Initialize();

    platform.ProcessTouchEvent(MakeTouch(3, Spark::Mobile::TouchEventType::Began, 0.5f, 0.5f));
    platform.ProcessTouchEvent(MakeTouch(3, Spark::Mobile::TouchEventType::Ended, 0.5f, 0.5f));
    platform.Update(0.016f);
    EXPECT_EQ(platform.GetGestures().size(), static_cast<size_t>(1));

    // No new touches this frame: the previous gesture is consumed.
    platform.Update(0.016f);
    EXPECT_EQ(platform.GetGestures().size(), static_cast<size_t>(0));
}
