/**
 * @file Test_ui-2d_tween.cpp
 * @brief Hardening tests for TweenSystem: ApplyEase coverage + reentrant-callback safety.
 *
 * Covers two audit findings:
 *  - ApplyEase easing functions had zero unit coverage.
 *  - TweenSystem::Update() invalidated its map iterator when a completion
 *    callback reentrantly created or cancelled tweens.
 */

#include "../TestFramework.h"
#include "../../SparkEngine/Source/Engine/Tween/TweenSystem.h"

// ============================================================================
// ApplyEase — pure easing function coverage
// ============================================================================

TEST(ApplyEase_LinearIsIdentity)
{
    EXPECT_NEAR(Spark::ApplyEase(Spark::EaseType::Linear, 0.0f), 0.0f, 0.0001f);
    EXPECT_NEAR(Spark::ApplyEase(Spark::EaseType::Linear, 0.25f), 0.25f, 0.0001f);
    EXPECT_NEAR(Spark::ApplyEase(Spark::EaseType::Linear, 0.5f), 0.5f, 0.0001f);
    EXPECT_NEAR(Spark::ApplyEase(Spark::EaseType::Linear, 0.75f), 0.75f, 0.0001f);
    EXPECT_NEAR(Spark::ApplyEase(Spark::EaseType::Linear, 1.0f), 1.0f, 0.0001f);
}

TEST(ApplyEase_NonElasticEndpoints)
{
    // Every non-elastic ease must map t=0 -> 0 and t=1 -> 1.
    const Spark::EaseType eases[] = {
        Spark::EaseType::Linear,       Spark::EaseType::EaseInQuad,  Spark::EaseType::EaseOutQuad,
        Spark::EaseType::EaseInOutQuad, Spark::EaseType::EaseInCubic, Spark::EaseType::EaseOutCubic,
        Spark::EaseType::EaseInOutCubic, Spark::EaseType::EaseOutBounce};

    for (Spark::EaseType ease : eases)
    {
        EXPECT_NEAR(Spark::ApplyEase(ease, 0.0f), 0.0f, 0.001f);
        EXPECT_NEAR(Spark::ApplyEase(ease, 1.0f), 1.0f, 0.001f);
    }
}

TEST(ApplyEase_KnownMidpoints)
{
    EXPECT_NEAR(Spark::ApplyEase(Spark::EaseType::EaseInQuad, 0.5f), 0.25f, 0.0001f);
    EXPECT_NEAR(Spark::ApplyEase(Spark::EaseType::EaseOutQuad, 0.5f), 0.75f, 0.0001f);
    EXPECT_NEAR(Spark::ApplyEase(Spark::EaseType::EaseInCubic, 0.5f), 0.125f, 0.0001f);
}

TEST(ApplyEase_MonotonicAndBounded)
{
    const Spark::EaseType eases[] = {Spark::EaseType::EaseInQuad, Spark::EaseType::EaseOutQuad,
                                     Spark::EaseType::EaseInCubic, Spark::EaseType::EaseOutCubic};
    const float samples[] = {0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f};

    for (Spark::EaseType ease : eases)
    {
        float previous = -0.001f;
        for (float t : samples)
        {
            float value = Spark::ApplyEase(ease, t);
            EXPECT_GE(value, -0.001f);
            EXPECT_LE(value, 1.001f);
            // Non-decreasing across the sweep (tiny epsilon for float noise).
            EXPECT_GE(value, previous - 0.0001f);
            previous = value;
        }
    }
}

TEST(ApplyEase_ElasticClampedAtEndpoints)
{
    EXPECT_NEAR(Spark::ApplyEase(Spark::EaseType::EaseInElastic, 0.0f), 0.0f, 0.0001f);
    EXPECT_NEAR(Spark::ApplyEase(Spark::EaseType::EaseInElastic, 1.0f), 1.0f, 0.0001f);
    EXPECT_NEAR(Spark::ApplyEase(Spark::EaseType::EaseOutElastic, 0.0f), 0.0f, 0.0001f);
    EXPECT_NEAR(Spark::ApplyEase(Spark::EaseType::EaseOutElastic, 1.0f), 1.0f, 0.0001f);
}

// ============================================================================
// TweenSystem::Update — reentrant completion-callback safety (iterator invalidation)
// ============================================================================

TEST(TweenSystem_CreateFromCompletionCallbackNoCrash)
{
    // A completion callback that creates a follow-up tween used to invalidate
    // the live map iterator (rehash) mid-walk. This must complete cleanly and
    // run the follow-up on the next frame.
    auto& ts = Spark::TweenSystem::GetInstance();
    ts.Initialize();

    bool followupRan = false;
    Spark::TweenHandle h = ts.Create(0.1f, nullptr);
    if (auto* t = ts.Get(h))
    {
        t->OnComplete([&ts, &followupRan]()
                      { ts.Create(0.1f, [&followupRan](float) { followupRan = true; }); });
    }

    ts.Update(0.2f); // completes h -> callback creates a new tween
    ts.Update(0.2f); // drives the follow-up tween
    EXPECT_TRUE(followupRan);

    ts.Shutdown();
}

TEST(TweenSystem_CancelAllFromCompletionCallbackNoCrash)
{
    // CancelAll() from inside a completion callback clears the map while a
    // TweenInstance is mid-update. The deferred-clear guard must prevent the
    // use-after-free and still drain every tween.
    auto& ts = Spark::TweenSystem::GetInstance();
    ts.Initialize();

    Spark::TweenHandle h1 = ts.Create(0.1f, nullptr);
    ts.Create(0.1f, nullptr); // a second live tween
    if (auto* t = ts.Get(h1))
    {
        t->OnComplete([&ts]() { ts.CancelAll(); });
    }

    ts.Update(0.2f); // completes h1 -> callback CancelAll()s the map
    EXPECT_EQ(ts.GetActiveTweenCount(), 0u);

    ts.Shutdown();
}
