// TestCameraInterpolation.cpp - Tests for camera smooth movement interpolation
// Standalone implementations for CI testing (no DirectXMath dependency)

#include "TestFramework.h"
#include <cmath>
#include <algorithm>

namespace TestCameraInterp
{

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        Vec3() = default;
        Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

        float DistanceTo(const Vec3& o) const
        {
            float dx = x - o.x, dy = y - o.y, dz = z - o.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    };

    Vec3 Lerp(const Vec3& a, const Vec3& b, float t)
    {
        return {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)};
    }

    float SmoothStep(float t) { return t * t * (3.0f - 2.0f * t); }

    // ============================================================================
    // Simulated camera with smooth transition
    // ============================================================================

    class TestCamera
    {
      public:
        Vec3 m_position;
        bool m_isTransitioning = false;
        Vec3 m_transitionStart;
        Vec3 m_transitionEnd;
        float m_transitionDuration = 0.0f;
        float m_transitionElapsed = 0.0f;

        TestCamera() = default;
        explicit TestCamera(const Vec3& pos) : m_position(pos) {}

        void SmoothMoveTo(float x, float y, float z, float duration)
        {
            m_transitionStart = m_position;
            m_transitionEnd = {x, y, z};
            m_transitionDuration = (std::max)(duration, 0.001f);
            m_transitionElapsed = 0.0f;
            m_isTransitioning = true;
        }

        void Update(float deltaTime)
        {
            if (m_isTransitioning)
            {
                m_transitionElapsed += deltaTime;
                float t = std::clamp(m_transitionElapsed / m_transitionDuration, 0.0f, 1.0f);
                float smoothT = SmoothStep(t);
                m_position = Lerp(m_transitionStart, m_transitionEnd, smoothT);
                if (t >= 1.0f)
                {
                    m_isTransitioning = false;
                }
            }
        }

        bool IsTransitioning() const { return m_isTransitioning; }
    };

} // namespace TestCameraInterp

// ============================================================================
// Tests
// ============================================================================

TEST(CameraInterp_SmoothMove_StartsAtOrigin)
{
    TestCameraInterp::TestCamera cam({0, 0, 0});
    cam.SmoothMoveTo(10, 20, 30, 1.0f);
    cam.Update(0.0f); // No time passed

    EXPECT_NEAR(cam.m_position.x, 0.0f, 0.01f);
    EXPECT_NEAR(cam.m_position.y, 0.0f, 0.01f);
    EXPECT_NEAR(cam.m_position.z, 0.0f, 0.01f);
}

TEST(CameraInterp_SmoothMove_EndsAtTarget)
{
    TestCameraInterp::TestCamera cam({0, 0, 0});
    cam.SmoothMoveTo(10, 20, 30, 1.0f);
    cam.Update(1.0f); // Full duration

    EXPECT_NEAR(cam.m_position.x, 10.0f, 0.01f);
    EXPECT_NEAR(cam.m_position.y, 20.0f, 0.01f);
    EXPECT_NEAR(cam.m_position.z, 30.0f, 0.01f);
}

TEST(CameraInterp_SmoothMove_MidwayIsInterpolated)
{
    TestCameraInterp::TestCamera cam({0, 0, 0});
    cam.SmoothMoveTo(10, 0, 0, 1.0f);
    cam.Update(0.5f); // Halfway through

    // With SmoothStep, t=0.5 -> smoothT = 0.5*0.5*(3 - 2*0.5) = 0.5
    EXPECT_NEAR(cam.m_position.x, 5.0f, 0.01f);
    EXPECT_TRUE(cam.IsTransitioning());
}

TEST(CameraInterp_SmoothMove_NotAtTarget)
{
    TestCameraInterp::TestCamera cam({0, 0, 0});
    cam.SmoothMoveTo(100, 0, 0, 2.0f);
    cam.Update(0.5f); // Quarter of the way through

    // t = 0.25, smoothT = 0.25^2 * (3 - 0.5) = 0.0625 * 2.5 = 0.15625
    EXPECT_TRUE(cam.m_position.x > 0.0f);
    EXPECT_TRUE(cam.m_position.x < 100.0f);
    EXPECT_TRUE(cam.IsTransitioning());
}

TEST(CameraInterp_SmoothMove_TransitionComplete)
{
    TestCameraInterp::TestCamera cam({0, 0, 0});
    cam.SmoothMoveTo(10, 20, 30, 1.0f);
    cam.Update(1.5f); // Over duration

    EXPECT_FALSE(cam.IsTransitioning());
    EXPECT_NEAR(cam.m_position.x, 10.0f, 0.01f);
    EXPECT_NEAR(cam.m_position.y, 20.0f, 0.01f);
    EXPECT_NEAR(cam.m_position.z, 30.0f, 0.01f);
}

TEST(CameraInterp_SmoothMove_ZeroDuration)
{
    TestCameraInterp::TestCamera cam({0, 0, 0});
    cam.SmoothMoveTo(10, 20, 30, 0.0f);
    cam.Update(0.001f); // Any small step should complete

    EXPECT_FALSE(cam.IsTransitioning());
    EXPECT_NEAR(cam.m_position.x, 10.0f, 0.01f);
    EXPECT_NEAR(cam.m_position.y, 20.0f, 0.01f);
    EXPECT_NEAR(cam.m_position.z, 30.0f, 0.01f);
}

TEST(CameraInterp_SmoothMove_IncrementalUpdate)
{
    TestCameraInterp::TestCamera cam({0, 0, 0});
    cam.SmoothMoveTo(10, 0, 0, 1.0f);

    // Update in small increments (101 steps to ensure we exceed duration
    // despite floating-point accumulation errors)
    for (int i = 0; i < 101; ++i)
    {
        cam.Update(0.01f);
    }

    EXPECT_FALSE(cam.IsTransitioning());
    EXPECT_NEAR(cam.m_position.x, 10.0f, 0.01f);
}

TEST(CameraInterp_SmoothMove_SmoothStepCurve)
{
    // Verify SmoothStep produces ease-in-ease-out (value at t=0.5 should be 0.5)
    float smoothHalf = TestCameraInterp::SmoothStep(0.5f);
    EXPECT_NEAR(smoothHalf, 0.5f, 0.001f);

    // At t=0, should be 0
    float smoothZero = TestCameraInterp::SmoothStep(0.0f);
    EXPECT_NEAR(smoothZero, 0.0f, 0.001f);

    // At t=1, should be 1
    float smoothOne = TestCameraInterp::SmoothStep(1.0f);
    EXPECT_NEAR(smoothOne, 1.0f, 0.001f);

    // Early values should be less than linear
    float smoothQuarter = TestCameraInterp::SmoothStep(0.25f);
    EXPECT_TRUE(smoothQuarter < 0.25f);
}

TEST(CameraInterp_SmoothMove_FromNonOrigin)
{
    TestCameraInterp::TestCamera cam({5, 10, 15});
    cam.SmoothMoveTo(15, 30, 45, 1.0f);
    cam.Update(1.0f);

    EXPECT_NEAR(cam.m_position.x, 15.0f, 0.01f);
    EXPECT_NEAR(cam.m_position.y, 30.0f, 0.01f);
    EXPECT_NEAR(cam.m_position.z, 45.0f, 0.01f);
}
