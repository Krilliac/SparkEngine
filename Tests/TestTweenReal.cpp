/**
 * @file TestTweenReal.cpp
 * @brief Real-class tests for Spark::Tween + EaseEvaluate
 *
 * Phase JJ deep-wire.
 */

#include "TestFramework.h"
#include "Utils/Tween.h"

TEST(TweenReal_EaseLinearAtEndpoints)
{
    EXPECT_NEAR(Spark::EaseEvaluate(Spark::Ease::Linear, 0.0f), 0.0f, 0.001f);
    EXPECT_NEAR(Spark::EaseEvaluate(Spark::Ease::Linear, 1.0f), 1.0f, 0.001f);
    EXPECT_NEAR(Spark::EaseEvaluate(Spark::Ease::Linear, 0.5f), 0.5f, 0.001f);
}

TEST(TweenReal_EaseInQuad)
{
    EXPECT_NEAR(Spark::EaseEvaluate(Spark::Ease::InQuad, 0.0f), 0.0f, 0.001f);
    EXPECT_NEAR(Spark::EaseEvaluate(Spark::Ease::InQuad, 1.0f), 1.0f, 0.001f);
    EXPECT_NEAR(Spark::EaseEvaluate(Spark::Ease::InQuad, 0.5f), 0.25f, 0.001f);
}

TEST(TweenReal_EaseInCubic)
{
    EXPECT_NEAR(Spark::EaseEvaluate(Spark::Ease::InCubic, 0.0f), 0.0f, 0.001f);
    EXPECT_NEAR(Spark::EaseEvaluate(Spark::Ease::InCubic, 1.0f), 1.0f, 0.001f);
    EXPECT_NEAR(Spark::EaseEvaluate(Spark::Ease::InCubic, 0.5f), 0.125f, 0.001f);
}

TEST(TweenReal_ConstructionAndStartState)
{
    float target = 0.0f;
    Spark::Tween t(&target, 100.0f, 2.0f, Spark::Ease::Linear);
    EXPECT_FALSE(t.IsFinished());
}

TEST(TweenReal_UpdateAdvancesToward)
{
    float target = 0.0f;
    Spark::Tween t(&target, 100.0f, 2.0f, Spark::Ease::Linear);
    // Advance 1 second = halfway.
    t.Update(1.0f);
    EXPECT_NEAR(target, 50.0f, 1.0f);
}

TEST(TweenReal_UpdateCompletes)
{
    float target = 0.0f;
    Spark::Tween t(&target, 100.0f, 1.0f, Spark::Ease::Linear);
    t.Update(1.5f); // Over-advance.
    EXPECT_TRUE(t.IsFinished());
    EXPECT_NEAR(target, 100.0f, 1.0f);
}

TEST(TweenReal_Kill)
{
    float target = 0.0f;
    Spark::Tween t(&target, 100.0f, 2.0f);
    EXPECT_FALSE(t.IsFinished());
    t.Kill();
    EXPECT_TRUE(t.IsFinished());
}

TEST(TweenReal_SetDelayChainable)
{
    // SetDelay returns a reference for chaining; we just verify the
    // call doesn't crash. Note: the delay field is tracked via
    // `m_delay` but `m_delayRemaining` init happens on first update
    // internally — exact timing semantics are implementation-defined.
    float target = 0.0f;
    Spark::Tween t(&target, 100.0f, 1.0f);
    auto& ref = t.SetDelay(2.0f);
    EXPECT_TRUE(&ref == &t);
}
