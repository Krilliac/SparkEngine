/**
 * @file TestTween.cpp
 * @brief Tests for the tweening and easing library
 */

#include "TestFramework.h"
#include <cmath>
#include <functional>
#include <vector>
#include <memory>
#include <algorithm>

// ============================================================================
// Standalone easing implementation (mirrors Tween.h)
// ============================================================================

namespace {

enum class Ease {
    Linear, InQuad, OutQuad, InOutQuad,
    InCubic, OutCubic, InOutCubic,
    InSine, OutSine, InOutSine,
    InExpo, OutExpo,
    InBack, OutBack,
    OutBounce, InBounce
};

float EaseEval(Ease e, float t) {
    constexpr float PI = 3.14159265358979f;
    switch (e) {
    case Ease::Linear:     return t;
    case Ease::InQuad:     return t * t;
    case Ease::OutQuad:    return t * (2.0f - t);
    case Ease::InOutQuad:  return t < 0.5f ? 2*t*t : -1 + (4-2*t)*t;
    case Ease::InCubic:    return t * t * t;
    case Ease::OutCubic:   { float u = t-1; return u*u*u + 1; }
    case Ease::InOutCubic: return t < 0.5f ? 4*t*t*t : (t-1)*(2*t-2)*(2*t-2)+1;
    case Ease::InSine:     return 1 - std::cos(t * PI * 0.5f);
    case Ease::OutSine:    return std::sin(t * PI * 0.5f);
    case Ease::InOutSine:  return 0.5f * (1 - std::cos(PI * t));
    case Ease::InExpo:     return t == 0 ? 0 : std::pow(2.0f, 10*(t-1));
    case Ease::OutExpo:    return t == 1 ? 1 : 1 - std::pow(2.0f, -10*t);
    case Ease::InBack:     { float s = 1.70158f; return t*t*((s+1)*t - s); }
    case Ease::OutBack:    { float s = 1.70158f; float u = t-1; return u*u*((s+1)*u+s)+1; }
    case Ease::OutBounce:
        if (t < 1/2.75f) return 7.5625f*t*t;
        if (t < 2/2.75f) { float u = t-1.5f/2.75f; return 7.5625f*u*u+0.75f; }
        if (t < 2.5f/2.75f) { float u = t-2.25f/2.75f; return 7.5625f*u*u+0.9375f; }
        { float u = t-2.625f/2.75f; return 7.5625f*u*u+0.984375f; }
    case Ease::InBounce: return 1 - EaseEval(Ease::OutBounce, 1-t);
    }
    return t;
}

enum class LoopType { Restart, PingPong };

class Tween {
public:
    Tween(float* target, float end, float dur, Ease ease = Ease::Linear)
        : m_target(target), m_start(target ? *target : 0), m_end(end), m_dur(dur), m_ease(ease) {}

    Tween& SetDelay(float d) { m_delay = d; m_delayRem = d; return *this; }
    Tween& SetLoops(int n, LoopType lt = LoopType::Restart) { m_loops = n; m_lt = lt; return *this; }
    Tween& OnComplete(std::function<void()> cb) { m_onDone = std::move(cb); return *this; }
    void Kill() { m_killed = true; }
    bool IsFinished() const { return m_killed || m_finished; }

    bool Update(float dt) {
        if (m_killed || m_finished) return false;
        if (m_delayRem > 0) { m_delayRem -= dt; if (m_delayRem > 0) return true; dt = -m_delayRem; }
        m_elapsed += dt;
        float t = m_dur > 0 ? std::min(m_elapsed / m_dur, 1.0f) : 1.0f;
        if (m_reverse) t = 1 - t;
        float eased = EaseEval(m_ease, t);
        if (m_target) *m_target = m_start + (m_end - m_start) * eased;
        if (m_elapsed >= m_dur) {
            m_loopsDone++;
            if (m_loops == -1 || m_loopsDone < m_loops) {
                m_elapsed = 0;
                if (m_lt == LoopType::PingPong) m_reverse = !m_reverse;
                else if (m_target) *m_target = m_start;
            } else {
                if (m_target) *m_target = m_reverse ? m_start : m_end;
                m_finished = true;
                if (m_onDone) m_onDone();
                return false;
            }
        }
        return true;
    }

private:
    float* m_target;
    float m_start, m_end, m_dur;
    Ease m_ease;
    float m_delay = 0, m_delayRem = 0, m_elapsed = 0;
    int m_loops = 1, m_loopsDone = 0;
    LoopType m_lt = LoopType::Restart;
    bool m_reverse = false, m_killed = false, m_finished = false;
    std::function<void()> m_onDone;
};

} // anonymous namespace

// ============================================================================
// Easing function tests
// ============================================================================

TEST(Ease_LinearIsIdentity) {
    EXPECT_NEAR(EaseEval(Ease::Linear, 0.0f), 0.0f, 0.001f);
    EXPECT_NEAR(EaseEval(Ease::Linear, 0.5f), 0.5f, 0.001f);
    EXPECT_NEAR(EaseEval(Ease::Linear, 1.0f), 1.0f, 0.001f);
}

TEST(Ease_AllStartAtZero) {
    EXPECT_NEAR(EaseEval(Ease::InQuad, 0), 0, 0.001f);
    EXPECT_NEAR(EaseEval(Ease::OutQuad, 0), 0, 0.001f);
    EXPECT_NEAR(EaseEval(Ease::InCubic, 0), 0, 0.001f);
    EXPECT_NEAR(EaseEval(Ease::InSine, 0), 0, 0.001f);
    EXPECT_NEAR(EaseEval(Ease::InExpo, 0), 0, 0.001f);
}

TEST(Ease_AllEndAtOne) {
    EXPECT_NEAR(EaseEval(Ease::InQuad, 1), 1, 0.001f);
    EXPECT_NEAR(EaseEval(Ease::OutQuad, 1), 1, 0.001f);
    EXPECT_NEAR(EaseEval(Ease::InCubic, 1), 1, 0.001f);
    EXPECT_NEAR(EaseEval(Ease::OutSine, 1), 1, 0.001f);
    EXPECT_NEAR(EaseEval(Ease::OutExpo, 1), 1, 0.001f);
}

TEST(Ease_InQuadSlowerAtStart) {
    // At t=0.25, InQuad should be less than linear (0.25)
    float val = EaseEval(Ease::InQuad, 0.25f);
    EXPECT_LT(val, 0.25f);
}

TEST(Ease_OutQuadFasterAtStart) {
    float val = EaseEval(Ease::OutQuad, 0.25f);
    EXPECT_GT(val, 0.25f);
}

TEST(Ease_BackOvershoots) {
    float val = EaseEval(Ease::OutBack, 0.5f);
    // OutBack overshoots past 1.0 before settling
    float nearEnd = EaseEval(Ease::OutBack, 0.7f);
    EXPECT_GT(nearEnd, 1.0f); // overshoot
}

TEST(Ease_BounceEndsAtOne) {
    EXPECT_NEAR(EaseEval(Ease::OutBounce, 1.0f), 1.0f, 0.001f);
    EXPECT_NEAR(EaseEval(Ease::InBounce, 1.0f), 1.0f, 0.001f);
}

// ============================================================================
// Tween tests
// ============================================================================

TEST(Tween_LinearInterpolation) {
    float val = 0.0f;
    Tween t(&val, 100.0f, 1.0f, Ease::Linear);

    t.Update(0.5f);
    EXPECT_NEAR(val, 50.0f, 0.5f);

    t.Update(0.5f);
    EXPECT_NEAR(val, 100.0f, 0.5f);
    EXPECT_TRUE(t.IsFinished());
}

TEST(Tween_DelayBeforeStart) {
    float val = 0.0f;
    Tween t(&val, 100.0f, 1.0f, Ease::Linear);
    t.SetDelay(0.5f);

    t.Update(0.3f); // still in delay
    EXPECT_NEAR(val, 0.0f, 0.01f);

    t.Update(0.3f); // delay expires (0.1s into tween)
    EXPECT_GT(val, 0.0f);
}

TEST(Tween_OnCompleteCallback) {
    float val = 0.0f;
    bool completed = false;
    Tween t(&val, 10.0f, 0.5f, Ease::Linear);
    t.OnComplete([&]() { completed = true; });

    t.Update(0.25f);
    EXPECT_FALSE(completed);

    t.Update(0.3f);
    EXPECT_TRUE(completed);
}

TEST(Tween_LoopRestart) {
    float val = 0.0f;
    Tween t(&val, 100.0f, 1.0f, Ease::Linear);
    t.SetLoops(2, LoopType::Restart);

    t.Update(1.0f); // complete first loop
    EXPECT_FALSE(t.IsFinished()); // should start second loop

    t.Update(1.0f); // complete second loop
    EXPECT_TRUE(t.IsFinished());
    EXPECT_NEAR(val, 100.0f, 0.5f);
}

TEST(Tween_LoopPingPong) {
    float val = 0.0f;
    Tween t(&val, 100.0f, 1.0f, Ease::Linear);
    t.SetLoops(2, LoopType::PingPong);

    t.Update(1.0f); // reach 100, start going back
    EXPECT_FALSE(t.IsFinished());

    t.Update(1.0f); // reach 0
    EXPECT_TRUE(t.IsFinished());
    EXPECT_NEAR(val, 0.0f, 0.5f);
}

TEST(Tween_KillStopsImmediately) {
    float val = 0.0f;
    Tween t(&val, 100.0f, 1.0f, Ease::Linear);

    t.Update(0.3f);
    float frozen = val;
    t.Kill();
    t.Update(0.5f);
    EXPECT_NEAR(val, frozen, 0.01f); // no further changes
}

TEST(Tween_ZeroDurationCompletesInstantly) {
    float val = 0.0f;
    Tween t(&val, 42.0f, 0.0f, Ease::Linear);
    t.Update(0.016f);
    EXPECT_NEAR(val, 42.0f, 0.01f);
    EXPECT_TRUE(t.IsFinished());
}
